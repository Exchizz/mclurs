#

#include <stdio.h>
#include <stdlib.h>
#include "assert.h"
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

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
#include "ring.h"
#include "lut.h"
#include "snapshot.h"
#include "reader.h"
#include "writer.h"

/*
 * Reader local types and structures
 */

#define N_USBDUX_CHANS	16	/* Number of channels in USBDUX_FAST */

#define USBDUXFAST_COMEDI_500mV	1 /* Bit 3 control output is 0 iff the CR_RANGE is one */
#define USBDUXFAST_COMEDI_750mV	0 /* Bit 3 control output is 1 iff the CR_RANGE is zero */

rparams reader_parameters;	/* The externally-visible parameters for the reader thread */

struct comedi_state
{ comedi_t  *device;		/* The Comedi device handle */
  int	     devflags;		/* Comedi device flags */
  comedi_cmd command;		/* The command to send to the device */
  int	     fd;		/* The device file descriptor for reading data */
  int	     sample_ns;		/* The ADC inter-sample interval [ns] */
  int	     poll_adc_interval;	/* Interval to wait when ADC is running [ms] */
  unsigned   c[N_USBDUX_CHANS]; /* Command chennel list */
  sampl_t   *comedi_buffer;	/* The memory-mapped Comedi streaming buffer  */
  uint64_t   buffer_length;	/* The size of the Comedi streaming buffer [bytes] */
  uint64_t   buffer_samples;	/* The buffer size in samples */
  uint64_t   head,		/* The current sample number at the front of the Comedi buffer */
	     tail;		/* The current last sample number processed in the buffer */
  struct readbuf *ring_buf;	/* Ring buffer handle for Comedi transfer */
  int	     adc_range;		/* The ADC full-scale range: 500 for 500mV and 750 for 750mV */
  void	   (*convert)(sampl_t *, sampl_t *, int); /* LUT conversion function */
  queue	     write_queue;	/* The queue header for the queue of snapshots */
};

struct reader_state
{ int	      rtprio;		/* Thread RT priority */
  const char *comedi_device;	/* The Comedi device to use */
  unsigned    bufsz;		/* Comedi streaming buffer target size [pages] */
  unsigned    ringsz;		/* Ring buffer target size [pages] */
  int	      sample_ns;	/* The ADC inter-sample interval [ns] */
  int	      adc_run;		/* Is the ADC capture running? */
  int	      state;		/* The state of the reader (READER_PARAM, READER_RESTING, READER_RUN, ...) */
  int	      stoploop;		/* Reader main loop runs when this is false */
  int	      poll_delay;	/* Delay interval during main reactor loop [ms] */
  int         permu;		/* Millionths of the ADC buffer represented by the poll_delay */
  void	     *command;		/* Command socket */
  void	     *position;		/* Position reporting socket */
  void	     *rd_log;		/* Reader logging socket */
};

/*
 * Reader state definitions 
 *
 * The reader starts in PARAM state and is subsequently initialised by
 * the main thread to RESTING state.  This means that all parameters
 * have been verified and the various buffer sizes and such computed
 * and allocated.  The 'Param' command changes parameter values and
 * resets the state to PARAM.
 *
 * Param (space Name=value) [can be multi-frame]
 *   Name is a parameter name from the parameter descriptor array, value is a suitable value.
 * Quit
 *
 * Initialisation (RESTING) is managed in PARAM state using command
 * 'Ready'; the ring buffer is allocated and mapped, Comedi command is
 * checked and initialised.  Current buffers are released and new ones
 * allocated.  The verify_reader_params() routine handles all this.
 *
 * Ready
 * Quit
 *
 * In RESTING state, the 'Go' command initiates the ADC transfer process.
 * The state changes to ARMED if successful, otherwise RESTING (with
 * error messages).
 *
 * Go
 * Quit
 *
 * In ARMED state, actually seeing data from the ADC changes to RUN
 * state.  'Stop' command stops acquisition and changes back to
 * RESTING, 'Quit' terminates the thread.  No visible data within the
 * timeout window is equivalent to an 'Stop' but with an error code
 * sent via the pos channel.
 *
 * Stop
 * Quit
 */

