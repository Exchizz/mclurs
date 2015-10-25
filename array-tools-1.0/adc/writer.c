#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

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

#include "error.h"
#include "util.h"
#include "param.h"
#include "queue.h"
#include "mman.h"
#include "strbuf.h"
#include "chunk.h"
#include "adc.h"
#include "snapshot.h"
#include "reader.h"
#include "writer.h"
#include "rtprio.h"

/* We import the READER's ADC object for its time conversion and activity check routines */ 
import adc reader_adc;

/*
 * --------------------------------------------------------------------------------
 *
 * TYPES INTERNAL TO THE WRITER THREAD
 *
 * -- snapshot descriptor
 * -- snapshot file descriptor
 * -- forward function declarations
 * -- local queue headers
 */

/* Snapshot Descriptor Structure */

typedef struct {                /* Private snapshot descriptor structure used by WRITER */
  queue       s_xQ[2];          /* Queue headers -- must be first member */
#define s_Q          s_xQ[0]    /* Active snapshot queue header */
#define s_fileQhdr   s_xQ[1]    /* Header for the queue of file descriptor structures */
  uint16_t    s_name;           /* 'Name' for snapshot */
  int         s_dirfd;          /* Dirfd of the samples directory */
  uint64_t    s_first;          /* First sample to collect for the next repetition */
  uint64_t    s_last;           /* Collect up to but not including this sample in the next repetition */
  uint32_t    s_samples;        /* Number of samples to save */
  int         s_bytes;          /* Total size of one sample file */
  uint32_t    s_count;          /* Repetition count for this snapshot */
  int         s_pending;        /* Count of pending repetitions */
  int         s_done;           /* Count of completed repetitions */
  int         s_status;         /* Status of this snapshot */
  const char *s_path;           /* Directory path for this snapshot */
  strbuf      s_error;          /* Error strbuf for asynchronous operation */
}
  snap_t;

/* Forward declarations of snapshot descriptor routines */

private const char*snapshot_name(snap_t *);

#define qp2snap(qp)  ((snap_t *)&(qp)[0])
#define snap2qp(s)   (&((s)->s_xQ[0]))

#define fq2snap(fq)  ((snap_t *)&(fg)[-1])
#define snap2fq(s)   (&((s)->s_xQ[1]))

#define qp2sname(p)  snapshot_name(qp2snap(p))

/* Snapshot File Descriptor Structure */

typedef struct _sfile {
  queue       f_Q;                      /* Queue header for file descriptor structures */
  snap_t     *f_parent;                 /* The snap_t structure that generated this file capture */
  int         f_fd;                     /* System file descriptor -- only needed while pages left to map */
  int         f_indexnr;                /* Index number of this file in the full set for the snapshot */
  int         f_nchunks;                /* Number of chunks allocated for this file transfer */
  int         f_written;                /* Number of chunks actually written so far */
  int         f_pending;                /* Number of chunks controlled by the READER thread */
  chunk_t    *f_chunkQ;                 /* Pointer to this file's WRITER chunk queue */
  int         f_status;                 /* Status flags for this file */
  strbuf      f_error;                  /* The strbuf to write error text into */
  uint16_t    f_name;                   /* Unique number for debugging */
  char        f_file[FILE_NAME_SIZE];   /* Name of this file:  the hexadecimal first sample number .s16 */
}
  snapfile_t;

/* Forward declarations of snapshot file descriptor routines needed by snapshot */

private snapfile_t *alloc_snapfile();
private int  setup_snapfile(snapfile_t *, snap_t *);
private void completed_snapfile(snapfile_t *);
private void abort_snapfile(snapfile_t *);
private void debug_snapfile(snapfile_t *);
import  const char *snapfile_name(snapfile_t *);

#define qp2file(p)      ((snapfile_t *)(p))
#define file2qp(f)      (&(f)->f_Q)

#define qp2fname(p)     snapfile_name(qp2file(p))

/* Local queue headers etc. used by the WRITER thread */

private QUEUE_HEADER(snapQ);            /* The list of active snapshots */
public queue *ssQp = &snapQ;            /* For debugging */
private QUEUE_HEADER(WriterChunkQ);     /* The list of chunks awaiting mapping, in order of first sample */
public queue *wcQp = &WriterChunkQ;     /* For debugging */

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

public wparams writer_parameters;

/* The values below are computed from writer_parameters by the verify() function */

private int wp_nframes;         /* Number of transfer frames prepared */
private int wp_chunksamples;    /* Number of samples in a chunk */
private int wp_snap_dirfd;      /* Snapdir path fd */
private int wp_snap_curfd;      /* Path fd of the 'working' directory */
private int wp_totxfrsamples;   /* Total scheduled transfer samples remaining */
private int wp_nfiles;          /* Number of files in progress */

/* Read-only access to chunk size, needed by READER */

public int writer_chunksize_samples() {
  return wp_chunksamples;
}

/*
 * Reader thread comms initialisation (failure is fatal).
 *
 * Called after the process-wide ZMQ context is created (elsewhere).
 */

private void *reader;
private void *command;

public void  *logskt_WRITER;    /* Used by WARNING and LOG macros */

private void create_writer_comms() {
  import void *snapshot_zmq_ctx;
  void *log;
  /* Create necessary sockets */
  command  = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_REP, WRITER_CMD_ADDR);    /* Receive commands */
  assertv(command != NULL, "Failed to instantiate reader command socket\n");
  log      = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_PUSH, LOG_SOCKET);  /* Socket for log messages */
  assertv(log != NULL,     "Failed to instantiate reader log socket\n");
  reader   = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, READER_QUEUE_ADDR);
  assertv(reader != NULL,  "Failed to instantiate reader queue socket\n");
  logskt_WRITER = log;
}

/* CLose everything created above */

private void close_writer_comms() {
  zmq_close(logskt_WRITER);
  zmq_close(reader);
  zmq_close(command);
}

/*
 * Copy the necessary capabilities from permitted to effective set (failure is fatal).
 *
 * The WRITER needs:
 *
 * CAP_IPC_LOCK -- ability to mmap and mlock pages.
 * CAP_SYS_NICE -- ability to set RT scheduling priorities
 * CAP_SYS_ADMIN (WRITER) -- ability to set RT IO scheduling priorities (unused at present)
 *
 * These capabilities should be in the CAP_PERMITTED set, but not in CAP_EFFECTIVE which was cleared
 * when the main thread dropped privileges by changing to the desired non-root uid/gid.
 */

private int set_up_writer_capability() {
  int   ret;
  cap_t c = cap_get_proc();
  const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, CAP_SYS_ADMIN, };

  cap_set_flag(c, CAP_EFFECTIVE, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
  ret = cap_set_proc(c);
  cap_free(c);
  return ret;
}

/*
 * Set the WRITER thread to real-time priority, if RTPRIO is set...
 */

private int set_writer_rt_scheduling() {

  if( writer_parameters.w_schedprio > 0 ) {     /* Then there is RT priority scheduling to set up */
    if( set_rt_scheduling(writer_parameters.w_schedprio) < 0 )
      return -1;

    /* Successfully applied RT scheduling */
    return 1;
  }

  /* RT scheduling not applicable:  no RTPRIO set */
  return 0;
}

