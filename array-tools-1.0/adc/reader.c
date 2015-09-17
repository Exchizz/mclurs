#

#include "general.h"

#include <stdio.h>
#include <stdlib.h>
#include "assert.h"
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/capability.h>

#include <zmq.h>
#include <pthread.h>

#include <comedi.h>
#include <comedilib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "util.h"
#include "param.h"
#include "queue.h"
#include "strbuf.h"
#include "mman.h"
#include "chunk.h"
#include "adc.h"
#include "snapshot.h"
#include "tidy.h"
#include "reader.h"
#include "writer.h"

/*
 * READER global data structures
 */

public rparams reader_parameters;   /* The externally-visible parameters for the reader thread */
public adc reader_adc;		    /* The ADC object for the READER */

/*
 * READER state machine definitions.
 *
 * The READER state is kept in the rp_state variable, private to the
 * READER thread.
 *
 * ERROR state: this occurs when a serious error happens, normally due
 *   to bad parameters.  One can leave ERROR state using the Param
 *   command.
 *
 * PARAM state: results from initialisation by the main thread routine
 *   and after the receipt of a Param command, because of the activity
 *   of the verify function.  Failure of parameters to verify sends us
 *   to ERROR state.  Successful verification also results in the
 *   creation and parameterisation of an ADC object.
 *
 * RESTING state: a successful execution of the Init command leaves us
 *   in RESTING state.  In this state, an initialised ADC object is
 *   available.  Errors in parameter verfication or instantiation of
 *   the initialised ADC object put us into ERROR state.
 *
 * ARMED state: executing the Go command initiates a data transfer and
 *   moves the READER to this state.  We stay in ARMED state until the
 *   first data has been seen (i.e. the ADC object has changed from
 *   running to running and live).  Failure of data to arrive within a
 *   reasonable time causes an automatic transition to the ERROR
 *   state, with the same cleanup as done by the Halt command, which
 *   may be issued in this or the RUN state.
 *
 * RUN state: automatic transition from ARMED on receipt of the first
 *   data.  In ARMED and RUN state the Halt command will terminate
 *   data acqusition and return the READER to ERROR state (as a
 *   special case; the parameters are valid, but after Halt there is
 *   no ADC object).
 *
 * The Quit command issued in any state will cause the READER to shut
 * down cleanly.
 *
 * The WRITER will reject Snap commands unless the READER is in ARMED
 * or RUN state (in fact, unless the ADC object exists and reports
 * itself as running).
 */

private int rp_state;

#define READER_ERROR	0	/* An error occurred, base start state */
#define	READER_PARAM	1	/* There are parameters that need to be verified */
#define	READER_RESTING	2	/* READER is ready, Comedi and mmap setup has been done */
#define	READER_ARMED	3	/* The ADC has been started */
#define READER_RUN	4	/* Data from the ADC has been seen in the buffers */

/*
 * READER forward definitions
 */

private void drain_reader_chunk_queue();

/*
 * READER thread comms initialisation.
 * Called after the context is created.
 */

private void *writer;
private void *tidy;
private void *log;
private void *command;

private void create_reader_comms() {
  import void *snapshot_zmq_ctx;
  /* Create necessary sockets */
  command  = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_REP, READER_CMD_ADDR);	/* Receive commands */
  assertv(command != NULL, "Failed to instantiate reader command socket\n");
  log      = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_PUSH, LOG_SOCKET);  /* Socket for log messages */
  assertv(log != NULL, "Failed to instantiate reader log socket\n");
  writer = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, READER_QUEUE_ADDR);
  assertv(writer != NULL, "Failed to instantiate reader queue socket\n");
  tidy     = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, TIDY_SOCKET);  /* Socket to TIDY thread */
  assertv(tidy != NULL, "Failed to instantiate reader->tidy socket\n");
}

/* Close everything created above. */

private void close_reader_comms() {
  zmq_close(command);
  zmq_close(log);
  zmq_close(writer);
  zmq_close(tidy);
}