#define READER_ERROR	0	/* An error occurred, base start state */
#define	READER_PARAM	1	/* There are parameters that need to be verified */
#define	READER_RESTING	2	/* Reader is ready, Comedi and mmap setup has been done */
#define	READER_ARMED	3	/* The ADC has been started */
#define READER_RUN	4	/* Data from the ADC has been seen in the buffers */

/*
 * Reader internal state variables
 */

static struct reader_state	reader; /* Reader state + parameters */
static struct comedi_state	adc;	/* Comedi state + parameters */

/*
 * Reader thread comms initialisation.
 *
 * Called after the context is created.
 */

static void *wr_queue_reader;
static void *log;
static void *command;

static int create_reader_comms() {
  /* Create necessary sockets */
  command  = zh_bind_new_socket(zmq_main_ctx, ZMQ_REP, READER_CMD_ADDR);	/* Receive commands */
  assertv(command != NULL, "Failed to instantiate reader command socket\n");
  log      = zh_connect_new_socket(zmq_main_ctx, ZMQ_PUSH, LOG_SOCKET);  /* Socket for log messages */
  assertv(log != NULL, "Failed to instantiate reader log socket\n");
  wr_queue_reader = zh_bind_new_socket(zmq_main_ctx, ZMQ_PAIR, READER_QUEUE_ADDR);
  assertv(wr_queue_reader != NULL, "Failed to instantiate reader queue socket\n");
}

/*
 * Resource usage debugging
 */

void print_rusage() {
  struct rusage usage;

  getrusage(RUSAGE_SELF, &usage);
  fprintf(stderr, "Reader: maj %ld min %ld swap %d vsw %d isw %d\n",
	  usage.ru_majflt, usage.ru_minflt, usage.ru_nswap, usage.ru_nvcsw, usage.ru_nivcsw
	  );
}

/*
 * Create and initialise Comedi channel and command
 *
 * ASSUMES that reader structure comedi_device, bufsz, ringsz,
 * sample_ns, tmpdir are all set up correctly from the parameters.
 *
 * COMPLETES all necessary values in the adc structure.
 *
 * RETURNS zero for success, < 0 for failure (value is failed step number)
 */