/*
 * Debug WRITER parameters
 */

private void debug_writer_params() {
  wparams *wp = &writer_parameters;

  LOG(WRITER, 1, "TMPDIR=%s, SNAPDIR=%s, RTprio=%d;  WOF=%g; "
      "FrameRAM = %d[MiB], ChunkSize = %d[kiB], nFrames = %d, xfrSampleQ = %d[kiB]\n",
      tmpdir_path, wp->w_snapdir, wp->w_schedprio, wp->w_writeahead,
      wp->w_lockedram, wp->w_chunksize, wp_nframes, wp_totxfrsamples*sizeof(sampl_t)/1024);
}

/*
 * --------------------------------------------------------------------------------
 *
 * UTILITY FUNCTIONS USED ONLY BY THE WRITER THREAD
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Test for the presence of a directory by getting a path fd for it.
 */

private int test_directory(int dirfd, const char *name) {
  int ret;

  ret = openat(dirfd, name, O_PATH|O_DIRECTORY); /* Try to open the directory */
  return ret;
}


/*
 * Get a path handle to a directory, creating it if necessary.
 */

private int new_directory(int dirfd, const char *name) {
  int ret;

  ret = test_directory(dirfd, name);    /* Try to open the directory */
  if(ret < 0 ) {
    if( errno != ENOENT )               /* OK if it doesn't exist, otherwise fail */
      return -1;
    ret = mkdirat(dirfd, name, 0750);   /* Didn't exist, try to create it */
    if( ret < 0 )
      return -1;
    ret = openat(dirfd, name, O_PATH|O_DIRECTORY); /* Try again */
    if(ret < 0)                                  /* Give up on failure */
      return -1;
  }
  return ret;
}

/* ================================ Handle the Dir Command ================================ */

/*
 * Snapshot working directory parameter(s), used by the D command line.
 */

private param_t snapwd_params[] ={
#define SNAP_SETWD  0
  { "path",    NULL, NULL,
    PARAM_TYPE(string), PARAM_SRC_CMD,
    "working (sub-)directory for snapshots"
  },
};

private const int n_snapwd_params =  (sizeof(snapwd_params)/sizeof(param_t));

/*
 * Manage the WRITER's 'working directory':  clear the old, resetting to snapdir;
 * find/create and set a new one, clearing an old if necessary.
 */

private void clear_writer_wd() {
  int fd = wp_snap_curfd;

  if( fd != wp_snap_dirfd ) {
    wp_snap_curfd = wp_snap_dirfd;
    close(fd);
  }
}


private int set_writer_new_wd(const char *dir) {
  int fd;

  fd = new_directory(wp_snap_dirfd, dir);
  if(fd < 0)
    return -1;
  wp_snap_curfd = fd;
  return 0;
}

/*
 * Process a D command to change the working directory.  The command
 * comprises an introductory Dir verb followed by a  path=... parameter.
 */

private int process_dir_command(strbuf c) {
  strbuf   e   = strbuf_next(c);
  param_t *ps  = &snapwd_params[0]; 
  int      nps = n_snapwd_params;
  char    *path = NULL;
  int      err;

  /* Initialise the parameter value pointer */
  setval_param(&ps[SNAP_SETWD], (void **)&path);
  err = set_opt_params_from_string(strbuf_string(c), ps, nps);
  if(err < 0) {
    strbuf_appendf(e, "parameter parsing error at position %d", -err);
    reset_param(&ps[SNAP_SETWD]);
    return -1;
  }
  err = assign_param(&ps[SNAP_SETWD]);
  /* If this string copy fails, it's a programming error! */
  assertv(err==0, "Dir PATH parameter assignment failed: %m");
  reset_param(&ps[SNAP_SETWD]);

  if( !path ) {                 /* No path supplied, reset to snapdir */
    clear_writer_wd();
    LOG(WRITER, 1, "Processed Dir, path reset to '%s'\n", writer_parameters.w_snapdir); 
    return 0;
  }

  /* Path is now instantiated to the given parameter string */
  if(set_writer_new_wd(path) < 0) {
    strbuf_appendf(e, "cannot create path=%s: %m", path);
    return -1;
  }

  LOG(WRITER, 1, "Processed Dir, path set to '%s'\n", path); 
  return 0;
}

/* ================================ Handle the Snap command ================================ */

/*
 * Snapshot parameters, used by the S command line.
 * Local to this thread.
 *
 * Note the #defines, which are used to extract the parameter values
 * when building snapshot descriptors -- there is no need to search
 * for the parameter when we know exactly where it is.
 */