/*
 * Copy the necessary capabilities from permitted to effective set (failure is fatal).
 *
 * The READER needs:
 *
 * CAP_IPC_LOCK -- ability to mmap and mlock pages.
 * CAP_SYS_NICE -- ability to set RT scheduling priorities
 *
 * These capabilities should be in the CAP_PERMITTED set, but not in CAP_EFFECTIVE which was cleared
 * when the main thread dropped privileges by changing to the desired non-root uid/gid.
 */

private int set_up_reader_capability() {
  cap_t c = cap_get_proc();
  const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, };

  cap_set_flag(c, CAP_EFFECTIVE, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
  return cap_set_proc(c);
}

/*
 * Get a value from the monotonic krnel clock and express in nanoseconds.
 */

public uint64_t  monotonic_ns_clock() {
  uint64_t ret;
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);		/* Timestamp for debugging */
  ret = now.tv_sec;
  ret = ret*1000000000 + now.tv_nsec;
  return ret;
}

/*
 * Process a READER command from MAIN thread.  Generate replies as necessary.
 * Returns true if processing messages should continue..
 */

private int process_reader_command(void *s) {
  rparams *rp = &reader_parameters;
  int      used;
  int      ret;
  strbuf   cmd;
  char    *cmd_buf;
  strbuf   err;

  used = zh_get_msg(s, 0, sizeof(strbuf), &cmd);
  if( !used ) {			/* It was a quit message */
    if(rp_state == READER_ARMED || rp_state == READER_RUN || rp_state == READER_RESTING)
      adc_stop_data_transfer(reader_adc);
    return false;
  }

  cmd_buf = strbuf_string(cmd);
  err = strbuf_next(cmd);

  if(verbose > 1)
    zh_put_multi(log, 3, "READER cmd: '", &cmd_buf[0], "'");

  ret = 0;
  switch(cmd_buf[0]) {
  case 'p':
  case 'P':
    if( rp_state != READER_PARAM && rp_state != READER_RESTING && rp_state != READER_ERROR ) {
      strbuf_printf(err, "NO: Param issued but not in PARAM, RESTING or ERROR state");
      ret = -1;
      break;
    }
    ret = set_params_from_string(&cmd_buf[0], globals, n_global_params);
    if( ret < 0 ) { 
      strbuf_printf(err, "NO: Param -- parse error at position %d", -ret);
      break;
    }
    ret = assign_cmd_params(globals, n_global_params);
    if( ret < 0 ) { 
      strbuf_printf(err, "NO: Param -- assign error on param %d: %m", -ret);
      break;
    }

    /* Otherwise, succeeded in updating parameters */
    strbuf_printf(err, "NO: Param -- verify error: ");
    ret = verify_reader_params(&reader_parameters, err);
    if( ret < 0 ) { 
      break;
    }
    strbuf_printf(err, "OK Param");
    rp_state = READER_PARAM;
    break;

  case 'i':
  case 'I':
    if( rp_state != READER_PARAM ) {
      strbuf_printf(err, "NO: Init issued but not in PARAM state");
      ret = -1;
      break;
    }
    strbuf_printf(err, "NO: Init -- param verify error: ");
    ret = verify_reader_params(&reader_parameters, err);
    if( ret < 0 ) {
      rp_state = READER_ERROR;
      break;
    }
    ret = adc_init(reader_adc, err);
    if( ret < 0 ) {
      rp_state = READER_ERROR;
      break;
    }
    if(verbose > 0) {		/* Borrow the err buffer */
      strbuf_printf(err, "READER Init with dev %s, freq %g [Hz], isp %d [ns] and buf %d [MiB]",
		    rp->r_device, rp->r_frequency, adc_ns_per_sample(reader_adc), rp->r_bufsz);
      zh_put_multi(log, 1, strbuf_string(err)); 
    }
    strbuf_printf(err, "OK Init -- nchan %d isp %d [ns]", NCHANNELS, adc_ns_per_sample(reader_adc));
    rp_state = READER_RESTING;
    break;

  case 'g':
  case 'G':
    if( rp_state != READER_RESTING ) {
      strbuf_printf(err, "NO: Go issued but not in RESTING state");
      ret = -1;
      break;
    }
    ret = adc_start_data_transfer(reader_adc, err);
    if( ret < 0 ) {
      rp_state = READER_ERROR;
      break;
    }
    strbuf_printf(err, "OK Go");
    rp_state = READER_ARMED;
    break;

  case 'h':
  case 'H':
    if( rp_state != READER_ARMED && rp_state != READER_RUN ) {
      strbuf_printf(err, "NO: Halt issued but not in ARMED or RUN state");
      ret = -1;
      break;
    }
    adc_stop_data_transfer(reader_adc);	/* Terminate any transfer in progress */
    drain_reader_chunk_queue();		/* Empty the chunk queue */
    strbuf_printf(err, "OK Halt");
    adc_destroy(reader_adc);
    reader_adc = NULL;
    rp_state = READER_ERROR;
    break;

  default:
    strbuf_printf(err, "NO: READER -- Unexpected reader command");
    ret = -1;
    break;
  }
  if( ret < 0 ) {
    strbuf_revert(cmd);
    zh_put_multi(log, 4, strbuf_string(err), "\n > '", &cmd_buf[0], "'"); /* Error occurred, log it */
  }
  strbuf_clear(cmd);
  zh_put_msg(s, 0, sizeof(strbuf), (void *)&err); /* return message */
  return true;
}