static int comedi_transfer_initialise() {
  int         i, ret, range;
  uint64_t    buf_window;
  comedi_cmd *cmd;
  void       *map;

  bzero(&adc, sizeof(adc));	/* Initialise the ADC state structure */
  init_queue(&adc.write_queue);
  cmd = &adc.command;
  adc.device = comedi_open(reader.comedi_device);
  if( !adc.device )
    return -1;

  adc.devflags = comedi_get_subdevice_flags(adc.device, 0);
  adc.fd = comedi_fileno(adc.device);

  /* Initialise Comedi */
  if( !adc.buffer_length ) {
    int request = reader.bufsz * 1024 * 1024;

    ret = comedi_get_max_buffer_size(adc.device, 0);  
    if( request > ret ) {
      ret = comedi_set_max_buffer_size(adc.device, 0, request);
      if( ret < 0 )
	return -1;
    }
    ret = comedi_get_buffer_size(adc.device, 0);
    if( request > ret ) {
      ret = comedi_set_buffer_size(adc.device, 0, request);
      if( ret < 0 )
	return -2;
    }
  }

  adc.buffer_length = comedi_get_buffer_size(adc.device, 0);
  adc.buffer_samples = adc.buffer_length / sizeof(sampl_t);
  comedi_set_global_oor_behavior(COMEDI_OOR_NUMBER);

  //  fprintf(stderr, "Init step 2 complete:  buf length = %u\n", adc.buffer_length);

  /* Initialise the command structure */
  ret = comedi_get_cmd_generic_timed(adc.device, 0, cmd, N_USBDUX_CHANS, 0);
  assertv(ret == 0, "Unable to initialise a Comedi command structure: %s\n", comedi_strerror(comedi_errno()));

  /* Inter-channel sample period [ns] */
  adc.sample_ns = reader.sample_ns;
  //  fprintf(stderr, "Init check: inter-sample period = %d\n", adc.sample_ns);

  /* Set up the conversion function:  500mV or 750mV FSD */
  switch(adc.adc_range) {

  case 1:
  case 500:			/* Narrow FSD range */
    adc.convert = convert_raw_500mV;
    range = USBDUXFAST_COMEDI_500mV;
    break;

  case 0:
  case 750:			/* Wide FSD range */
    adc.convert = convert_raw_750mV;
    range = USBDUXFAST_COMEDI_750mV;
    break;

  default:
    return -3;
  }

  /* Set the command parameters from the reader parameter values */
  for(i=0; i<N_USBDUX_CHANS; i++)
    adc.c[i] = CR_PACK_FLAGS(i, range, AREF_GROUND, 0);
  cmd->chanlist    = &adc.c[0];
  cmd->stop_src    = TRIG_NONE;
  cmd->stop_arg    = 0;
  cmd->convert_arg = adc.sample_ns;

  /* Ask the driver to check the command structure and complete any omissions */
  (void) comedi_command_test(adc.device, cmd);
  ret = comedi_command_test(adc.device, cmd);
  if( ret < 0 )
    return -3;

  //  fprintf(stderr, "Init step 3 complete\n");

  /* Map the Comedi buffer into memory, twice */
  map = mmap(NULL, 2*adc.buffer_length, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0); /* Locate a suitable address */
  if( map == NULL )
    return -4;
  map = mmap(map, adc.buffer_length, PROT_READ, MAP_SHARED|MAP_FIXED|MAP_LOCKED, adc.fd, 0);
  if( map == NULL )
    return -4;
  adc.comedi_buffer = map;
  if( mmap( map+adc.buffer_length, adc.buffer_length, PROT_READ, MAP_SHARED|MAP_FIXED|MAP_LOCKED, adc.fd, 0) == NULL )
    return -4;

  //  fprintf(stderr, "Init step 4 complete: map=%p\n", map);

  /* Touch each page -- read only */
  prefault_pages(adc.comedi_buffer, 2*adc.buffer_length / sysconf(_SC_PAGESIZE), PREFAULT_RDONLY);
  //   fprintf(stderr, "Init check:  buf pages = %u\n", i);

  adc.head = 0;
  adc.tail = 0;

  //  fprintf(stderr, "Init step 5 complete\n");

  adc.ring_buf = create_ring_buffer(reader.ringsz, tmpdir_path);
  if( !adc.ring_buf )
    return -6;

  //  fprintf(stderr, "Init step 6 complete: ringsz=%u pages (%u bytes)\n", reader.ringsz, adc.ring_buf->rb_size);

  /* Calculate poll delay:  want max PERMU/1000000 of the Comedi buffer with new data per cycle */
  i = reader.permu;
  //  fprintf(stderr, "Init check:  permu = %d\n", i);

  buf_window = adc.buffer_samples;	 /* Number of samples in buffer  */
  //  fprintf(stderr, "Init check:  buf samples = %Lu\n", buf_window);

  buf_window *= adc.sample_ns; 		/* Time required for a buffer-full of data [ns] */
  //  fprintf(stderr, "Init check:  buf time[ns] = %Lu\n", buf_window);

  buf_window *= i;
  buf_window /= 1000000;      		/* Time required for adjusted ADC_XFER_PERMIL of buffer [ns] */
  //  fprintf(stderr, "Init check:  buf permu[ns] = %Lu\n", buf_window);

  if(buf_window < 1000000)     /* Want at least 1 ms pause */
    buf_window = 1000000;

  adc.poll_adc_interval = buf_window / 1000000; /* Convert to [ms] */
  //  fprintf(stderr, "Init step 7 complete:  buf window=%Lu poll interval=%d\n", buf_window, adc.poll_adc_interval);

  reader.state = READER_RESTING;
  return 0;
}

