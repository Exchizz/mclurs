#

#define  _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/capability.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#include "assert.h"

#include <zmq.h>
#include <pthread.h>

#include <comedi.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "util.h"
#include "param.h"
#include "queue.h"
#include "mman.h"
#include "strbuf.h"
#include "chunk.h"
#include "snapshot.h"
#include "reader.h"
#include "writer.h"

/*
 * Snapshot parameters, used by the S command line.
 * Local to this thread.
 *
 * Note the #defines, which are used to extract the parameter values
 * when building snapshot descriptors -- there is no need to search
 * for the parameter when we know exactly where it is.
 */

static param_t snapshot_params[] ={
#define SNAP_BEGIN  0
  { "begin",  NULL, NULL,
    PARAM_TYPE(int64), PARAM_SRC_CMD,
    "start time of snapshot [ns from epoch]"
  },
#define SNAP_END  1
  { "end",    NULL, NULL,
    PARAM_TYPE(int64), PARAM_SRC_CMD,
    "finish time of snapshot [ns from epoch]"
  },
#define SNAP_START  2
  { "start",  NULL, NULL,
    PARAM_TYPE(int64), PARAM_SRC_CMD,
    "start sample of snapshot"
  },
#define SNAP_FINISH 3
  { "finish", NULL, NULL,
    PARAM_TYPE(int64), PARAM_SRC_CMD,
    "end sample of snapshot"
  },
#define SNAP_LENGTH 4
  { "length", NULL, NULL,
    PARAM_TYPE(int32), PARAM_SRC_CMD,
    "length of snapshot [samples]"
  },
#define SNAP_COUNT  5
  { "count",   NULL, NULL,
    PARAM_TYPE(int32), PARAM_SRC_CMD,
    "repeat count of snapshot"
  },
#define SNAP_PATH  6
  { "path",    NULL, NULL,
    PARAM_TYPE(string), PARAM_SRC_CMD,
    "storage path of snapshot data"
  },
};

static const int n_snapshot_params = (sizeof(snapshot_params)/sizeof(param_t));

/*
 * Snapshot working directory parameter(s), used by the D command line.
 */

static param_t snapwd_params[] ={
#define SNAP_SETWD  0
  { "dir",    NULL, NULL,
    PARAM_TYPE(string), PARAM_SRC_CMD,
    "working (sub-)directory for snapshots"
  },
};

static const int n_snapwd_params =  (sizeof(snapwd_params)/sizeof(param_t));

/*
 * --------------------------------------------------------------------------------
 *
 * INITIALISATION ROUTINES FOR WRITER THREAD:
 *
 * - Establish the communication endpoints needed
 * - Set up the required effective capabilities
 * - Set up RT priority scheduling (if requested)
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Writer parameter structure.
 */

wparams writer_parameters;

/*
 * Reader thread comms initialisation (failure is fatal).
 *
 * Called after the process-wide ZMQ context is created (elsewhere).
 */

static void *log;
static void *reader;
static void *command;

static int create_writer_comms() {
  /* Create necessary sockets */
  command  = zh_bind_new_socket(zmq_main_ctx, ZMQ_REP, WRITER_CMD_ADDR);	/* Receive commands */
  assertv(command != NULL, "Failed to instantiate reader command socket\n");
  log      = zh_connect_new_socket(zmq_main_ctx, ZMQ_PUSH, LOG_SOCKET);  /* Socket for log messages */
  assertv(log != NULL,     "Failed to instantiate reader log socket\n");
  reader   = zh_connect_new_socket(zmq_main_ctx, ZMQ_PAIR, READER_QUEUE_ADDR);
  assertv(reader != NULL,  "Failed to instantiate reader queue socket\n");
}

/*
 * Copy the necessary capabilities from permitted to effective set (failure is fatal).
 *
 * The writer needs:
 *
 * CAP_IPC_LOCK -- ability to mmap and mlock pages.
 * CAP_SYS_NICE -- ability to set RT scheduling priorities
 * CAP_SYS_ADMIN (Writer) -- ability to set RT IO scheduling priorities (unused at present)
 *
 * These capabilities should be in the CAP_PERMITTED set, but not in CAP_EFFECTIVE which was cleared
 * when the main thread dropped privileges by changing to the desired non-root uid/gid.
 */