private param_t snapshot_params[] ={
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

private const int n_snapshot_params = (sizeof(snapshot_params)/sizeof(param_t));

/*
 * --------------------------------------------------------------------------------
 * FUNCTIONS ETC. TO MANAGE SNAPSHOT DESCRIPTORS
 *
 * The WRITER maintains a list of "active" snapshot descriptors.  A descriptor
 * is created in response to an S command and is "active" until it has been both
 * (a) completely processed and also (b) reported back in response to a Z
 * command.  These data structures are entirely private to the WRITER.
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Allocate and free snap_t structures
 */

private uint16_t snap_counter = 0;

private snap_t *alloc_snapshot() {
  snap_t *ret = calloc(1, sizeof(snap_t));

  if( !snap_counter ) snap_counter++; /* Avoid snapshots called 0000 */
  
  if(ret) {
    init_queue( snap2qp(ret) );
    ret->s_dirfd = -1;
    ret->s_name = snap_counter++;
    init_queue( snap2fq(ret) );
  }
  return ret;
}

private void free_snapshot(snap_t *s) {
  if( !queue_singleton(snap2qp(s)) )
    de_queue(snap2qp(s));
  assertv(queue_singleton(snap2fq(s)),
          "Freeing snapshot %p with non-empty file queue %p", s, queue_next(snap2fq(s)));
  if(s->s_dirfd >= 0)
    close(s->s_dirfd);
  if(s->s_path)
    free((void *)s->s_path);
  free( (void *)s );
}

/* Debugging routine to return unique name */
public const char *snapshot_name(snap_t *s) {
  import  queue *ssQp;
  private int ix = -1;
  private char store[16][8];

  if(snap2qp(s) == ssQp)
    return "SQhead";
  
  ix = (ix+1)&0xf;
  snprintf(&store[ix][0], 8, "s:%04hx", s->s_name);
  return &store[ix][0];
}

/*
 * Manage the WRITER snapshot queue:
 *
 * - Check the parameters in an S command
 */

private int check_snapshot_params(param_t ps[], strbuf e) {
  int ret;

  /* path= is MANDATORY */
  if( !param_isset(&ps[SNAP_PATH]) ) {
    strbuf_appendf(e, "missing PATH parameter");
    return -1;
  }
  ret = assign_param(&ps[SNAP_PATH]);
  /* If this string copy fails, it's a programming error! */
  assertv(ret==0, "Snapshot PATH parameter assignment failed: %m");

  /* EITHER begin= OR start= is MANDATORY */
  if( !param_isset(&ps[SNAP_BEGIN]) && !param_isset(&ps[SNAP_START]) ) {
    strbuf_appendf(e, "neither BEGIN nor START present");
    return -1;
  }

  /* IF begin= THEN end= XOR length= AND NOT finish= is REQUIRED */
  if( param_isset(&ps[SNAP_BEGIN]) ) {
    if( param_isset(&ps[SNAP_FINISH]) ) {
      strbuf_appendf(e, "BEGIN with FINISH present");
      return -1;
    }
    if( !param_isset(&ps[SNAP_END]) && !param_isset(&ps[SNAP_LENGTH]) ) {
      strbuf_appendf(e, "BEGIN but neither END nor LENGTH present");
      return -1;
    }
    if( param_isset(&ps[SNAP_END]) && param_isset(&ps[SNAP_LENGTH]) ) {
      strbuf_appendf(e, "BEGIN with both END and LENGTH present");
      return -1;
    }
    ret = assign_param(&ps[SNAP_BEGIN]); /* Error implies bad number */
    if(ret < 0) {
      strbuf_appendf(e, "cannot assign BEGIN value %s: %m", ps[SNAP_BEGIN].p_str);
      return -1;
    }
    if( param_isset(&ps[SNAP_END]) ) {
      ret = assign_param(&ps[SNAP_END]); /* Error implies bad number */
      if(ret < 0) {
        strbuf_appendf(e, "cannot assign END value %s: %m", ps[SNAP_END].p_str);
        return -1;
      }
    }
    if( param_isset(&ps[SNAP_LENGTH]) ) {
      ret = assign_param(&ps[SNAP_LENGTH]); /* Error implies bad number */
      if(ret < 0) {
        strbuf_appendf(e, "cannot assign LENGTH value %s: %m", ps[SNAP_LENGTH].p_str);
        return -1;
      }
    }
  }

  /* IF start= THEN finish= XOR length= AND NOT end= is REQUIRED */
  if( param_isset(&ps[SNAP_START]) ) {
    if( param_isset(&ps[SNAP_END]) ) {
      strbuf_appendf(e, "START with END present");
      return -1;
    }
    if( !param_isset(&ps[SNAP_FINISH]) && !param_isset(&ps[SNAP_LENGTH]) ) {
      strbuf_appendf(e, "START but neither FINISH nor LENGTH present");
      return -1;
    }
    if( param_isset(&ps[SNAP_FINISH]) && param_isset(&ps[SNAP_LENGTH]) ) {
      strbuf_appendf(e, "START with both FINISH and LENGTH present");
      return -1;
    }
    ret = assign_param(&ps[SNAP_START]); /* Error implies bad number */
    if(ret < 0) {
      strbuf_appendf(e, "cannot assign START value %s: %m", ps[SNAP_START].p_str);
      return -1;
    }
    if( param_isset(&ps[SNAP_FINISH]) ) {
      ret = assign_param(&ps[SNAP_FINISH]); /* Error implies bad number */
      if(ret < 0) {
        strbuf_appendf(e, "cannot assign FINISH value %s: %m", ps[SNAP_FINISH].p_str);
        return -1;
      }
    }
    if( param_isset(&ps[SNAP_LENGTH]) ) {
      ret = assign_param(&ps[SNAP_LENGTH]); /* Error implies bad number */
      if(ret < 0) {
        strbuf_appendf(e, "cannot assign LENGTH value %s: %m", ps[SNAP_LENGTH].p_str);
        return -1;
      }
    }
  }

  /* count= is OPTIONAL */
  if( param_isset(&ps[SNAP_COUNT]) ) {
      ret = assign_param(&ps[SNAP_COUNT]); /* Error implies bad number */
      if(ret < 0) {
        strbuf_appendf(e, "cannot assign COUNT value %s: %m", ps[SNAP_COUNT].p_str);
        return -1;
      }
    }

  /* All required parameters present in legal combination and values parse */
  LOG(WRITER, 2, "Snapshot parameter check successful\n");
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

private void setup_snapshot_samples(snap_t *s, param_t p[]) {

  /* Start with length= -- if present, no finish= or end= spec. needed */
  if( param_isset(&p[SNAP_LENGTH]) ) {  /* Length was stored in s_samples, round up to integral number of pages */
    s->s_bytes = s->s_samples * sizeof(sampl_t);
    s->s_bytes += (sysconf(_SC_PAGE_SIZE) - (s->s_bytes % sysconf(_SC_PAGE_SIZE))) % sysconf(_SC_PAGE_SIZE);
    s->s_samples = s->s_bytes / sizeof(sampl_t);
  }

  /* Mandatory EITHER begin= OR start= -- it was begin= */
  if( param_isset(&p[SNAP_BEGIN]) ) {   /* Begin time was stored in s_first */
    s->s_first = adc_time_to_sample(reader_adc, s->s_first);
    s->s_first = s->s_first - (s->s_first % NCHANNELS);   /* Fix to NCHANNELS boundary */
    if( !s->s_samples ) {                                 /* No length given, need end from s_last */
      s->s_last = adc_time_to_sample(reader_adc, s->s_last);
      s->s_last = s->s_last + ((NCHANNELS - (s->s_last % NCHANNELS)) % NCHANNELS); /* Round up to integral number of channel sweeps */
      s->s_samples = s->s_last - s->s_first;
      s->s_bytes = s->s_samples * sizeof(sampl_t);
      s->s_bytes += (sysconf(_SC_PAGE_SIZE) - (s->s_bytes % sysconf(_SC_PAGE_SIZE))) % sysconf(_SC_PAGE_SIZE);
      s->s_samples = s->s_bytes / sizeof(sampl_t);          /* Round up to integral number of system pages */
    }
    s->s_last = s->s_first + s->s_samples;                  /* Calculate end point using rounded-up sample count */
  }

  /* Mandatory EITHER begin= OR start= -- it was start= */
  if( param_isset(&p[SNAP_START]) ) {   /* Start sample was stored in s_first */
    if( !s->s_samples ) {       /* No length given, need end from s_last */
      s->s_last += ((NCHANNELS - (s->s_last % NCHANNELS)) % NCHANNELS); /* Round up to integral number of channel sweeps */
      s->s_samples = s->s_last - s->s_first;                /* Compute requested length */
      s->s_bytes = s->s_samples * sizeof(sampl_t);
      s->s_bytes += (sysconf(_SC_PAGE_SIZE) - (s->s_bytes % sysconf(_SC_PAGE_SIZE))) % sysconf(_SC_PAGE_SIZE);
      s->s_samples = s->s_bytes / sizeof(sampl_t);          /* Round up to integral number of system pages */
    }
    s->s_last = s->s_first + s->s_samples;                  /* Calculate end point using rounded-up sample count */
  }

  /* Optional count=, default is 1 */
  if( !param_isset(&p[SNAP_COUNT]) ) { /* The count parameter was written to s_count */
    s->s_count = 1;
  }

  s->s_pending = 0;
  s->s_status  = 0;
  LOG(WRITER, 2, "Snapshot samples setup completed for %s\n", snapshot_name(s));
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

private snap_t *build_snapshot_descriptor(strbuf c) {
  strbuf      e   = strbuf_next(c);
  param_t    *ps  = &snapshot_params[0]; 
  int         nps = n_snapshot_params;
  const char *path = NULL;
  snap_t     *ret;
  int         err;
  int         i;
    
  if( !(ret = alloc_snapshot()) ) { /* Allocation failed */
    strbuf_appendf(e, "unable to allocate snapshot descriptor: %m");
    return ret;
  }

  /* Initialise the targets for the parameters */
  setval_param(&ps[SNAP_BEGIN],  (void **) &ret->s_first);
  setval_param(&ps[SNAP_END],    (void **) &ret->s_last);
  setval_param(&ps[SNAP_START],  (void **) &ret->s_first);
  setval_param(&ps[SNAP_FINISH], (void **) &ret->s_last);
  setval_param(&ps[SNAP_LENGTH], (void **) &ret->s_samples);
  setval_param(&ps[SNAP_COUNT],  (void **) &ret->s_count);
  setval_param(&ps[SNAP_PATH],   (void **) &path);

  /* Process the S command parameters */
  err = set_params_from_string(strbuf_string(c), ps, nps);
  if(err < 0) {                 /* Error parsing command string */
    strbuf_appendf(e, "parameter parsing error at position %d", -err);
    goto FAIL;
  }
  /* Check the populated parameters and assign to values */
  err = check_snapshot_params(ps, e);
  if(err < 0) {                 /* Problems put into strbuf by check function */
    goto FAIL;
  }

  if(ret->s_last <= ret->s_first) { /* Parameter error:  end before start */
    strbuf_appendf(e, "end %016llx before start %016llx", ret->s_last, ret->s_first);
    goto FAIL;
  }
  
  /* Path may not already exist */
  ret->s_dirfd = test_directory(wp_snap_curfd, path);
  if(ret->s_dirfd >= 0) {       /* Then directory already exists */
    strbuf_appendf(e, "requested dir path=%s already exists", path);
    goto FAIL;
  }

  if( !adc_is_running(reader_adc) ) {
    strbuf_appendf(e, "data acquisition is currently stopped", path);
    goto FAIL;
  }

  /* Now try to create required directory */
  ret->s_dirfd = new_directory(wp_snap_curfd, path);
  if(ret->s_dirfd < 0) {
    strbuf_appendf(e, "unable to create dir path=%s: %m", path);
    goto FAIL;
  }
  ret->s_path = strdup(path);

  /* Set up the sample-dependent values -- cannot fail */
  setup_snapshot_samples(ret, ps);

  /* Finished with the parameters, their values etc. now */
  for(i=0; i<nps; i++) reset_param(&ps[i]);
  
  /* All done, no errors */
  LOG(WRITER, 1, "New snapshot %s prepared\n", snapshot_name(ret));
  ret->s_status = SNAPSHOT_PREPARE; /* Structure complete but no files/chunks yet... */
  return ret;

 FAIL:
  for(i=0; i<nps; i++) reset_param(&ps[i]);
  LOG(WRITER, 2, "Snapshot %s preparation failed, snapshot freed\n", snapshot_name(ret));
  free_snapshot(ret);
  return NULL;
}

/*
 * Set up snapshot -- create the necessary file descriptor structures etc.
 */

private void setup_snapshot(snap_t *s) {
  snapfile_t *f = alloc_snapfile();

  if(f == NULL) {
    strbuf_appendf(s->s_error, "Failed to allocate file %d/%d", s->s_pending+s->s_done+1, s->s_count);
    s->s_status = SNAPSHOT_ERROR;
    return;
  }
  if( setup_snapfile(f, s) < 0 ) {
    s->s_status = SNAPSHOT_ERROR;
    return;
  }
  s->s_first += s->s_samples; /* Move current sample indices to next file */
  s->s_last  += s->s_samples;
  debug_snapfile(f);
}

/*
 * Called when a snapshot file has just been written.
 */

private void refresh_snapshot(snap_t *s) {

  LOG(WRITER, 2, "Refresh of snapshot %s with status %s\n", snapshot_name(s), snapshot_status(s->s_status));
  if(s->s_status == SNAPSHOT_ERROR) {   /* Tidy up after an error */
    while(s->s_pending) {                       /* There are files that have not got the message */
      assertv(!queue_singleton(snap2fq(s)),
              "Pending file count %d and file header Q mismatch in snapshot %p\n", s->s_pending, s);
      snapfile_t *f = qp2file(queue_next(snap2fq(s)));
      abort_snapfile(f);
      completed_snapfile(f);
    }
    return;
  }
  else if(s->s_done == s->s_count) {    /* No files left to request */
    s->s_status = SNAPSHOT_COMPLETE;
    strbuf_printf(s->s_error, "OK Snap %s: %s %d/%d files", snapshot_name(s), snapshot_status(s->s_status), s->s_done, s->s_count);
    return;
  }
  else if(s->s_done + s->s_pending == s->s_count) {  /* All required files are in progress */
    return;
  }
  else {        /* See if this snapshot should have another file */
    if(wp_nfiles < 2 || s->s_pending == 0) {
      setup_snapshot(s);
    }
  }
}

/*
 * Determine whether a snapshot has completed, i.e. is no longer in progress
 */

private int snapshot_is_complete(snap_t *s) {
  return s->s_status == SNAPSHOT_COMPLETE || s->s_status == SNAPSHOT_ERROR;
}

/*
 * Debugging function for snapshot descriptors...
 */

private void debug_snapshot_descriptor(snap_t *s) {
  LOG(WRITER, 2,
      "Snap s:%04hx at %p: path '%s' fd %d status %s "
      "sQ[%s,%s] "
      "fQ[%s,%s] "
      "files %d/%d/%d "
      "S:%08lx B:%08lx F:%016llx L:%016llx\n",
      s->s_name, s, s->s_path, s->s_dirfd, snapshot_status(s->s_status),
      qp2sname(queue_prev(&s->s_Q)), qp2sname(queue_next(&s->s_Q)),
      qp2fname(queue_prev(&s->s_fileQhdr)), qp2fname(queue_next(&s->s_fileQhdr)),
      s->s_done, s->s_pending, s->s_count, 
      s->s_samples, s->s_bytes, s->s_first, s->s_last);
}

/* ================================ Handle a Z(Status) Command ================================ */

/*
 * Snapshot status request parameter(s), used by the Z command line.
 */

private param_t status_params[] ={
#define SNAP_NAME  0
  { "name",    NULL, NULL,
    PARAM_TYPE(int16), PARAM_SRC_CMD,
    "snapshot name"
  },
};

private const int n_status_params =  (sizeof(status_params)/sizeof(param_t));

/*
 * The snapshot s should report its status as follows.  If it is a
 * pending snapshot, it should append a status line to the given
 * strbuf x.  If it is completed (with or without error) it should
 * transfer its own error strbuf to the chain by inserting it
 * immediately following x.  The idea is that on success the caller
 * will ignore the c strbuf and the chain following will give status
 * reports for completed snapshots..
 */

private void snapshot_report_status(strbuf x, snap_t *s) {

  if(snapshot_is_complete(s)) {  /* If completed, attach its error strbuf */
    queue_ins_after(strbuf2qp(x), strbuf2qp(s->s_error));
    s->s_error = (strbuf)NULL;
    return;
  }
  /* Snapshot is in progress:  append a status line to x */
  strbuf_appendf(x, "Snap %04hx: %s %d/%d/%d\n",
                 s->s_name, snapshot_status(s->s_status),
                 s->s_done, s->s_pending, s->s_count);
}

/*
 * Process a Z command to collect and return snapshot status.  The command
 * comprises an introductory Z verb followed by an optional name=... parameter.
 *
 * The caller has written an initial NO: prefix into the e strbuf, for
 * the error case.  For success, it will rewrite an OK line.  The c
 * strbuf is not cleared here or in the caller, since it is used by
 * snapshot_report_status for snapshots in progress.
 */

private int process_status_command(strbuf c) {
  strbuf   e     = strbuf_next(c);
  param_t *ps    = &status_params[0]; 
  int      nps   = n_status_params;
  uint16_t name  = 0;
  int      err;
  snap_t  *s     = NULL;

  /* Initialise the parameter value pointer */
  setval_param(&ps[SNAP_NAME], (void **)&name);
  err = set_opt_params_from_string(strbuf_string(c), ps, nps);
  if(err < 0) {
    strbuf_appendf(e, "parameter parsing error at position %d", -err);
    reset_param(&ps[SNAP_NAME]);
    return -1;
  }
  err = assign_param(&ps[SNAP_NAME]);
  /* If this string copy fails, it's a programming error! */
  assertv(err==0, "Status NAME parameter assignment failed: %m");
  reset_param(&ps[SNAP_NAME]);

  if(queue_singleton(&snapQ)) { /* There are no snapshots in the queue */
    if(name) {
      strbuf_appendf(e, "Snapshot %hd not found: queue empty", name);
      return -1;
    }
    else {
      strbuf_printf(c, " Files: %d, Xfr space %d[ki] samples\n",
                    wp_nfiles, wp_totxfrsamples/1024);
      return 0;
    }
  }
  
  if(name) {                    /* A spcific snapshot is requested */
    for_nxt_in_Q(queue *p, queue_next(&snapQ), &snapQ)
      if(name == qp2snap(p)->s_name) {
        s = qp2snap(p);
        break;
      }
    end_for_nxt;
    if(s == NULL) {
      strbuf_appendf(e, "Snapshot %hd not found", name);
      return -1;
    }
    /* ... we got one */
    strbuf_printf(c, "\n");
    snapshot_report_status(c, s);
    if(snapshot_is_complete(s)) {  /* If completed, free it */
      LOG(WRITER, 2, "Status of completed snapshot %s returned, snapshot freed\n", snapshot_name(s));
      free_snapshot(s);
    }
  }
  else {        /* Otherwise, look at all the snapshots in the queue */
    /*
     * Note that, the loop below, we alter the queue being traversed
     * since free_snapshot unlinks the current snapshot.  This is OK,
     * since the loop macros have already determined whether the node
     * being worked is the last one or not.
     */
    strbuf_printf(c, " Files: %d, Xfr space %d[ki] samples\n",
                  wp_nfiles, wp_totxfrsamples/1024);

    for_nxt_in_Q(queue *p, queue_next(&snapQ), &snapQ)
      s = qp2snap(p);
      snapshot_report_status(c, s);      /* Report the status of each one */
      if(snapshot_is_complete(s)) {       /* If completed, free it */
        LOG(WRITER, 2, "Status of completed snapshot %s returned, snapshot freed\n", snapshot_name(s));
        free_snapshot(s);
      }
    end_for_nxt;
  }
  LOG(WRITER, 1, "Status command processing succeeded\n");
  return 0;
}

/* =========================== Deal with the Snapshot File Queue ============================== */

/*
 * --------------------------------------------------------------------------------
 * FUNCTIONS ETC. FOR SNAPSHOT FILE DESCRIPTOR STRUCTURES:  ONE OF THESE PER FILE TO CAPTURE.
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Allocate and free snapfile_t structures
 */

private uint16_t snapfile_counter;

private snapfile_t *alloc_snapfile() {
  snapfile_t *ret = calloc(1, sizeof(snapfile_t));

  if(ret) {
    init_queue(&ret->f_Q);
    ret->f_fd = -1;
    ret->f_name = ++snapfile_counter;
  }
  return ret;
}

private void free_snapfile(snapfile_t *f) {
  if(f->f_fd >= 0)
    close(f->f_fd);
  assertv(f->f_chunkQ == NULL, "Freeing snapfile %p with remaining chunks %p\n", f, f->f_chunkQ);
  free((void *)f);
}

/* Debugging routine to return unique name */
public const char *snapfile_name(snapfile_t *f) {
  private int ix = -1;
  private char store[16][8];

  ix = (ix+1)&0xf;
  snprintf(&store[ix][0], 8, "f:%04hx", f->f_name);
  return &store[ix][0];
}

/*
 * Initialise a snapfile_t structure from a snap_t structure.
 */

private int setup_snapfile(snapfile_t *f, snap_t *s) {
  wparams *wp = &writer_parameters;
  int fd;
  int ret;

  f->f_indexnr = s->s_done+s->s_pending;

  snprintf(&f->f_file[0], FILE_NAME_SIZE, "%016llx.s16", s->s_first);
  fd = openat(s->s_dirfd, &f->f_file[0], O_RDWR|O_CREAT|O_EXCL, 0600);
  if(fd < 0) {
    strbuf_appendf(s->s_error, "Unable to open sample file %s in path %s: %m\n", &f->f_file[0], s->s_path);
    return -1;
  }

  ret = ftruncate(fd, s->s_bytes); /* Pre-size the file */
  if(ret < 0) {                    /* Try to tidy up... */
    strbuf_appendf(s->s_error, "Unable to truncate sample file %s to size %d[B]: %m\n", &f->f_file[0], s->s_bytes);
    unlinkat(s->s_dirfd, &f->f_file[0], 0);
    close(fd);
    return -1;
  }

  /* Allocate and initialise the chunks */
  int nc = s->s_bytes / wp->w_chunksize; /* Number of milli-chunks to use (because chunksize is in [kiB] */
  f->f_nchunks = (nc+1023) / 1024;
  f->f_chunkQ  = alloc_chunk(f->f_nchunks);
  if( f->f_chunkQ == NULL ) {
    strbuf_appendf(s->s_error, "Cannot allocate %d chunks for file %s: %m\n", f->f_nchunks, &f->f_file[0]);
    unlinkat(s->s_dirfd, &f->f_file[0], 0);
    close(fd);
    return -1;    
  }

  /* Basic book-keeping entries from here:  no options for failure */
  f->f_fd      = fd;
  f->f_parent  = s;
  f->f_error   = s->s_error;
  f->f_written = 0;

  /*
   * This next variable accounts for the number of samples we have
   * committed to write.  It is initialised by verify() from the
   * locked RAM and overbooking parameters.
   *
   * It is decremented here when we set up a file for capture.  It is
   * later incremented in one of two places: for a successfully
   * written chunk it is incremented by the queue message handler; for
   * a failed chunk, it is incremented by abort_file when it processes
   * the chunks in the file's chunk list.
   */
  
  wp_totxfrsamples -= s->s_samples;

  /* Go through the chunk queue writing in data */
  uint64_t first  = s->s_first;
  uint64_t rest   = s->s_samples;
  uint32_t chunk  = wp_chunksamples;
  uint32_t pagesz = sysconf(_SC_PAGESIZE)/sizeof(sampl_t);
  uint32_t offset = 0;

  for_nxt_in_Q(queue *p, chunk2qp(f->f_chunkQ), chunk2qp(f->f_chunkQ))
    chunk_t *c = qp2chunk(p);
    /* Determine chunk parameters */
    set_chunk_status(c, SNAPSHOT_READY);
    set_chunk_owner(c, CHUNK_OWNER_WRITER);
    c->c_parent  = f;
    c->c_error   = f->f_error;
    c->c_fd      = f->f_fd;
    c->c_ring    = NULL;        /* The ADC object computes this pointer */
    c->c_frame   = NULL;        /* The transfer frames are allocated elsewhere */
    c->c_first   = first;

    /* Deal with final partial chunks */
    if(rest > chunk && rest < 2*chunk) {        /* The penultimate chunk has a partial final chunk */
      chunk = rest / 2;                         /* Split the total remaining data in two */
      chunk = (chunk + pagesz - 1)/pagesz;
      chunk *= pagesz;                          /* Need to ensure file offset is page-aligned */
    }
    if(rest < chunk)            /* The last chunk */
      chunk = rest;

    /* Write in the chunk size and offset values */
    c->c_samples = chunk;
    c->c_last    = first + chunk;
    c->c_offset  = offset;

    /* Compute the placement of the next chunk */
    offset += chunk*sizeof(sampl_t);
    first  += chunk;
    rest   -= chunk;

    /* Add the chunk to the WRITER chunk queue */
    queue *pos = &WriterChunkQ;
    if( !queue_singleton(&WriterChunkQ) ) {
      for_nxt_in_strict_Q(queue *p, queue_next(&WriterChunkQ), &WriterChunkQ);
        chunk_t *h = rq2chunk(p);
        if(h->c_first > c->c_first) {
          pos = p;
          break;
        }
      end_for_nxt;
    }
    queue_ins_before(pos, chunk2rq(c));
    
  end_for_nxt;

  f->f_status = SNAPSHOT_READY;
  s->s_pending++;
  wp_nfiles++;             /* One more file in progress */
  LOG(WRITER, 2, "New file %s set up for snamshot %s\n", snapfile_name(f), snapshot_name(s));
  return 0;
}

/*
 * Completed file descriptor -- called when file acquisition ends,
 * both normally and exceptionally.
 *
 *  We assume that the READER has cleared up any assigned frames when
 * deleting the file chunks in the READER queue.  Therefore, at this
 * point, only the file on disk remains -- remove it if there was an
 * error.  Adjust the book-keeping in the snap_t structure to show
 * this file as done.  Release the chunk descriptors.
 *
 * The file is finally written/gone when the TIDY thread has unmapped
 * the frames released by the READER.
 */

private void completed_snapfile(snapfile_t *f) {
  snap_t *s      = f->f_parent;
  int     status = s->s_status;

  if(f->f_fd >= 0)
    close(f->f_fd);

  s->s_pending--;
  wp_nfiles--;          /* One less file in progress */
  
  release_chunk(f->f_chunkQ);   /* Finished with these now */
  f->f_chunkQ = NULL;
  
  if(f->f_status == SNAPSHOT_ERROR) {
    s->s_status = SNAPSHOT_ERROR;
    unlinkat(s->s_dirfd, &f->f_file[0], 0);  /* If the file failed, remove it */
    LOG(WRITER, 1, "Snapshot %s file %s failed and was unlinked\n", snapshot_name(s), snapfile_name(f));
  }
  else {
    s->s_done++;                /* This file is done, it was pending before */
    s->s_status = SNAPSHOT_WRITING;
    LOG(WRITER, 1, "Snapshot %s file %s completed normally\n", snapshot_name(s), snapfile_name(f));
  }
  de_queue(file2qp(f));         /* Remove this one from the snapshot */
  free_snapfile(f);             /* And free the structure */
  if( status != SNAPSHOT_ERROR )
    refresh_snapshot(s);
}

/*
 * Abort a file from the WRITER thread's viewpoint: remove all chunks
 * from the WRITER's chunk queue and mark the file and all not yet
 * written chunks in ERROR state.
 *
 * N.B. The READER AND WRITER both use the rq chunk linkage, and keep
 * track of who has it by means of exchanged messages and the owner
 * status field.
 *
 * Adjust the w_totxfrsamples parameter for any chunks marked in ERROR
 * state; the WRITTEN chunks were accounted for when received from the
 * READER.
 */

private void abort_snapfile(snapfile_t *f) {

  f->f_status = SNAPSHOT_ERROR;

  assertv(f->f_chunkQ != NULL, "Aborted file %s at %p has an empty chunk queue\n", snapfile_name(f), f);

  for_nxt_in_Q(queue *p, chunk2qp(f->f_chunkQ), chunk2qp(f->f_chunkQ))
    chunk_t *c = qp2chunk(p);

    if( chunk_in_writer(c) ) {
      de_queue(chunk2rq(c));                /* Remove from WRITER chunk queue */
      if( !is_chunk_status(c, SNAPSHOT_WRITTEN) ) {
        if( !is_chunk_status(c, SNAPSHOT_ERROR) ) {
          wp_totxfrsamples += c->c_samples;
        }
        set_chunk_status(c, SNAPSHOT_ERROR);
      }
    }
  end_for_nxt;
  /* Need to do something with the snapshot */
  LOG(WRITER, 2, "File %s of snapshot %s aborted\n", snapfile_name(f), snapshot_name(f->f_parent));
}

/*
 * Check whether a snapfile is alive or not.
 */

public int is_dead_snapfile(snapfile_t *f) {
  return (f->f_status&SNAPSHOT_STATUS_MASK) == SNAPSHOT_ERROR;
}

/*
 * Emit debugging data for a given file descriptor.
 */

private void debug_snapfile(snapfile_t *f) {
  snap_t *s = f->f_parent;
  int     left = MSGBUFSIZE-1,
          used = 0;
  int     i;
  char    buf[MSGBUFSIZE];

  used = snprintf(&buf[used], left,
                  "File %s (f:%04hx) of snapshot s:%04hx, at %p: "
                  "Q [f:%04hx,f:%04hx] "
                  "fd %d ix %d nc %d/%d st %s\n",
                  &f->f_file[0], f->f_name, s->s_name, f,
                  qp2fname(queue_prev(&f->f_Q)), qp2fname(queue_next(&f->f_Q)),
                  f->f_fd, f->f_indexnr, f->f_written, f->f_nchunks, snapshot_status(f->f_status)
                  );
  if(used >= left) used = left;
  left -= used;
  i = 0;
  for_nxt_in_Q(queue *p, chunk2qp(f->f_chunkQ), chunk2qp(f->f_chunkQ))
    int u = snprintf(&buf[used], left,
                     " > %03d: ", i++);
    if(u >= left) u = left;
    used += u;
    left -= u;
    u = debug_chunk(&buf[used], left, qp2chunk(p));
    used += u;
    left -= u;
  end_for_nxt;
  zh_put_multi(logskt_WRITER, 1, &buf[0]);
}

/*
 * --------------------------------------------------------------------------------
 *
 * MAIN LOOP TASKS: deal with command and queue messages as they arrive and transfer
 *                  chunks to the READER when possible.
 *
 *  --------------------------------------------------------------------------------
 */

/*
 * Service the WRITER queue, i.e try to find frames to attach to
 * chunks, and pass such chunks to the READER.  Steps are:
 *
 * - check to see what is in the WRITER queue
 * - allocate at least one frame and pass chunk to READER
 * - loop while time remains...
 *
 * The READER receives messages for chunks that are now in Waiting
 * state, and it returns chunks in either Written state or Error
 * state.
 *
 * Note that when the READER returns a chunk in error state there may
 * be other chunks in transit as messages between the WRITER and
 * READER...  The WRITER needs to keep track of these and make sure
 * they are released in an orderly fashion.
 */

private uint64_t writer_service_queue(uint64_t start) {
  uint64_t now  = start;
  uint64_t stop = start + WRITER_MAX_CHUNK_DELAY;
  int      nc = 0;
  int      max;

  for(max=WRITER_MAX_CHUNKS_TRANSFER; max > 0 && !queue_singleton(&WriterChunkQ) && now < stop; --max, nc++) { /* Only ever do max chunks at the most */
    chunk_t *c = rq2chunk(queue_next(&WriterChunkQ));
    
    if( map_chunk_to_frame(c) < 0 ) {
      if(is_chunk_status(c, SNAPSHOT_ERROR)) { /* Something nasty went wrong! */
        wp_totxfrsamples += c->c_samples;
        abort_snapfile(c->c_parent);
        //      debug_snapfile(c->c_parent);
        completed_snapfile(c->c_parent);
        WARNING(WRITER, "service queue aborts chunk %s: %s\n", c_nstr(c), strbuf_string(c->c_error));
      }
      max = 0;                    /* Couldn't get a frame, so we are done */
    }
    else {                      /* We succeeded */
      de_queue(chunk2rq(c));    /* Hand the chunk over to the READER thread */
      set_chunk_status(c, SNAPSHOT_WAITING);
      c->c_parent->f_pending++;
      set_chunk_owner(c, CHUNK_OWNER_READER);
      send_object_ptr(reader, (void *)&c);
      LOG(WRITER, 2, "service queue transfers chunk %s to READER\n", c_nstr(c));
    }
    now = monotonic_ns_clock();
  }
  if(nc) {
    LOG(WRITER, 2, "service queue did %d chunks in %d[ns]\n", nc, (int)(now-start));
  }
  return now;                   /* Current end-of-loop time */
}

/*
 * Deal with a queue message from the READER thread.  These messages
 * are chunk pointers and fall into two disjoint classes.  In either
 * case any chunk received here has been detached from the READER's
 * chunk queue and its frame has been released.
 *
 * - a chunk in SNAPSHOT_WRITTEN state:
 *   release the write commitment.
 *   
 * If this was the last chunk of a snapfile, then run completed_snapfile.
 *
 * - a chunk in SNAPSHOT_ERROR state:
 *   abort the snapfile.
 *
 * In this case the READER will have released all chunks in its queue
 * and marked them in SNAPSHOT_ERROR state so that the abort_snapfile
 * routine can clean them up.  Chunks owned by the READER will be
 * moved into ERROR state by the READER, so are not tidied by
 * abort_snapfile which runs for the first erroneous chunk.  The
 * snapfile structure is tidied by completed_snapfile which runs when
 * the last pending chunk is returned.
 */

private int process_reader_message(void *s) {
  chunk_t    *c;
  snapfile_t *f;
  
  /* We are expecting a chunk pointer message */
  recv_object_ptr(s, (void **)&c);
  assertv(c != NULL, "Queue message from READER was NULL pointer\n");
  
  f = c->c_parent;
  f->f_pending--;
  wp_totxfrsamples += c->c_samples;

  LOG(WRITER, 3, "chunk %s received from READER\n", c_nstr(c));
  if(is_chunk_status(c, SNAPSHOT_WRITTEN)) {
    set_chunk_owner(c, CHUNK_OWNER_WRITER);
    f->f_written++;
    if(f->f_written == f->f_nchunks) {
      f->f_status = SNAPSHOT_WRITTEN;
      completed_snapfile(f);    /* This file is finished -- all chunks were written */
    }
    return true;      
  }
  if(is_chunk_status(c, SNAPSHOT_ERROR)) {
    if(f->f_status != SNAPSHOT_ERROR)
      abort_snapfile(f);        /* Tidy the chunk list, marking all WRITER-owned chunks into SNAPSHOT_ERROR state */
    set_chunk_owner(c, CHUNK_OWNER_WRITER); /* Needs to be after the abort_snapfile() to avoid dual counting */
    if(f->f_pending == 0)
      completed_snapfile(f);    /* This file is finished -- no pending chunks in transit */
    return true;
  }
  
  assertv(false, "Chunk %s received in unexpected state %s\n", c_nstr(c), snapshot_status(c->c_status));
}

/* ================================ Process Command Messages ================================ */

private int process_writer_command(void *s) {
  int     ret;
  strbuf  cmd;
  char   *cmd_buf;
  strbuf  err;

  ret = recv_object_ptr(s, (void **)&cmd);
  if( !ret ) {                  /* Quit */
    return false;
  }

  cmd_buf = strbuf_string(cmd);
  err = strbuf_next(cmd);

  switch(cmd_buf[0]) {
  case 'd':                     /* Dir command */
  case 'D':
    /* Call the command handler for Dir */
    strbuf_printf(err, "NO: Dir -- ");
    ret = process_dir_command(cmd);
    if(ret == 0) {
      strbuf_printf(err, "OK Dir");
      strbuf_clear(cmd);
    }
    break;

  case 'z':
  case 'Z':
    strbuf_printf(err, "NO: Ztatus -- ");
    ret = process_status_command(cmd);
    if(ret == 0) {
      strbuf_printf(err, "OK Ztatus:");
    }
    break;

  case 's':                     /* Snap command */
  case 'S':
    /* Try to build a snapshot descriptor */
    strbuf_printf(err, "NO: Snap -- ");
    snap_t *s = build_snapshot_descriptor(cmd);
    if(s != NULL) {                 /* Snapshot building succeeded */
      queue_ins_after(&snapQ, snap2qp(s));
      strbuf_printf(err, "OK Snap %04hx ", s->s_name);
      s->s_error = (strbuf)de_queue((queue *)cmd);
      strbuf_clear(cmd);
      debug_snapshot_descriptor(s);
      refresh_snapshot(s);
    }
    else {
      ret = -1;
    }
    break;

  default:
    strbuf_printf(err, "NO: WRITER -- unexpected WRITER command");
    ret = -1;
    break;
  }

  if(ret < 0) {
    strbuf_revert(cmd);
    LOG(WRITER, 1, "%s\n > '%s'\n", strbuf_string(err), &cmd_buf[0]); /* Error occurred, log the problem */
    strbuf_clear(cmd);
  }
  send_object_ptr(s, (void *)&err);
  return true;
}

/*
 * WRITER thread message loop
 */

private void writer_thread_msg_loop() {    /* Read and process messages */
  int borrowedtime;
  int running;
  int n;

  zmq_pollitem_t  poll_list[] =
    {  { reader, 0, ZMQ_POLLIN, 0 },
       { command, 0, ZMQ_POLLIN, 0 },
    };
#define N_POLL_ITEMS    (sizeof(poll_list)/sizeof(zmq_pollitem_t))
  int (*poll_responders[N_POLL_ITEMS])(void *) =
    { process_reader_message,
      process_writer_command,
    };

  /* WRITER initialisation is complete */
  writer_parameters.w_running = !die_die_die_now;
  LOG(WRITER, 1, "thread is initialised");

  running = writer_parameters.w_running;
  borrowedtime = 0;             /* Keeps track of the number of [ms] we owe */

  while( running && !die_die_die_now ) {
    int delay = borrowedtime + WRITER_POLL_DELAY; /* WRITER_POLL_DELAY is how long we wait normally in [ms] */

    if(delay > WRITER_POLL_DELAY) /* Ensure loop responsiveness: upper bound on requested delay */
      delay = WRITER_POLL_DELAY;

    int ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, (delay<=0? 0 : delay));

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      WARNING(WRITER, "thread message loop interrupted");
      break;
    }
    if(ret < 0)
      break;

    /* If we did some waiting, we owe no time but are up-to-date */
    borrowedtime = (delay >= 0? 0 : delay);

    /*
     * Delays may occur in the message service routines because of
     * page faults when responding to commands: hence the mechanism
     * with borrowedtime.
     */
    uint64_t tick = monotonic_ns_clock();
    
    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
        if( !(*poll_responders[n])(poll_list[n].socket) )
          running = false;
      }
    }
    
    uint64_t tock = writer_service_queue(tick);
    borrowedtime -= (tock-tick+500000)/1000000; /* Rounded elapsed time in [ms] */
  }
}