/*
 * Set the READER thread to real-time priority, if RTPRIO is set...
 */

public int set_reader_rt_scheduling() {

  if( reader_parameters.r_schedprio > 0 ) {	/* Then there is RT priority scheduling to set up */
    if( set_rt_scheduling(reader_parameters.r_schedprio) < 0 )
      return -1;

    /* Successfully applied RT scheduling */
    return 1;
  }

  /* RT scheduling not applicable:  no RTPRIO set */
  return 0;
}


/*
 * Handle a message from the WRITER.  The message will be a chunk
 * which is ready to add to the READER's pending-work queue.  Chunks
 * arrive here with a state of SNAPSHOT_WAITING or SNAPSHOT_ERROR (if
 * they were in transit when an error occurred).  The latter are sent
 * straight back to the WRITER, which is counting down pending chunks
 * to file completion, after their frame has been released.
 */

private QUEUE_HEADER(ReaderChunkQ);

private int process_queue_message(void *s) {
  rparams *rp = &reader_parameters;
  chunk_t *c;
  int      ret;

  ret = zh_get_msg(s, 0, sizeof(chunk_t *), (void *)&c);
  assertv(ret==sizeof(chunk_t *), "Received message from WRITER with wrong size %d (not %d)\n", ret, sizeof(chunk_t *));

  if(rp_state != READER_ARMED && rp_state != READER_RUN) {
    strbuf_appendf(c->c_error, "READER thread ADC is not running");
    c->c_status = SNAPSHOT_ERROR;
  }
  else {  /* Check the chunk is still current -- set SNAPSHOT_ERROR state on failure */
    adc_setup_chunk(reader_adc, c);
  }
  
  if(c->c_status==SNAPSHOT_ERROR) {		/* we send it straight back */
    ret = zh_put_msg(writer, 0, sizeof(chunk_t *), (void *)&c);
    assertv(ret==sizeof(chunk_t *), "Message returned to WRITER with wrong size %d (not %d)\n", ret, sizeof(chunk_t *));
    ret = zh_put_msg(tidy, 0, sizeof(frame *), &c->c_frame);
    assertv(ret==sizeof(frame *), "Frane message to TIDY with wrong size %d (not %d)\n", ret, sizeof(frame *));
    c->c_frame = NULL;
    return true;
  }

  assertv(c->c_status==SNAPSHOT_WAITING, "Received chunk c:%04hx has unexpected state %s\n", c->c_name, snapshot_status(c->c_status));

  /* Add the chunk to the READER chunk queue */
  queue *pos = &ReaderChunkQ;
  if( !queue_singleton(&ReaderChunkQ) ) {
    for_nxt_in_Q(queue *p, queue_next(&ReaderChunkQ), &ReaderChunkQ);
    chunk_t *h = rq2chunk(p);
    if(h->c_first > c->c_first) {
      pos = p;
      break;
    }
    end_for_nxt;
  }
  queue_ins_before(pos, chunk2rq(c));
  return true;
}