static int set_up_writer_capability() {
  cap_t c = cap_get_proc();
  const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, CAP_SYS_ADMIN, };

  cap_set_flag(c, CAP_EFFECTIVE, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
  return cap_set_proc(c);
}

/*
 * Debug writer parameters
 */

#define MSGBUFSIZE 1024

static void debug_writer_params() {
  char buf[MSGBUFSIZE];

  if(debug_level<1)
    return;

  snprintf(buf, MSGBUFSIZE,
	   "Writer: TMPDIR=%s, SNAPDIR=%s\n", tmpdir_path, writer_parameters.w_snapdir);
  zh_put_multi(log, 1, buf);
}

/*
 * --------------------------------------------------------------------------------
 *
 * UTILITY FUNCTIONS USED ONLY BY THE WRITER THREAD
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Get a path handle to a directory, creating it if necessary.
 */

static int new_directory(int dirfd, const char *name) {
  int ret;

  ret = openat(dirfd, name, O_PATH|O_DIRECTORY); /* Try to open the directory */
  if(ret < 0 ) {
    if( errno != ENOENT )			 /* OK if it doesn't exist, otherwise fail */
      return -1;
    ret = mkdirat(dirfd, name, 0750);		 /* Didn't exist, try to create it */
    if( ret < 0 )
      return -1;
    ret = openat(dirfd, name, O_PATH|O_DIRECTORY); /* Try again */
    if(ret < 0)					 /* Give up on failure */
      return -1;
  }
}

/*
 * Manage the writer's 'working directory':  clear the old, resetting to snapdir;
 * find/create and set a new one, clearing an old if necessary.
 */

static void clear_writer_wd() {
  int fd = writer_parameters.w_snap_curfd;

  if( fd != writer_parameters.w_snap_dirfd ) {
    writer_parameters.w_snap_curfd = writer_parameters.w_snap_dirfd;
    close(fd);
  }
}


static int set_writer_new_wd(const char *dir) {
  int fd;

  clear_writer_wd();
  fd = new_directory(writer_parameters.w_snap_dirfd, dir);
  if(fd < 0)
    return -1;
  writer_parameters.w_snap_curfd = fd;
  return 0;
}

/*
 * --------------------------------------------------------------------------------
 * FUNCTIONS ETC. TO MANAGE SNAPSHOT DESCRIPTORS
 *
 * The writer maintains a list of "active" snapshot descriptors.  A descriptor
 * is created in response to an S command and is "active" until it has been both
 * (a) completely processed and also (b) reported back in response to a Z
 * command.  These data structures are entirely private to the writer.
 *
 * --------------------------------------------------------------------------------
 */

typedef struct {		/* Private snapshot descriptor structure used by writer */
  queue	      s_Q;		/* Queue header -- must be first member */
  char        s_name[SNAP_NAME_SIZE];	/* Printable name for snapshot */
  int         s_dirfd;		/* Dirfd of the samples directory */
  uint64_t    s_first;		/* The first sample to collect */
  uint64_t    s_last;		/* Collect up to but not including this sample */
  uint32_t    s_samples;	/* The number of samples to save */
  int	      s_bytes;		/* The total size of one sample file */
  uint32_t    s_count;		/* The remaining repetition count for this snapshot */
  int	      s_pending;	/* Count of pending repetitions */
  int	      s_status;		/* Status of this snapshot */
  const char *s_path;		/* Directory path for this snapshot */
  strbuf      s_cmdetc;		/* Command and Error string buffers */
  queue	      s_fileQhdr;	/* Header for the queue of file descriptor structures */
}
  snap_t;

static QUEUE_HEADER(snapQ);	/* The list of active snapshots */

#define qp2snap(qp)  ((snap_t *)qp)
#define snap2qp(s)   (&((s)->s_Q))

/*
 * Allocate and free snap_t structures
 */

static snap_t *alloc_snapshot() {
  static int snap_counter = 0;
  snap_t *ret = calloc(1, sizeof(snap_t));

  if(ret) {
    init_queue( snap2qp(ret) );
    ret->s_dirfd = -1;
    snprintf(&ret->s_name[0], SNAP_NAME_SIZE, "snap_%08x", snap_counter);
    init_queue(&ret->s_fileQhdr);
  }
  return ret;
}

