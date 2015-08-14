#

#define  _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
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
#include "snapshot.h"
#include "reader.h"
#include "writer.h"

/*
 * Snapshot parameters, used by the snapshot commadn line.  Local to this thread.
 *
 * Note the #defines, which are used to extract the parameter values
 * when building snapshot descriptors -- there is no need to search
 * for the parameter when we know exactly where it is.
 */

// #define N_SNAP_PARAMS 7

param_t snapshot_params[N_SNAP_PARAMS] ={
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

const int n_snapshot_params =	N_SNAP_PARAMS; // (sizeof(snapshot_params)/sizeof(param_t));

/*
 * Socket for sending log messages from writer thread
 */

static void *wr_log;

/*
 * Writer parameter structure.
 */

wparams writer_parameters;

/*
 * Debug writer parameters
 */

#define MSGBUFSIZE 1024

void debug_writer_params() {
  char buf[MSGBUFSIZE];

  if(debug_level<1)
    return;

  snprintf(buf, MSGBUFSIZE,
	   "Writer: TMPDIR=%s, SNAPDIR=%s, WUID=%d, WGID=%d\n", tmpdir_path, writer_parameters.w_snapdir,
	   writer_parameters.w_uid, writer_parameters.w_gid);
  zh_put_multi(wr_log, 1, buf);
}

/*
 * Utility function: get a handle to a directory, creating it if
 * necessary and making sure the ownership is correct.
 */

static int new_directory(int dirfd, const char *name, uid_t uid, gid_t gid) {
  int ret;
  struct stat dir;

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
  if( fstatat(ret, "", &dir, AT_EMPTY_PATH) < 0 )
    return -1;
  if( (dir.st_uid != uid || dir.st_gid != gid) && fchownat(ret, "", uid, gid, AT_EMPTY_PATH ) < 0 )
    return -1;
  return ret;			/* Path fd for directory, with correct ownership */
}

/*
 * Debugging function for snapshot descriptors...
 */

static void debug_snapshot_descriptor(snapw *s) {
  char buf[MSGBUFSIZE];
  char *parch = "BESFLCP";
  char params[N_SNAP_PARAMS+1];
  snapr *r = s->this_snap;
  int i;

  for(i=0; i<N_SNAP_PARAMS; i++)
    params[i] = (s->params[i]? parch[i] : ' ');
  params[N_SNAP_PARAMS] = '\0';

  snprintf(buf, MSGBUFSIZE,
	   "Snap %s: file '%s' path '%s' state (%d,%d) P:%s; S:%08lx B:%08lx C:%08lx F:%016llx L:%016llx\n",
	   &s->name[0], &r->file[0], s->path,
	   r->rd_state, s->wr_state, &params[0],
	   r->samples, r->bytes, r->count, r->first, r->last);
  zh_put_multi(wr_log, 1, buf);
}

/*
 * Manage the write queue:  get parameters for a snapshot descriptor (queue entry).
 * Check the necessary parameters are present...
 */

static int get_writer_params(snapw *s) {
  int i;

  assertv(n_snapshot_params <= N_SNAP_PARAMS, "Inconsistent snapshot parameter list size (%d vs %d)\n", n_snapshot_params, N_SNAP_PARAMS);
  for(i=0; i<n_snapshot_params; i++)
    s->params[i] = pop_param_value(&snapshot_params[i]);

  /* path= is MANDATORY */
  if( !s->params[SNAP_PATH] )
    return -1;

  /* EITHER begin= OR start= is MANDATORY */
  if( !s->params[SNAP_BEGIN] && !s->params[SNAP_START] )
    return -2;

  /* IF begin= THEN end= XOR length= AND NOT finish= is REQUIRED */
  if( s->params[SNAP_BEGIN] ) {
    if( s->params[SNAP_FINISH] )
      return -3;
    if( !s->params[SNAP_END] && !s->params[SNAP_LENGTH] )
      return -3;
    if( s->params[SNAP_END] && s->params[SNAP_LENGTH] )
      return -3;
  }

  /* IF start= THEN finish= XOR length= AND NOT end= is REQUIRED */
  if( s->params[SNAP_START] ) {
    if( s->params[SNAP_END] )
      return -4;
    if( !s->params[SNAP_FINISH] && !s->params[SNAP_LENGTH] )
      return -4;
    if( s->params[SNAP_FINISH] && s->params[SNAP_LENGTH] )
      return -4;
  }

  /* count= is OPTIONAL */

  return 0;
}

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

    /* Complete the snapr structure sample-range contents */

    /* Start with length= -- if present, no finish= or end= spec. needed */
    if( s->params[SNAP_LENGTH] ) {
      if( assign_value(snapshot_params[SNAP_LENGTH].p_type, s->params[SNAP_LENGTH], &r->samples) < 0 )
	return -5;
      r->bytes = r->samples * sizeof(sampl_t);
      r->bytes += (sysconf(_SC_PAGE_SIZE) - r->bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
      r->samples = r->bytes / sizeof(sampl_t);
    }

    /* Mandatory EITHER begin= OR start= -- it was begin= */
    if( s->params[SNAP_BEGIN] ) {
      uint64_t time_val;
      if( assign_value(snapshot_params[SNAP_BEGIN].p_type, s->params[SNAP_BEGIN], &time_val) < 0 )
	return -6;
      time_val -= reader_parameters.r_capture_start_time; /* Time index of desired sample */
      time_val /= reader_parameters.r_inter_sample_ns;	/* Sample index of desired sample */
      r->first = time_val - (time_val % NCHAN); /* Fix to NCHAN boundary */
      if( !r->samples ) {			/* No length given, need end */
	if( assign_value(snapshot_params[SNAP_END].p_type, s->params[SNAP_END], &time_val) < 0 )
	  return -7;
	time_val -= reader_parameters.r_capture_start_time;
	time_val /= reader_parameters.r_inter_sample_ns;
	r->last = time_val + ((NCHAN - (time_val % NCHAN)) % NCHAN); /* Round up to integral number of channel sweeps */
	if(r->last <= r->first) {
	  errno = ERANGE;
	  return -8;
	}
	r->samples = r->last - r->first;
	r->bytes = r->samples * sizeof(sampl_t);
	r->bytes += (sysconf(_SC_PAGE_SIZE) - r->bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
	r->samples = r->bytes / sizeof(sampl_t);	 /* Round up to integral number of system pages */
      }
      r->last = r->first + r->samples;
    }

    /* Mandatory EITHER begin= OR start= -- it was start= */
    if( s->params[SNAP_START] ) {
      if( assign_value(snapshot_params[SNAP_START].p_type, s->params[SNAP_START], &r->first) < 0 )
	return -9;
      if( !r->samples ) {			/* No length given, need end */
	if( assign_value(snapshot_params[SNAP_FINISH].p_type, s->params[SNAP_FINISH], &r->last) < 0 )
	  return -10;
	if(r->last <= r->first) {
	  errno = ERANGE;
	  return -11;
	}
	r->last += ((NCHAN - (r->last % NCHAN)) % NCHAN); /* Round up to integral number of channel sweeps */
	r->samples = r->last - r->first;
	r->bytes = r->samples * sizeof(sampl_t);
	r->bytes += (sysconf(_SC_PAGE_SIZE) - r->bytes % sysconf(_SC_PAGE_SIZE)) % sysconf(_SC_PAGE_SIZE);
	r->samples = r->bytes / sizeof(sampl_t);	 /* Round up to integral number of system pages */
      }
      r->last = r->first + r->samples;
    }

    /* Optional count=, default is 1 */
    if( s->params[SNAP_COUNT] ) {
      if( assign_value(snapshot_params[SNAP_COUNT].p_type, s->params[SNAP_COUNT], &r->count) < 0 )
	return -12;
      if(r->count < 1) {
	errno = ERANGE;
	return -13;
      }
    }
    else {
      r->count = 1;
    }

    return 0;
}

static int initialise_snapshot_buffer(snapw *s, snapr *r) {
  int ret, fd;

  /* Open/mmap the (next) file for the sample data */
  snprintf(&r->file[0], SNAP_NAME_SIZE, "%016llx.s16", r->first);

  if(debug_level > 0)
    debug_snapshot_descriptor(s);

  fd = openat(s->dirfd, r->file, O_RDWR|O_CREAT|O_EXCL|O_NONBLOCK, 0600);
  if(fd < 0) {
    return -15;
  }
  ret = fchown(fd, writer_parameters.w_uid, writer_parameters.w_gid);
  if(ret < 0) {
    return -16;
  }
  ret = ftruncate(fd, r->bytes); /* Pre-size the file */
  if(ret < 0) {
    return -17;
  }

  errno = 0;
  r->mmap = mmap(NULL, r->bytes, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fd, 0);
  if( r->mmap == NULL || errno )
    return -18;
  close(fd);

  /* Touch the pages, pre-fault write and mark dirty */
  prefault_pages(r->mmap, r->bytes / sysconf(_SC_PAGESIZE), PREFAULT_RDWR);

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
  s->dirfd = new_directory(writer_parameters.w_snap_dirfd, s->path, 
			   writer_parameters.w_uid,
			   writer_parameters.w_gid);
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

/*
 * Manage the write queue:  deal with queue message from reader.
 */

static void process_queue_message(void *socket, void *command) {
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
      zh_put_multi(wr_log, 4, "Snapshot ", s->name, " aborted", timebuf);
      s->wr_state = SNAPSHOT_DONE;
      break;

    case SNAPSHOT_WRITTEN:					 /* The reader has finished with this one */
      zh_put_multi(wr_log, 4, "Snapshot ", s->name, " completed", timebuf);
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
      zh_put_multi(wr_log, 3, "Snapshot ", s->name, " rollover failed");
    }
    else {							 /* New snapshot descriptor ready */
      s->wr_state = SNAPSHOT_REPLIED;
      ret = zh_put_msg(socket, 0, sizeof(snapr *), (void *)&r);	 /* Hand it off to Reader */
      assertv(ret > 0, "Message to reader failed with %d\n", ret);
      return;
    }
  }

  /*
   * And here we tidy up
   */

  if( s->wr_state == SNAPSHOT_DONE ) {				 /* Finished with this descriptor */
    destroy_snapshot_descriptor(s);
    return;
  }

  /* SHOULD NOT REACH HERE, IF THE STATE MACHINE IS WORKING AS EXPECTED! */
  fprintf(stderr, "Writer: queue message in illegal state pair (%d,%d)\n", r->rd_state, s->wr_state);
  abort();
}

/*
 * Handle command message
 */

#define	COMMAND_BUFSIZE		1024
char command_buffer[COMMAND_BUFSIZE];

int process_writer_command(void *socket) {
  int used;
  int   ret;
  char *err = "OK";
  char *p;
  static char err_buf[COMMAND_BUFSIZE];

  used = zh_collect_multi(socket, &command_buffer[0], COMMAND_BUFSIZE, "");
  if(debug_level > 2)
    zh_put_multi(wr_log, 3, "Writer cmd: '", &command_buffer[0], "'");
  switch(command_buffer[0]) {
  case 'q':
  case 'Q':
    // zh_put_multi(socket, 1, "Quit OK"); /* Reply handled in main thread */
    return false;

  case 's':
  case 'S':
    p=&command_buffer[1];
    while( *p && !isspace(*p) ) p++; /* Are there parameters to process? */
    while( *p && isspace(*p) ) p++;
    if( !*p ) {			     /* There should be more string here */
      err = "Snapshot parameters apparently missing";
      break;
    }
    /* Process the snapshot parameters */
    int ret = push_params_from_string(p, snapshot_params, n_snapshot_params);
    if( ret < 0 ) { 
      snprintf(err_buf, COMMAND_BUFSIZE, "Snapshot param error at %d: %s", -ret, strerror(errno));
      err = err_buf;
      break;
    }

    /* Otherwise, succeeded in parsing snapshot parameters */
    snapw *s = NULL;
    ret = build_snapshot_descriptor(&s);

    if( ret == 0 ) {		    /* Send it to the reader for queueing */
      s->wr_state = SNAPSHOT_CHECK; /* We shall reply when reader has finished with it */
      ret = zh_put_msg(wr_queue_writer, 0, sizeof(snapr *), (void *)&s->this_snap);
      assertv(ret == sizeof(snapr *), "Queue message size inconsistent, %d vs. %d\n", ret, sizeof(snapr *));
      return true;
    }
    else {
      int error = errno;
      if(s != NULL) {
        destroy_snapshot_descriptor(s);
      }
      snprintf(err_buf, COMMAND_BUFSIZE, "Problem building snapshot descriptor at step %d: %s", -ret, strerror(error));
      err = err_buf;
      break;
    }
    return true;

  default:
    err = "Unexpected reader command: ";
    break;
  }
  zh_put_multi(wr_log, 2, err, &command_buffer[0]); /* Error occurred, return message */
  zh_put_multi(socket, 4, "NO: ERROR ", err, " in ", &command_buffer[0]);
  return true;
}

/*
 * Writer thread main routine
 */

void *writer_main(void *arg) {
  int ret, n;
  void *command;
  int running;

  /* Open and attach the writer thread's sockets */
  wr_log = zh_connect_new_socket(zmq_main_ctx, ZMQ_PUSH, LOG_SOCKET);
  assertv(wr_log != NULL, "Failed to instantiate log socket\n");
  command = zh_connect_new_socket(zmq_main_ctx, ZMQ_REP, WRITER_CMD_ADDR);
  assertv(command != NULL, "Failed to instantiate command socket\n");

  if(debug_level > 1)
    debug_writer_params();

  zmq_pollitem_t  poll_list[] =
    { { command, 0, ZMQ_POLLIN, 0 },
      { wr_queue_writer, 0, ZMQ_POLLIN, 0 },
    };
#define	POLL_NITEMS	(sizeof(poll_list)/sizeof(zmq_pollitem_t))

/* Writer initialisation is complete */
  zh_put_multi(wr_log, 1, "Writer thread is initialised");

  running = true;
  while( running ) {
    int ret = zmq_poll(&poll_list[0], POLL_NITEMS, -1);

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      zh_put_multi(wr_log, 1, "Writer loop interrupted");
      break;
    }
    if(ret < 0)
      break;
    if( poll_list[0].revents & ZMQ_POLLIN ) {
      if(debug_level > 2)
	fprintf(stderr, "Writer thread gets command message\n");
      running = process_writer_command(command);
    }
    if( poll_list[1].revents & ZMQ_POLLIN ) {
      if(debug_level > 2)
	fprintf(stderr, "Writer thread gets queue message\n");
      process_queue_message(wr_queue_writer, command);
    }
  }

  zh_put_multi(wr_log, 1, "Writer thread is terminating by return");

  /* Clean up ZeroMQ sockets */
  zmq_close(wr_log);
  zmq_close(command);
  writer_thread_running = false;
  return (void *)"normal exit";
}