/* ================================ Thread Startup ================================ */

/*
 * WRITER thread main routine
 */

public void *writer_main(void *arg) {
  int ret;

  create_writer_comms();

  if( set_up_writer_capability() < 0 ) {
    WARNING(WRITER, "thread capabilities are deficient");
  }

  if( check_effective_capabilities_ok() < 0 ) {
    WARNING(WRITER, "thread fails to set effective capabilities");
  }

  ret = set_writer_rt_scheduling();
  switch(ret) {
  case 1:
    LOG(WRITER, 1, "RT scheduling succeeded");
    break;
  case 0:
    LOG(WRITER, 1, "using normal scheduling: RTPRIO unset");
    break;
  default:
    WARNING(WRITER, "RT scheduling setup failed: %s", strerror(errno));
    break;
  }

  debug_writer_params();
  writer_thread_msg_loop();
  LOG(WRITER, 1, "thread terminates by return");

  /* Clean up our ZeroMQ sockets */
  close_writer_comms();
  writer_parameters.w_running = false;
  return (void *)NULL;
}

/*
 * Verify the parameters for the WRITER and construct the WRITER state.
 *
 * Called by the MAIN thread during start up initialisation.
 */

public int verify_writer_params(wparams *wp, strbuf e) {
  import int tmpdir_dirfd;      /* Imported from snapshot.c */
  int ret;

  if( wp->w_schedprio != 0 ) {  /* Check for illegal value */
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
    strbuf_appendf(e, "Transfer chunk size %d KiB outwith compiled-in range [%d, %d] [kiB]",
                  wp->w_chunksize, MIN_CHUNK_SZ, MAX_CHUNK_SZ);
    return -1;
  }

  /* Compute the number of frames available */
  const int pagesize = sysconf(_SC_PAGESIZE);
  int sz = wp->w_chunksize*1024;
  int nfr;
  sz = pagesize * ((sz + pagesize - 1) / pagesize); /* Round up to multiple of PAGE SIZE */
  wp->w_chunksize = sz / 1024;
  nfr = (wp->w_lockedram * 1024*1024) / sz;         /* Number of frames that fit in locked RAM */
  if(nfr < MIN_NFRAMES) {
    strbuf_appendf(e, "Adjusted chunk size %d KiB and given RAM %d MiB yield too few (%d < %d) frames",
                   wp->w_chunksize, wp->w_lockedram, nfr, MIN_NFRAMES);
    return -1;
  }
  wp_nframes = nfr;
  wp_chunksamples = wp->w_chunksize * 1024 / sizeof(sampl_t);

  /*
   * Check the writeahead fraction -- this is the proportion by which
   * the locked transfer RAM may be "overbooked".  Should be positive
   * and not too big :-)).
   */

  if(wp->w_writeahead < 0 || wp->w_writeahead > 1) {
    strbuf_appendf(e, "Transfer writeahead fraction %g out of compiled-in range [0,1]", wp->w_writeahead);
    return -1;
  }
  wp_totxfrsamples = nfr*wp->w_chunksize*1024*(1 + wp->w_writeahead) + pagesize-1;
  wp_totxfrsamples = pagesize * (wp_totxfrsamples / pagesize);
  wp_totxfrsamples = wp_totxfrsamples / sizeof(sampl_t);

  wp_nfiles = 0;                /* Currently no files in progress */

  /*
   * Check the snapdir directory exists and get a path fd for it.
   * Assumes we are already running as the non-privileged user.
   */
  wp_snap_dirfd = new_directory(tmpdir_dirfd, wp->w_snapdir);
  if( wp_snap_dirfd < 0 ) {     /* Give up on failure */
    strbuf_appendf(e, "Snapdir %s inaccessible: %m", wp->w_snapdir);
    return -1;
  }
  wp_snap_curfd = wp_snap_dirfd; /* Initial default */

  /*
   * Now try to get the memory for the transfer RAM...  This maps in a set of
   * anonymous pages so requires CAP_IPC_LOCK capability.
   */
  ret = init_frame_system(e, nfr, wp->w_lockedram, wp->w_chunksize);
  return ret;
}