static void free_snapshot(snap_t *s) {
  if( !queue_singleton(snap2qp(s)) )
    de_queue(snap2qp(s));
  if( !queue_singleton(&s->s_fileQhdr) ) {
    /* Panic! */
  }
  if(s->s_dirfd >= 0)
    close(s->s_dirfd);
  free( (void *)s );
}

/*
 * Manage the writer snapshot queue:
 *
 * - Check the parameters in an S command
 */

static int check_snapshot_params(param_t ps[]) {

  /* path= is MANDATORY */
  if( ps[SNAP_PATH].p_str )
    return -1;

  /* EITHER begin= OR start= is MANDATORY */
  if( !ps[SNAP_BEGIN].p_str && !ps[SNAP_START].p_str )
    return -2;

  /* IF begin= THEN end= XOR length= AND NOT finish= is REQUIRED */
  if( ps[SNAP_BEGIN].p_str ) {
    if( ps[SNAP_FINISH].p_str )
      return -3;
    if( !ps[SNAP_END].p_str && !ps[SNAP_LENGTH].p_str )
      return -3;
    if( ps[SNAP_END].p_str && ps[SNAP_LENGTH].p_str )
      return -3;
  }

  /* IF start= THEN finish= XOR length= AND NOT end= is REQUIRED */
  if( ps[SNAP_START].p_str ) {
    if( ps[SNAP_END].p_str )
      return -4;
    if( !ps[SNAP_FINISH].p_str && !ps[SNAP_LENGTH].p_str )
      return -4;
    if( ps[SNAP_FINISH].p_str && ps[SNAP_LENGTH].p_str )
      return -4;
  }

  /* count= is OPTIONAL */

  return 0;
}

/*
 * Complete the snap_t structure sample-range contents -- we know the parameter
 * subset is correct We can also assume that the various members of the snap_t
 * structure have been instantiated by parameter assignment handled by the
 * caller.  We need the param[] array to determine which case we are handling.
 * No errors can occur here because they are dealt with by the caller(s) of this
 * routine.
 */