/*
 * Execute the Comedi command, starting the asynchronous data transfer.
 *
 * ASSUMES adc structure device and command are filled out.  Updates reader state.
 *
 * RETURNS 0 for normal completion, <0 for errors.
 */

static int comedi_start_data_transfer() {
  int   ret;

  if( reader.state != READER_RESTING ) {
    errno = ENOTSUP;
    return -1;
  }

  /* Execute the command to initiate data acquisition */
  ret = comedi_command(adc.device, &adc.command);
  if( ret < 0 )
    return -2;

  /* Loop reading data into the ring buffer */
  reader.state = READER_ARMED;
  reader.poll_delay = adc.poll_adc_interval; /* Calculated on data transfer rate */
  reader.adc_run = 1;
  return 0;
}

/*
 * Stop an on-going data transfer and release resources.
 *
 * INVALIDATES the content of the adc structure, MAINTAINS consistent
 * reader structure state.
 */

static int comedi_stop_data_transfer() {
  /* Tell Comedi to stop, if necessary */
  if( reader.adc_run )
    comedi_cancel(adc.device, 0);

  /* Asynchronous loop completed, free all resources */
  munlock(adc.comedi_buffer, 2*adc.buffer_length);
  munmap(adc.comedi_buffer, 2*adc.buffer_length);
  close(adc.fd);
  comedi_close(adc.device);
  destroy_ring_buffer(adc.ring_buf);
  reader_parameters.r_inter_sample_ns = 0;
  reader_parameters.r_capture_start_time = 0;
  reader.state = READER_PARAM;
  reader.adc_run = 0;
  reader.poll_delay = -1;	/* No data expected */
  return 0;
}

/*
 * Compute start time of data stream when data first arrives.
 *
 * The data began ns samples before the timestamp in ts (more-or-less).
 */

static void compute_data_start_timestamp(struct timespec *ts, int ns) {
  uint64_t timestamp_ns;
  long	   delay = ns*adc.sample_ns;

  timestamp_ns = ts->tv_sec;
  timestamp_ns = timestamp_ns * 1000000000 + ts->tv_nsec;
  timestamp_ns -= delay;
  reader_parameters.r_inter_sample_ns = adc.sample_ns;
  reader_parameters.r_capture_start_time = timestamp_ns;
} 

/*
 * Process a reader command from main thread.  Generate replies as necessary.
 */

#define	COMMAND_BUFSIZE		1024
char command_buffer[COMMAND_BUFSIZE];