/*
 * Abort the chunk which is at the head of the ReaderChunkQ, i.e. it is
 * queue_next(&ReaderChunkQ).  This means we must scan for its
 * siblings in the queue, remove them and set their status to
 * SNAPSHOT_ERROR, and return them to the WRITER.  We assume that the
 * caller has set the c_error strbuf.
 */

private void abort_queue_head_chunk() {
  snapfile_t *parent = rq2chunk(queue_next(&ReaderChunkQ))->c_parent;
  int         ret;
  
  for_nxt_in_Q(queue *p, queue_next(&ReaderChunkQ), &ReaderChunkQ);
  chunk_t *c = rq2chunk(p);
  if(c->c_parent == parent) {
    de_queue(p);
    c->c_status = SNAPSHOT_ERROR;
    ret = zh_put_msg(writer, 0, sizeof(chunk_t *), (void *)&c);
    assertv(ret==sizeof(chunk_t *), "Abort to WRITER with wrong size %d (not %d)\n", ret, sizeof(chunk_t *));
    ret = zh_put_msg(tidy, 0, sizeof(frame *), &c->c_frame);
    assertv(ret==sizeof(frame *), "Frane message to TIDY with wrong size %d (not %d)\n", ret, sizeof(frame *));
    c->c_frame = NULL;
  }
  end_for_nxt;
}

/*
 * Drain the READER chunk queue when turning off the data capture.
 * Any snapshots in progress are aborted.
 */

private void drain_reader_chunk_queue() {

  while( !queue_singleton(&ReaderChunkQ) ) {
    chunk_t *c = rq2chunk(queue_next(&ReaderChunkQ));
    strbuf_appendf(c->c_error, "aborted because of READER ADC shutdown");
    abort_queue_head_chunk();
  }
}

/*
 * READER thread message loop
 */

private void reader_thread_msg_loop() {    /* Read and process messages */
  int ret;
  int running;
  int poll_delay = -1;

  /* Main loop:  read messages and process messages */
  zmq_pollitem_t  poll_list[] =
    { { writer,  0, ZMQ_POLLIN, 0 },
      { command, 0, ZMQ_POLLIN, 0 },
    };
#define	N_POLL_ITEMS	(sizeof(poll_list)/sizeof(zmq_pollitem_t))
  int (*poll_responders[N_POLL_ITEMS])(void *) =
    { process_queue_message,
      process_reader_command,
    };

  zh_put_multi(log, 1, "READER thread is initialised");
  rp_state = READER_PARAM;
  reader_parameters.r_running = true;
  running = true;

  /* CURRENT CODE FOR SETTING THE DATA START TIME WILL BE LATE ... ? */

  while( running && !die_die_die_now ) {
    int ret; 
    int nb;
    int delay = 10;
    int n;

#if 0
    if( reader.adc_run ) {		/* If ADC is running, process data  */

      /* Process any items in the write queue that are now ready -- do this before reading new data */
      queue *n = adc.write_queue.q_next;
      if( n != &adc.write_queue ) {	/* The write queue is not empty */
	snapr *s = (snapr *) n;

	if( s->count && adc.tail > s->last ) { /* There is a write queue item ready to process */
	  process_ready_write_queue_item(s);
	  delay = 0;			       /* If we wrote a file, don't wait in the poll */
	}
      }

      /* Now, retrieve any new data if possible */
      nb = comedi_get_buffer_contents(adc.device, 0);
      if(nb) {
	int ns = nb / sizeof(sampl_t);

	if( rp_state != READER_RUN ) { /* First data has arrived */
	  struct timespec start_stamp;

	  clock_gettime(CLOCK_MONOTONIC, &start_stamp);
	  compute_data_start_timestamp(&start_stamp, ns);
	  rp_state = READER_RUN;
	}

	/* NEED TO WORRY HERE ABOUT DATA THAT IS BEING FLUSHED TO FILE */
	/* WHICH MAY MEAN SOME OF THE BUFFER SHOULD NOT BE OVERWRITTEN */

	sampl_t *in  = &adc.comedi_buffer[ adc.tail % adc.buffer_samples ]; /* This is the live data start point */
	sampl_t *out = &((sampl_t *)adc.ring_buf->rb_start)[ adc.tail % adc.ring_buf->rb_samples ];

	adc.head += ns;			/* This many new samples have arrived in the Comedi buffer */
	(*adc.convert)(out, in, ns);    /* Copy the data from in to out with LUT conversion */
	adc.tail += ns;			/* Processed this many new samples */
	ret = comedi_mark_buffer_read(adc.device, 0, nb);
	assertv(ret == nb, "Comedi mark_buffer_read returned %d instead of %d\n", ret, nb);
      }
    }

#endif

    ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, delay);	/* Look for commands here */
    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      zh_put_multi(log, 1, "READER loop interrupted");
      break;
    }
    if(ret < 0)
      break;

    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
	running = running & (*poll_responders[n])(poll_list[n].socket); /* N.B. not && */
      }
    }
  }
}