static void setup_snapshot_samples(snap_t *s, param_t p[]) {

  /* Start with length= -- if present, no finish= or end= spec. needed */
  if( p[SNAP_LENGTH].p_str ) {	/* Length was stored in s_samples, round up to integral number of pages */
    s->s_bytes = s->s_samples * sizeof(sampl_t);
    s->s_bytes += (sysconf(_SC_PAGE_SIZE) - s->s_bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
    s->s_samples = s->s_bytes / sizeof(sampl_t);
  }

  /* Mandatory EITHER begin= OR start= -- it was begin= */
  if( p[SNAP_BEGIN].p_str ) {	/* Begin time was stored in s_first */
    s->s_first -= reader_parameters.r_capture_start_time; /* Time index of desired sample */
    s->s_first /= reader_parameters.r_inter_sample_ns;    /* Sample index of desired sample */
    s->s_first = s->s_first - (s->s_first % NCHAN);	  /* Fix to NCHAN boundary */
    if( !s->s_samples ) {				  /* No length given, need end from s_last */
      s->s_last -= reader_parameters.r_capture_start_time;
      s->s_last /= reader_parameters.r_inter_sample_ns;
      s->s_last = s->s_last + ((NCHAN - (s->s_last % NCHAN)) % NCHAN); /* Round up to integral number of channel sweeps */
      s->s_samples = s->s_last - s->s_first;
      s->s_bytes = s->s_samples * sizeof(sampl_t);
      s->s_bytes += (sysconf(_SC_PAGE_SIZE) - s->s_bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
      s->s_samples = s->s_bytes / sizeof(sampl_t);	    /* Round up to integral number of system pages */
    }
    s->s_last = s->s_first + s->s_samples;		    /* Calculate end point using rounded-up sample count */
  }

  /* Mandatory EITHER begin= OR start= -- it was start= */
  if( p[SNAP_START].p_str ) {	/* Start sample was stored in s_first */
    if( !s->s_samples ) {	/* No length given, need end from s_last */
      s->s_last += ((NCHAN - (s->s_last % NCHAN)) % NCHAN); /* Round up to integral number of channel sweeps */
      s->s_samples = s->s_last - s->s_first;		    /* Compute requested length */
      s->s_bytes = s->s_samples * sizeof(sampl_t);
      s->s_bytes += (sysconf(_SC_PAGE_SIZE) - s->s_bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
      s->s_samples = s->s_bytes / sizeof(sampl_t);	    /* Round up to integral number of system pages */
    }
    s->s_last = s->s_first + s->s_samples;		    /* Calculate end point using rounded-up sample count */
  }

  /* Optional count=, default is 1 */
  if( !p[SNAP_COUNT].p_str ) { /* The count parameter was written to s_count */
    s->s_count = 1;
  }

  s->s_pending = 0;
  s->s_status  = 0;
}

/*
 * Build snapshot from S command line: the main thread passes a ring
 * of strbufs comprising the command buffer and the error buffer.
 *
 * The sequence of operations is:
 * - allocate a snap_t structure and bind the parameter val pointers to it
 * - populate the parameter structures from the string in the command buffer
 * - check the parameter set for correctness (check_snapshot_params)
 * - check the snapshot path and create the dirfd
 * - populate the sample value elements (setup_snapshot_samples)
 * - return the complete structure
 *
 * Errors arising during the above process cause an error status mark and are
 * reported in the error buffer.
 */

static snap_t *build_snapshot_descriptor(strbuf c) {
  strbuf   e   = strbuf_next(c);
  param_t *ps  = &snapshot_params[0]; 
  int      nps = n_snapshot_params;
  snap_t  *ret;
  int	   err;

  if( !(ret = alloc_snapshot()) ) { /* Allocation failed: no memory? */

    return ret;
  }

  /* Initialise the targets for the parameters */
  ps[SNAP_BEGIN].p_val  = (void **) &ret->s_first;
  ps[SNAP_END].p_val    = (void **) &ret->s_last;
  ps[SNAP_START].p_val  = (void **) &ret->s_first;
  ps[SNAP_FINISH].p_val = (void **) &ret->s_first;
  ps[SNAP_LENGTH].p_val = (void **) &ret->s_samples;
  ps[SNAP_COUNT].p_val  = (void **) &ret->s_count;
  ps[SNAP_PATH].p_val   = (void **) &ret->s_path;

  /* Check the populated parameters */
  err = check_snapshot_params(ps);
  if(err < 0) {

  }

  /* Assign parameters to values */

  if(ret->s_last <= ret->s_first) { /* Parameter error:  end before start */

  }
  /* Do path */

  /* Set up the sample-dependent values */
  setup_snapshot_samples(ret, ps);

  /* All done, no errors */
  ret->s_status = SNAPSHOT_INIT;
  return ret;
}

/*
 * Set up snapshot -- create the necessary file descriptor structures etc.
 */

static void setup_snapshot(snap_t *s) {
}

/*
 * Called when a snapshot file has just been written.
 */

static void refresh_snapshot(snap_t *s) {

  if( !s->s_count ) {	/* No files left to request. */

  }
  else {			/* A file request has just completed */
    s->s_pending--;
  }
}

/*
 * Debugging function for snapshot descriptors...
 */

static void debug_snapshot_descriptor(snap_t *s) {
  char buf[MSGBUFSIZE];

  snprintf(buf, MSGBUFSIZE,
	   "Snap %s: path '%s' status %d  P:%s; S:%08lx B:%08lx C:%08lx F:%016llx L:%016llx\n",
	   &s->s_name[0], s->s_path, s->s_status,
	   s->s_samples, s->s_bytes, s->s_count, s->s_first, s->s_last);
  zh_put_multi(log, 1, buf);
}

/*
 * --------------------------------------------------------------------------------
 * FUNCTIONS ETC. FOR SNAPSHOT FILE DESCRIPTOR STRUCTURES:  ONE OF THESE PER FILE TO CAPTURE.
 *
 * --------------------------------------------------------------------------------
 */

typedef struct {
  queue	      f_Q;			/* Queue header for file descriptor structures */
  snap_t     *f_parent;			/* The snap_t structure that generated this file capture */
  int         f_fd;			/* System file descriptor -- only needed while pages left to map */
  char        f_name[SNAP_NAME_SIZE];	/* Name of this file:  the hexadecimal first sample number .s16 */
  int	      f_ixnr;			/* Index number of this file in the full set for the snapshot */
  queue	      f_chunkQhdr;		/* Header for this file's writer chunk queue */
}
  snapfile_t;

/*
 * Allocate and free snapfile_t structures
 */

static snapfile_t *alloc_snapfile() {
  snapfile_t *ret = calloc(1, sizeof(snapfile_t));

  if(ret) {
    ret->f_fd = -1;
  }
  return ret;
}

static void free_snapfile(snapfile_t *f) {
  if(f->f_fd >= 0)
    close(f->f_fd);
  free((void *)f);
}

/*
 * Initialise a snapfile_t structure from a snap_t structure.
 */

static int init_snapfile(snapfile_t *f, snap_t *s) {
  int fd;
  int ret;

  snprintf(&f->f_name[0], SNAP_NAME_SIZE, "%016llx.s16", s->s_first);
  fd = openat(s->s_dirfd, &f->f_name[0], O_RDWR|O_CREAT|O_EXCL, 0600);
  if(fd < 0) {
    return -1;
  }
  ret = ftruncate(fd, s->s_bytes); /* Pre-size the file */
  if(ret < 0) {			   /* Try to tidy up... */
    int e = errno;		   /* Save the original errno */
    unlinkat(s->s_dirfd, &f->f_name[0], 0);
    close(fd);
    errno = e;
    return -2;
  }
  f->f_fd = fd;
  return 0;
}

/*
 * Set up snapshot file descriptor, create chunks etc.
 */

static void setup_snapfile(snapfile_t *f) {
}

/*
 * Completed file descriptor -- called when file acquisition ends successfully
 */

static void completed_snapfile(snapfile_t *f) {
}

/*
 * Abort file descriptor -- called when file acquisition is aborted
 */

static void abort_snapfile(snapfile_t *f) {
}

#if 0

/*
 * Manage the write queue:  build a snapshot descriptor (queue entry).
 *
 * The command specifies:   begin=<time> or start=<sample>,
 *			    end=<time>   or finish=<sample> or len=<size>
 *			    count=<number> (optional)
 *			    path=<dir>
 *
 * These parameters should have been instantiated in the parameter
 * structures by the caller.  Here we check that the required
 * information is available and that the file system operations are
 * allowed.  The result is a snap structure that describes the capture
 * which can be passed to the Reader for checking data availability
 * and for queueing on the write-queue in the reader.
 */

static int build_snapshot_rd_descriptor(snapw *s, snapr *r) {
  int ret;

  init_queue(&r->Q);
  r->parent = s;


    return 0;
}

static int initialise_snapshot_file(snapw *s, snapr *r) {
  int ret, fd;

  r->mmap = mmap_and_lock(fd, 0, r->bytes, PROT_RDWR|PREFAULT_RDWR|MAL_LOCKED);
  if( r->mmap == NULL || errno )
    return -18;
  close(fd);

  r->rd_state = SNAPSHOT_INIT;
  return 0;
}

static int build_snapshot_descriptor(snapw **sp) {
  snapw *s;
  snapr *r;
  int ret;

  s = (snapw *)calloc(1, sizeof(snapw));
  r = (snapr *)calloc(1, sizeof(snapr));
  if( s == NULL || r == NULL )
    return -1;

  s->this_snap = r;
  snprintf(&s->name[0], SNAP_NAME_SIZE, "%p", (void *)s);
  *sp = s;

  /* Fill out the parameters here */
  ret = get_writer_params(s);
  if( ret < 0 ) {
    errno = EINVAL;
    return ret;
  }

  /* Mandatory path: create/open a correctly-owned directory for this data */
  s->path = s->params[SNAP_PATH];
  s->dirfd = new_directory(writer_parameters.w_snap_dirfd, s->path);
  if(s->dirfd < 0) {					     /* Give up on failure */
    return -14;
  }

  s->preload = SNAP_WR_PRELOAD;

  ret = build_snapshot_rd_descriptor(s, r);
  if( ret < 0 ) {
    return ret;
  }

  s->stride  = SNAP_WR_PRELOAD * r->samples;

  ret = initialise_snapshot_buffer(s, r);
  if( ret < 0 ) {
    return ret;
  }

  s->wr_state = SNAPSHOT_INIT;
  return 0;
}

static int refresh_snapshot_descriptor(snapw *s) {
  snapr *r = s->this_snap;

  assertv(r != NULL, "Got NULL pointer to work on\n");
  assertv(r->count > 0, "Count %d not positive\n", r->count);
  r->start = NULL;
  munmap(r->mmap, r->bytes);	/* Data may actually be written out here... */
  r->mmap = NULL;
  r->first += s->stride;
  r->last  += s->stride;

  return initialise_snapshot_buffer(s, r);
}

static void destroy_snapshot_descriptor(snapw *s) {
  int i;

  if( s == NULL )
    return;

  if( s->dirfd > 0 )
    close(s->dirfd);

  for(i=0; i<N_SNAP_PARAMS; i++) {
    if(s->params[i])
      free(s->params[i]);
  }

  if( s->this_snap ) {
    snapr *r = s->this_snap;
    munmap(r->mmap, r->bytes);
    free(r);
    s->this_snap = NULL;
  }

  free(s);
  return;
}

#endif

/*
 * --------------------------------------------------------------------------------
 *
 * DEAL WITH COMMAND AND QUEUE MESSAGES AS THEY ARRIVE.
 *
 *  --------------------------------------------------------------------------------
 */

/*
 * Manage the write queue:  deal with queue message from reader.
 */

static int process_queue_message(void *socket) {
  snapr *r = NULL;
  snapw *s;
  int ret;

  ret = zh_get_msg(socket, 0, sizeof(snapr *), (void *)&r);
  assertv(ret == sizeof(snapr *), "Queue message size wrong %d vs %d\n", ret, sizeof(snapr *));		/* We are expecting a message */
  assertv(r != NULL, "Queue message was NULL pointer\n");

  s = r->parent;
  assertv(s != NULL, "Queue message %p with null parent\n", r);

  /*
   * This first if() handles the case where the received command is waiting for its reply.
   *
   * We sent a snapshot descriptor to the reader for checking.  By the time we process this reply, the Reader
   * will have rejected with an error or queued the snapshot.  The Reader replies to this block when the first
   * batch of snapshot data (all of it, unless the snapshot repeats) has been transferred.
   */

  if( s->wr_state == SNAPSHOT_CHECK ) {				 /* This is reader's reply for this message */
    switch( r->rd_state ) {
    case SNAPSHOT_ERROR:					 /* Failed reader check -- send error reply */
      zh_put_multi(command, 2, "NO SNAP ", &s->name[0]);
      unlinkat(s->dirfd, &r->file[0], 0);			 /* Remove contentless snapshot file */
      s->wr_state = SNAPSHOT_DONE;
      break;

    case SNAPSHOT_STOPPED:					 /* Snapshot parameters fine, but no capture in progress */
      zh_put_multi(command, 2, "ST SNAP ", &s->name[0]);
      unlinkat(s->dirfd, &r->file[0], 0);			 /* Remove contentless snapshot file */
      s->wr_state = SNAPSHOT_DONE;
      break;

    case SNAPSHOT_WRITTEN:
      zh_put_multi(command, 2, "OK SNAP ", &s->name[0]);
      s->wr_state = SNAPSHOT_REPLIED;
      break;
    }
  }

  /*
   * Next we handle the case where a snapshot part has been written successfully and a subsequent block in the
   * same series is being processed.
   *
   * At this point we have sent a (positive) reply to the original snapshot command message.  We can arrive
   * here if the Reader has replied to a SNAPSHOT_CHECK request with SNAPSHOT_WRITTEN, or to a repeat (i.e. 
   * SNAPSHOT_REPLIED) request.  If the reply was SNAPSHOT_WRITTEN, then we check for repeats and organise them
   * as required.
   */

#define TIMESPECSUB(a,b,c) do { c.tv_sec=a.tv_sec-b.tv_sec; c.tv_nsec=a.tv_nsec-b.tv_nsec; if(c.tv_nsec<0) { c.tv_sec--; c.tv_nsec+=1000000000; } } while(0)

  if( s->wr_state == SNAPSHOT_REPLIED ) {			 /* An OK reply has already been sent;  this is a repeat snapshot  */
    struct timespec iv1;
    char   timebuf[256] = { '\0' };

    TIMESPECSUB(r->written, r->ready,   iv1);			 /* Compute data copying interval */
    snprintf(timebuf, 256, " [%d,%09d]", iv1.tv_sec, iv1.tv_nsec);
    switch( r->rd_state ) {
    case SNAPSHOT_ERROR:					 /* Snapshot capture parameters unacceptable */
      zh_put_multi(log, 4, "Snapshot ", s->name, " aborted", timebuf);
      s->wr_state = SNAPSHOT_DONE;
      break;

    case SNAPSHOT_WRITTEN:					 /* The reader has finished with this one */
      zh_put_multi(log, 4, "Snapshot ", s->name, " completed", timebuf);
      s->wr_state = r->count? SNAPSHOT_REPEAT : SNAPSHOT_DONE;
      break;
    }
  }

  /*
   * Here we actually deal with repeat requests for new parts
   */

  if( s->wr_state == SNAPSHOT_REPEAT ) {			 /* Finished with a snapshot but count > 0, so repeat */
    ret = refresh_snapshot_descriptor(s);			 /* Re-initialise for next file */
    if(ret < 0) {
      s->wr_state = SNAPSHOT_DONE;
      zh_put_multi(log, 3, "Snapshot ", s->name, " rollover failed");
    }
    else {							 /* New snapshot descriptor ready */
      s->wr_state = SNAPSHOT_REPLIED;
      ret = zh_put_msg(socket, 0, sizeof(snapr *), (void *)&r);	 /* Hand it off to Reader */
      assertv(ret > 0, "Message to reader failed with %d\n", ret);
      return true;
    }
  }

  /*
   * And here we tidy up
   */

  if( s->wr_state == SNAPSHOT_DONE ) {				 /* Finished with this descriptor */
    destroy_snapshot_descriptor(s);
    return true;
  }

  /* SHOULD NOT REACH HERE, IF THE STATE MACHINE IS WORKING AS EXPECTED! */
  assertv(false, "Writer: queue message in illegal state pair (%d,%d)\n", r->rd_state, s->wr_state);
  return false;
}

/*
 * Handle command messages
 */

int process_writer_command(void *s) {
  int     used;
  int     ret;
  char   *p;
  strbuf  cmd;
  char   *cmd_buf;
  strbuf  err;

  used = zh_get_msg(s, 0, sizeof(strbuf), &cmd);
  if( !used ) {			/* Quit */
    return false;
  }

  cmd_buf = strbuf_string(cmd);
  err = strbuf_next(cmd);

  switch(cmd_buf[0]) {
  case 'd':			/* Dir command */
  case 'D':
    strbuf_printf(err, "OK Dir");
    break;

  case 'z':
  case 'Z':
    strbuf_printf(err, "OK Status");
    break;

  case 's':			/* Snap command */
  case 'S':
    p=&cmd_buf[1];
    while( *p && !isspace(*p) ) p++; /* Are there parameters to process? */
    while( *p && isspace(*p) ) p++;
    if( !*p ) {			     /* There should be more string here */
      strbuf_printf(err, "NO: Snap -- parameters apparently missing");
      break;
    }

    /* Process the snapshot parameters */
    int ret = push_params_from_string(p, snapshot_params, n_snapshot_params);
    if( ret < 0 ) { 
      strbuf_printf(err, "NO: Snap -- param error at %d: %m", -ret);
      break;
    }

    /* Otherwise, succeeded in parsing snapshot parameters */
    snapw *s = NULL;
    ret = build_snapshot_descriptor(&s);

    if( ret == 0 ) {		    /* Send it to the reader for queueing */
      s->wr_state = SNAPSHOT_CHECK; /* We shall reply when reader has finished with it */
      ret = zh_put_msg(reader, 0, sizeof(snapr *), (void *)&s->this_snap);
      assertv(ret == sizeof(snapr *), "Queue message size inconsistent, %d vs. %d\n", ret, sizeof(snapr *));
      strbuf_printf(err, "OK Snap");
    }
    else {
      int error = errno;
      if(s != NULL) {
        destroy_snapshot_descriptor(s);
      }
      errno = error;
      strbuf_printf(err, "NO: Snap -- problem building snapshot descriptor at step %d: %m", -ret);
    }
    return true;

  default:
    strbuf_printf(err, "NO: Writer -- unexpected writer command");
    break;
  }
  zh_put_multi(log, 3, strbuf_string(err), "\n  ", &cmd_buf[0]); /* Error occurred, return message */
  zh_put_msg(s, 0, sizeof(strbuf), (void *)&err);
  return true;
}

/*
 * Writer thread message loop
 */

static void writer_thread_msg_loop() {    /* Read and process messages */
  int ret;
  int running;
  int n;

  zmq_pollitem_t  poll_list[] =
    {  { reader, 0, ZMQ_POLLIN, 0 },
       { command, 0, ZMQ_POLLIN, 0 },
    };
#define	N_POLL_ITEMS	(sizeof(poll_list)/sizeof(zmq_pollitem_t))
  int (*poll_responders[N_POLL_ITEMS])(void *) =
    { process_queue_message,
      process_writer_command,
    };

  /* Writer initialisation is complete */
  zh_put_multi(log, 1, "Writer thread is initialised");

  running = true;
  while( running ) {
    int ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, -1);

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      zh_put_multi(log, 1, "Writer loop interrupted");
      break;
    }
    if(ret < 0)
      break;

    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
	running = (*poll_responders[n])(poll_list[n].socket);
      }
    }
  }
}