static void process_reader_command(void *s) {
  int     used;
  int     ret;
  char   *p;
  strbuf  cmd;
  char   *cmd_buf;
  strbuf  err;

  used = zh_get_msg(s, 0, sizeof(strbuf), &cmd);
  if( !used ) {			/* It was a quit message */
    if(reader.state == READER_ARMED || reader.state == READER_RUN || reader.state == READER_RESTING)
      comedi_stop_data_transfer();
    reader.stoploop = true;	/* No more waiting in the main loop */
    return;
  }

  cmd_buf = strbuf_string(cmd);
  err = strbuf_next(cmd);

  if(debug_level > 2)
    zh_put_multi(log, 3, "Reader cmd: '", &command_buffer[0], "'");
  switch(cmd_buf[0]) {
  case 'h':
  case 'H':
    if( reader.state != READER_ARMED && reader.state != READER_RUN ) {
      strbuf_printf(err, "NO: Halt issued but not in ARMED or RUN state");
      break;
    }
    comedi_stop_data_transfer();
    strbuf_printf(err, "OK Halt");
    break;

  case 'p':
  case 'P':
    if( reader.state != READER_PARAM && reader.state != READER_RESTING && reader.state != READER_ERROR ) {
      strbuf_printf(err, "NO: Param issued but not in PARAM, RESTING or ERROR state");
      break;
    }
    p=&cmd_buf[1];
    while( *p && !isspace(*p) ) p++; /* Are there parameters to process? */
    while( *p && isspace(*p) ) p++;
    if( *p ) {			       /* Assume yes if there is more string */
      int ret = push_params_from_string(p, globals, n_global_params);
      if( ret < 0 ) { 
	strbuf_printf(err, "NO: Param -- error at step %d: %m", -ret);
	break;
      }
      /* Otherwise, succeeded in updating parameters */
      strbuf_printf(err, "NO: Param -- verify error: ");
      ret = verify_reader_params(&reader_parameters, err);
      if( ret < 0 ) { 
        break;
      }
    }
    strbuf_printf(err, "OK Param");
    reader.state = READER_PARAM;
    if(debug_level > 1)
      debug_params(stderr, globals, n_global_params);
    return;

  case 'i':
  case 'I':
    if( reader.state != READER_PARAM ) {
      strbuf_printf(err, "NO: Init issued but not in PARAM state");
      break;
    }
    strbuf_printf(err, "NO: Init -- param verify error: ");
    ret = verify_reader_params(&reader_parameters, err);
    if( ret < 0 ) {
      reader.state = READER_ERROR;
      break;
    }
    ret = comedi_transfer_initialise();
    if( ret < 0 ) {
      reader.state = READER_ERROR;
      strbuf_printf(err, "NO: Init -- error at step %d: %m", -ret);
      break;
    }
    strbuf_printf(err, "OK Init");
    break;

  case 'g':
  case 'G':
    if( reader.state != READER_RESTING ) {
      strbuf_printf(err, "NO: Go issued but not in RESTING state");
      break;
    }
    ret = comedi_start_data_transfer();
    if( ret < 0 ) {
      reader.state = READER_ERROR;
      strbuf_printf(err, "NO: Go error at step %d: %m", -ret);
      break;
    }      
    strbuf_printf(err, "OK Go");
    break;

  default:
    strbuf_printf(err, "NO: Reader -- Unexpected reader command");
    break;
  }
  zh_put_multi(log, 3, strbuf_string(err), "\n   ", &cmd_buf[0]); /* Error occurred, log it */
  zh_put_msg(s, 0, sizeof(strbuf), (void *)&err); /* return message */
}

/*
 * Set the reader thread to real-time priority, if RTPRIO is set...
 */

int set_reader_rt_scheduling() {

  if( reader.rtprio > 0 ) {	/* Then there is RT priority scheduling to set up */
    if( set_rt_scheduling(reader.rtprio) < 0 )
      return -1;

    /* Successfully applied RT scheduling */
    return 1;
  }

  /* RT scheduling not applicable:  no RTPRIO set */
  return 0;
}

/*
 * Manage the write queue:  check the requested snapshot for acceptability.
 * Returns 1 for success, 0 for failure.
 *
 * adc.head is the current latest sample;
 * adc.head - adc.ring_buf->rb_samples is the oldest current sample.
 *
 * Note that adc.head and first are both uint64_t, so comparison will be unsigned...
 *
 * The request is valid if the start sample is futureward of the
 * oldest current sample and the requested size is smaller than the
 * ring buffer.
 *
 * Because we allow start to be at the current oldest, we must process
 * queued blocks before reading new data.
 */

static int check_snapshot_timing(snapr *r) {
  if(r->samples > adc.ring_buf->rb_samples ||
     r->first + adc.ring_buf->rb_samples < adc.head)
    return false;
  /* Snapshot is valid;  compute the start address for the wanted data */
  r->start = &((sampl_t *)adc.ring_buf->rb_start)[r->first % adc.ring_buf->rb_samples];
  return true;
}

/*
 * Manage the write queue:  deal with ready item -- write, dequeue and signal writer.
 *
 * All necessary state is included in the snapr structure.  Copy the
 * ring buffer pages to the memory-mapped file buffer.  Then hand the
 * structure back to the Writer.
 */