/*
 * READER thread main routine
 *
 * This loop either waits for a command on the command socket, or
 * loops reading from Comedi.  It aborts if it cannot get the sockets
 * it needs.
 */

public void *reader_main(void *arg) {
  int ret;
  char *thread_msg = "normal exit";

  create_reader_comms();
  
  if( set_up_reader_capability() < 0 ) {
    zh_put_multi(log, 1, "READER thread capabilities are deficient");
  }

  ret = set_reader_rt_scheduling();
  switch(ret) {
  case 1:
    zh_put_multi(log, 1, "READER RT scheduling succeeded");
    break;
  case 0:
    zh_put_multi(log, 1, "READER using normal scheduling: RTPRIO unset");
    break;
  default:
    zh_put_multi(log, 2, "READER RT scheduling setup failed: ", strerror(errno));
    break;
  }

  struct timespec test_stamp;
  ret = clock_gettime(CLOCK_MONOTONIC, &test_stamp);
  assertv(ret == 0, "Test failed to get monotonic clock time\n");  

  reader_thread_msg_loop();
  if(rp_state == READER_ARMED || rp_state == READER_RUN || rp_state == READER_RESTING) {
    adc_stop_data_transfer(reader_adc);
    adc_destroy(reader_adc);
  }

  zh_put_msg(tidy, 0, 0, NULL);	/* Tell TIDY thread to finish */

  zh_put_multi(log, 1, "READER thread terminates by return");

  /* Clean up our ZeroMQ sockets */
  close_reader_comms();
  reader_parameters.r_running = false;
  return (void *) thread_msg;
}

/*
 * Verify reader parameters and generate reader state description.
 */ 

public int verify_reader_params(rparams *rp, strbuf e) {

  if( rp->r_schedprio != 0 ) { /* Check for illegal value */
    int max, min;

    min = sched_get_priority_min(SCHED_FIFO);
    max = sched_get_priority_max(SCHED_FIFO);
    if(rp->r_schedprio < min || rp->r_schedprio > max) {
      strbuf_appendf(e, "RT scheduling priority %d not in kernel's acceptable range [%d,%d]",
		    rp->r_schedprio, min, max);
      return -1;
    }
  }

  if(reader_adc) {
    adc_destroy(reader_adc);
    reader_adc = NULL;
  }
    reader_adc = adc_new(rp->r_device, e);
  
  if( adc_set_chan_frequency(reader_adc, e, &rp->r_frequency) < 0 )
    return -1;
  
  if(rp->r_window < 1 || rp->r_window > 30) {
    strbuf_appendf(e, "Capture window %d seconds outwith compiled-in range [%d,%d] seconds",
		   rp->r_window, 1, 30);
    return -1;
  }

  if( adc_set_bufsz(reader_adc, e, rp->r_bufsz) < 0 )
    return -1;
  
  if( adc_set_range(reader_adc, e, rp->r_range) < 0 )
    return -1;
  
  rp_state = READER_PARAM;
  return 0;
}