/*
 * Verify the parameters for the writer and construct the writer state.
 */

int verify_writer_params(wparams *wp) {

  if( wp->w_schedprio != 0 ) { /* Check for illegal value */
    int max, min;

    min = sched_get_priority_min(SCHED_FIFO);
    max = sched_get_priority_max(SCHED_FIFO);
    if(wp->w_schedprio < min || wp->w_schedprio > max) {
      errno = ERANGE;
      return -1;
    }
  }

  /* Compute the UID and GID for file creation.
   *
   * If the GID parameter is set, use that for the group; if not, but
   * the UID parameter is set, get the group from that user and set
   * the uid from there too.  If neither is set, use the real uid/gid
   * of the thread.
   */
#if 0  
  p = find_param_by_name("gid", 3, ps, nps);
  assert(p != NULL);		/* Fatal if parameter not found */

  errno = 0;
  wp->w_gid = -1;
  if( get_param_value(p, &wgid) == 0 ) { /* Got a GID value */
    struct group *grp = getgrnam(wgid);

    if(grp == NULL)		/* The WGID name was invalid  */
      return -2;
    wp->w_gid = grp->gr_gid;
  }

  p = find_param_by_name("uid", 3, ps, nps);
  assert(p != NULL);		/* Fatal if parameter not found */

  errno = 0;
  wp->w_uid = -1;
  if( get_param_value(p, &wuid) == 0 ) { /* Got a UID value */
    struct passwd *pwd = getpwnam(wuid);

    if(pwd == NULL)		/* The WUID name was invalid */
      return -3;
    wp->w_uid = pwd->pw_uid;	/* Use this user's UID */
    if( wp->w_gid < 0 )
      wp->w_gid = pwd->pw_gid;	/* Use this user's principal GID */
  }
  else {
    wp->w_uid = getuid();		/* Use the real UID of this thread */
    wp->w_gid = getgid();		/* Use the real GID of this thread */
  }
#endif  
  /*
   * Check the snapdir directory exists and is correctly owned, and
   * get a path fd for it.
   */

  wp->w_snap_dirfd = new_directory(tmpdir_dirfd, wp->w_snapdir, wp->w_uid, wp->w_gid);
  if( wp->w_snap_dirfd < 0 )	/* Give up on failure */
    return -4;

  return 0;
}