static void process_ready_write_queue_item(snapr *r) {
  int ret;

  de_queue(&r->Q);					/* Remove from wait queue */
  clock_gettime(CLOCK_MONOTONIC, &r->ready);		/* Timestamp for debugging */
  memcpy(r->mmap, r->start, r->bytes);			/* Copy the data */
  clock_gettime(CLOCK_MONOTONIC, &r->written);;		/* Timestamp for debugging */
  r->rd_state = SNAPSHOT_WRITTEN;
  r->count--;						/* Completed 1 snapshot */
  ret = zh_put_msg(wr_queue_reader, 0, sizeof(snapr *), &r);  /* Reply, and return block to writer */
  assertv(ret > 0, "Failed to send message to writer\n");
  print_rusage();
}

/*
 * Manage the write queue:  deal with queue message from writer.
 *
 * Check that the samples requested have not yet passed out of the
 * ring buffer; if true, enqueue the snapshot and reply OK, otherwise
 * reply with an error.
 */

static void process_queue_message(void *s) {
  int   ret;
  snapr *r = NULL;

  ret = zh_get_msg(s, 0, sizeof(snapr *), (void *)&r);
  assertv(ret == sizeof(snapr *), "Reader receives wrongly sized message: %d got vs %d expected\n", ret, sizeof(snapr *));
  assertv(r != NULL, "Received NULL pointer from writer\n");

  if(reader.state != READER_ARMED && reader.state != READER_RUN) {
    r->rd_state = SNAPSHOT_STOPPED; /* Snapshot fine but cannot do it */
  }
  else {
    if( check_snapshot_timing(r) ) {
      queue *h = &adc.write_queue;
      queue *q = h->q_next;

      if(q == h) {
	queue_ins_after(h, &r->Q);
      }
      else {
	while(q->q_next != h && r->last > ((snapr *)q->q_next)->last);
	queue_ins_after(q, &r->Q);
      }
      r->rd_state = SNAPSHOT_WAITING;    
      return;			/* Snapshot accepted, queue it and reply later */
    }
    else {
      r->rd_state = SNAPSHOT_ERROR;
    }
  }

  /* An error was detected:  reply now */
  ret = zh_put_msg(s, 0, sizeof(snapr *), (void *)&r); /* Send reply */
  assertv(ret > 0, "Failed to send message to writer\n");
}

/*
 * Reader thread message loop
 */

static void reader_thread_msg_loop() {    /* Read and process messages */
  int ret;

  reader.stoploop = false;
  reader.poll_delay = -1;

  /* Main loop:  read messages and process messages */
  zmq_pollitem_t  poll_list[] =
    { { wr_queue_reader, 0, ZMQ_POLLIN, 0 },
      { command, 0, ZMQ_POLLIN, 0 },
    };
#define	N_POLL_ITEMS	(sizeof(poll_list)/sizeof(zmq_pollitem_t))
  void (*poll_responders[N_POLL_ITEMS])(void *) =
    { process_queue_message,
      process_reader_command,
    };

  zh_put_multi(log, 1, "Reader thread is initialised");
  reader.state = READER_PARAM;

  print_rusage();

  /* CURRENT CODE FOR SETTING THE DATA START TIME WILL BE LATE ... ? */

  while( !reader.stoploop ) {
    int ret; 
    int nb;
    int delay = reader.poll_delay;
    int n;

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

	if( reader.state != READER_RUN ) { /* First data has arrived */
	  struct timespec start_stamp;

	  clock_gettime(CLOCK_MONOTONIC, &start_stamp);
	  compute_data_start_timestamp(&start_stamp, ns);
	  reader.state = READER_RUN;
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

    ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, delay);	/* Look for commands here */
    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      zh_put_multi(log, 1, "Reader loop interrupted");
      break;
    }
    if(ret < 0)
      break;

    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
	(*poll_responders[n])(poll_list[n].socket);
      }
    }
  }
}

/*
 * Reader thread main routine
 *
 * This loop either waits for a command on the command socket, or
 * loops reading from Comedi.  It aborts if it cannot get the sockets
 * it needs.
 */