/*
 * Writer thread main routine
 */

void *writer_main(void *arg) {
  int ret, n;

  /* Verify parameter structure data */
  assertv(n_snapshot_params <= N_SNAP_PARAMS,
	  "Inconsistent snapshot parameter list size (%d vs %d)\n",
	  n_snapshot_params, N_SNAP_PARAMS);

  if(debug_level > 1)
    debug_writer_params();

  writer_thread_msg_loop();
  zh_put_multi(log, 1, "Writer thread is terminating by return");

  /* Clean up ZeroMQ sockets */
  zmq_close(log);
  zmq_close(reader);
  zmq_close(command);
  writer_thread_running = false;
  return (void *)"normal exit";
}

/*
 * Verify the parameters for the writer and construct the writer state.
 */

int verify_writer_params(wparams *wp, strbuf e) {
  extern int tmpdir_dirfd;	/* Imported from snapshot.c */
  int ret;

  if( wp->w_schedprio != 0 ) {	/* Check for illegal value */
    int max, min;

    min = sched_get_priority_min(SCHED_FIFO);
    max = sched_get_priority_max(SCHED_FIFO);
    if(wp->w_schedprio < min || wp->w_schedprio > max) {
      strbuf_appendf(e, "RT scheduling priority %d not in kernel's acceptable range [%d,%d]",
		    wp->w_schedprio, min, max);
      return -1;
    }
  }

  /*
   * Check that the requested mmap'd transfer RAM size and the
   * transfer chunk size are reasonable.
   */
  if(wp->w_lockedram < MIN_RAM_MB || wp->w_lockedram > MAX_RAM_MB) {
    strbuf_appendf(e, "Transfer Locked RAM parameter %d MiB outwith compiled-in range [%d, %d] MiB",
		  wp->w_lockedram, MIN_RAM_MB, MAX_RAM_MB);
    return -1;
  }
  if(wp->w_chunksize < MIN_CHUNK_SZ || wp->w_chunksize > MAX_CHUNK_SZ) {
    strbuf_appendf(e, "Transfer chunk size %d KiB outwith compiled-in range [%d, %d] KiB",
		  wp->w_chunksize, MIN_CHUNK_SZ, MAX_CHUNK_SZ);
    return -1;
  }

  /* Compute the number of frames available */
  const int pagesize = sysconf(_SC_PAGESIZE);
  int sz = wp->w_chunksize*1024;
  int nfr;
  sz = pagesize * ((sz + pagesize - 1) / pagesize); /* Round up to multiple of PAGE SIZE */
  wp->w_chunksize = sz / 1024;
  nfr = (wp->w_lockedram * 1024*1024) / sz;	    /* Number of frames that fit in locked RAM */
  if(nfr < MIN_NFRAMES) {
    strbuf_appendf(e, "Adjusted chunk size %d KiB and given RAM %d MiB yield too few (%d < %d) frames",
		   wp->w_chunksize, wp->w_lockedram, nfr, MIN_NFRAMES);
    return -1;
  }

  /*
   * Check the snapdir directory exists and is correctly owned, and
   * get a path fd for it.
   */
  wp->w_snap_dirfd = new_directory(tmpdir_dirfd, wp->w_snapdir);
  if( wp->w_snap_dirfd < 0 ) {	/* Give up on failure */
    strbuf_printf(e, "Snapdir %d inaccessible: %m", wp->w_snapdir);
    return -1;
  }

  /*
   * Now try to get the memory for the transfer RAM...
   */
  ret = init_frame_system(e, nfr, wp->w_lockedram, wp->w_chunksize);
  return ret;
}