void *reader_main(void *arg) {
  int ret;
  char *thread_msg = "thread exit";

  create_reader_comms();

  ret = set_reader_rt_scheduling();
  switch(ret) {
  case 1:
    zh_put_multi(log, 1, "Reader RT scheduling succeeded");
    break;
  case 0:
    zh_put_multi(log, 1, "Reader using normal scheduling: RTPRIO unset");
    break;
  default:
    zh_put_multi(log, 2, "Reader RT scheduling setup failed: ", strerror(errno));
    break;
  }

  struct timespec test_stamp;
  ret = clock_gettime(CLOCK_MONOTONIC, &test_stamp);
  assertv(ret == 0, "Failed to get monotonic clock time\n");  

  reader_thread_msg_loop();
  if(reader.state == READER_ARMED || reader.state == READER_RUN || reader.state == READER_RESTING) {
    comedi_stop_data_transfer();
  }

  zh_put_multi(log, 1, "Reader thread terminating by return");

  /* Clean up ZeroMQ sockets */
  zmq_close(wr_queue_reader);
  zmq_close(command);
  zmq_close(log);
  reader_thread_running = false;
  return (void *) thread_msg;
}

/*
 * Verify reader parameters and generate reader state description.
 */ 

int verify_reader_params(rparams *rp, strbuf e) {

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

  if(rp->r_frequency < 6e4 || rp->r_frequency > 3.75e5) {
    strbuf_appendf(e, "Sampling frequency %g not within compiled-in limits [%g,%g] Hz",
		   rp->r_frequency, 6e4, 3.75e5);
    return -1;
  }
  else {
    int ns = 1e9 / (rp->r_frequency*NCHAN); /* Inter-sample period */
    /* Correct for 30[MHz] USBDUXfast clock rate */
    reader.sample_ns = 100 * (ns / 100);
    if( (ns % 100) > 17 && (ns % 100) < 50 )
      reader.sample_ns += 33;
    if( (ns % 100) >= 50 && (ns % 100) < 83 )
      reader.sample_ns += 67;
    if( (ns & 100) >= 84 )
      reader.sample_ns += 100;
    rp->r_inter_sample_ns = reader.sample_ns; /* Need a plausible value at all times for computing snapshot data */
    rp->r_frequency = 1e9 / reader.sample_ns;
  }

  if(rp->r_window < 1 || rp->r_window > 30) {
    strbuf_appendf(e, "Capture window %d seconds outwith compiled-in range [%d,%d] seconds",
		   rp->r_window, 1, 30);
    return -1;
  }
  /* Got a reasonable window, i.e. ring buffer size */
  long page = 1e-9 * reader.sample_ns * sysconf(_SC_PAGESIZE) / sizeof(sampl_t); /* Duration of a page [ns] */
  reader.ringsz = (rp->r_window + 2) / page;

  if(rp->r_bufsz < 8 || rp->r_bufsz > 256) {
    strbuf_appendf(e, "Comedi buffer size %d MiB outwith compiled-in range [%d,%d] MiB",
		   rp->r_bufsz, 8, 256);
    return -1;
  }
  reader.bufsz = rp->r_bufsz;

  reader.comedi_device = rp->r_device;

#if 0
  p = find_param_by_name("permu", 5, ps, nps);
  assert( p != NULL );		/* Fatal if parameter not found */

  if( get_param_value(p, &v) == 0 ) { /* WINDOW value set */
    double temp;
    int permu;

    if( assign_value(p->p_type, v, &permu) < 0 )
      return -5;
    if( permu < 1 || permu > 15000 ) {
      errno = ERANGE;
      return -5;
    }

    temp = permu*reader.bufsz;
    temp *= 1024*1024;
    temp /= sysconf(_SC_PAGESIZE);

    if( temp < 1000000 ) {				/* Want at least one page */
      temp = 1000000*sysconf(_SC_PAGESIZE);
      temp /= reader.bufsz;
      temp /= 1024*1024;
      permu = (int)(temp + 0.5);			/* If PERMU is less than one page, increase it... */
    }
    reader.permu = permu;
  }
  else {
    return -5;
  }
#endif
  reader.state = READER_PARAM;
  return 0;
}
