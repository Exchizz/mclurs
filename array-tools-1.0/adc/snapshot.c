#

#include "general.h"

#define _GNU_SOURCE	/* Linux-specific code below (O_PATH) */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/capability.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <argtable2.h>
#include "argtab.h"

#include <zmq.h>
#include <pthread.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "assert.h"
#include <sched.h>

#include <comedi.h>
#include <comedilib.h>

#include "util.h"
#include "param.h"
#include "queue.h"
#include "strbuf.h"
#include "chunk.h"
#include "rtprio.h"
#include "snapshot.h"
#include "reader.h"
#include "writer.h"
#include "tidy.h"

/*
 * Snapshot version
 */

#define PROGRAM_VERSION	"1.1"
#define VERSION_VERBOSE_BANNER	"MCLURS ADC toolset...\n"

/*
 * Global parameters for the snapshot program
 */

public int die_die_die_now = 0;

import  rparams     reader_parameters;
import  wparams	   writer_parameters;
import  const char *tmpdir_path;
private const char *snapshot_addr;
private const char *snapshot_user;
private const char *snapshot_group;
private int	   schedprio;

public param_t globals[] ={
  { "tmpdir",   "/tmp",
    &tmpdir_path,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "directory for creation of temporary files"
  },
  { "freq",     "312.5e3",
    &reader_parameters.r_frequency,
    PARAM_TYPE(double), PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "sampling frequency (divided by 8) of the ADC [Hz]"
  },
  { "snapshot", "ipc://snapshot-CMD",
    &snapshot_addr,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
  { "snapdir",  "snap",
    &writer_parameters.w_snapdir,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "directory where samples are written"
  },
  { "dev",	"/dev/comedi0",
    &reader_parameters.r_device,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the Comedi device to open"
  },
  { "range",	"750",
    &reader_parameters.r_range,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "the ADC converter full-scale range [mV]"
  },
  { "bufsz",	"56",
    &reader_parameters.r_bufsz,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the Comedi buffer [MiB]"
  },
  { "window",	"10",
    &reader_parameters.r_window,
    PARAM_TYPE(double),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "guaranteed window in the ring buffer [s]"
  },
  { "bufhwm",	"0.9",
    &reader_parameters.r_buf_hwm_fraction,
    PARAM_TYPE(double), PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "ring buffer high-water mark fraction"
  }, 
  { "rtprio",	NULL,
    &schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time threads [0-99]"
  },
  { "rdprio",	NULL,
    &reader_parameters.r_schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time reader thread [0-99]"
  },
  { "wrprio",	NULL,
    &writer_parameters.w_schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time writer thread [0-99]"
  },
  { "user",	NULL,
    &snapshot_user,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "user/UID for file system access and creation"
  },
  { "group",	NULL,
    &snapshot_group,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "group/GID for file system access and creation"
  },
  { "ram",	"64",
    &writer_parameters.w_lockedram,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "amount of data RAM to lock [MiB]"
  },
  { "wof",	"0.5",
    &writer_parameters.w_writeahead,
    PARAM_TYPE(double), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "write overbooking fraction"
  },
  { "chunk",	"1024",
    &writer_parameters.w_chunksize,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "size of a transfer chunk [KiB]"
  },
};

public const int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

public int   verbose;
public char *program   = NULL;

/* Command line syntax options -- there are no mandatory arguments on the main command line! */

private struct arg_lit *h1, *vn1, *v1, *q1;
private struct arg_end *e1;

BEGIN_CMD_SYNTAX(help) {
  v1  = arg_litn("v",  "verbose", 0, 3,	"Increase verbosity"),
  q1  = arg_lit0("q",  "quiet",		"Decrease verbosity"),
  h1  = arg_lit0("h",  "help",		"Print usage help message"),
  vn1 = arg_lit0(NULL, "version",	"Print program version string"),
  e1  = arg_end(20)
} APPLY_CMD_DEFAULTS(help) {
  /* No defaults to apply here */
} END_CMD_SYNTAX(help)

private struct arg_lit *v2, *q2;
private struct arg_end *e2;

BEGIN_CMD_SYNTAX(main) {
  v2  = arg_litn("v",  "verbose", 0, 3,	      "Increase verbosity"),
  q2  = arg_lit0("q",  "quiet",		      "Decrease verbosity"),
        arg_str0("s",  "snapshot", "<url>",   "URL of snapshotter command socket"),
        arg_str0(NULL, "tmpdir", "<path>",    "Path to temporary directory"),
        arg_str0("S",  "snapdir", "<path>",   "Path to samples directory"),
        arg_dbl0("f",  "freq", "<real>",      "Per-channel sampling frequency [Hz]"),
        arg_dbl0("w",  "window", "<real>",    "Min. capture window length [s]"),
        arg_dbl0("B",  "bufhwm", "<real>",    "Ring buffer High-water mark fraction"),
        arg_str0("d",  "dev", "<path>",       "Comedi device to use"),
        arg_int0("P",  "rtprio", "<1-99>",    "Common thread RT priority"),
        arg_int0("R",  "rdprio", "<1-99>",    "Reader thread RT priority"),
        arg_int0("W",  "wrprio", "<1-99>",    "Writer thread RT priority"),
        arg_str0("u",  "user", "<uid/name>",  "User to run as"),
        arg_str0("g",  "group", "<gid/name>", "Group to run as"),
        arg_int0("b",  "bufsz", "<int>",      "Comedi ring buffer Size [MiB]"),
        arg_int0("m",  "ram", "<int>",        "Data Transfer RAM size [MiB]"),
        arg_int0("r",  "range", "<int>",      "ADC full-scale range [mV]"),
        arg_int0("c",  "chunk", "<int>",      "File transfer chunk size [kiB]"),
        arg_dbl0("W",  "wof", "<real>",       "Write Overbooking Fraction"),
  e2  = arg_end(20)
} APPLY_CMD_DEFAULTS(main) {
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params);
} END_CMD_SYNTAX(main);

/* Standard help routines: display the version banner */
private void print_version(FILE *fp, int verbosity) {
  fprintf(fp, "%s: Vn. %s\n", program, PROGRAM_VERSION);
  if(verbosity > 0) {		/* Verbose requested... */
    fprintf(fp, VERSION_VERBOSE_BANNER);
  }
}

/* Standard help routines: display the usage summary for a syntax */
private void print_usage(FILE *fp, void **argtable, int verbosity, char *program) {
  if( !verbosity ) {
    fprintf(fp, "Usage: %s ", program);
    arg_print_syntax(fp, argtable, "\n");
    return;
  }
  if( verbosity ) {
    char *suffix = verbosity>1? "\n\n" : "\n";
    fprintf(fp, "Usage: %s ", program);
    arg_print_syntaxv(fp, argtable, suffix);
    if( verbosity > 1 )
      arg_print_glossary(fp, argtable, "%-25s %s\n");
  }
}

/*
 * Snapshot globals for this file.
 */

private const char *snapshot_addr = NULL;  /* The address of the main command socket */
private const char *snapshot_user = NULL;  /* The user we should run as, after startup */
private const char *snapshot_group = NULL; /* The group to run as, after startup */
private int	   schedprio;		  /* Real-time priority for reader and writer */

/*
 * Snapshot globals shared between threads
 */

public void       *snapshot_zmq_ctx;	/* ZMQ context for messaging -- created by the TIDY thread */

public int	    tmpdir_dirfd;	/* The file descriptor obtained for the TMPDIR directory */
public const char *tmpdir_path;		/* The path for the file descriptor above */

/*
 * Thread handles for reader and writer
 */

private pthread_t reader_thread,
		  writer_thread,
		  tidy_thread;

private pthread_attr_t reader_thread_attr,
		       writer_thread_attr,
		       tidy_thread_attr;

/*
 * Establish main comms:  this routine runs last, so it mostly does connect() calls.
 * It must run when the other three threads are already active.
 */

private void *log_socket;	/* N.B.  This socket is opened by the TIDY thread, but not used there */
private void *reader;
private void *writer;
private void *command;

private int create_main_comms() {
  /* Create and initialise the sockets: reader and writer command sockets */
  reader = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_REQ, READER_CMD_ADDR);
  if( reader == NULL ) {
    fprintf(stderr, "%s: Error -- unable to connect internal socket to reader: %s\n", program, strerror(errno));
    return -1;
  }
  writer = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_REQ, WRITER_CMD_ADDR);
  if( writer == NULL ) {
    fprintf(stderr, "%s: Error -- unable to connect internal socket to writer: %s\n", program, strerror(errno));
    return -1;
  }

  /* Create and initialise the external command socket */
  command = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_REP, snapshot_addr);
  if( command == NULL ) {
    fprintf(stderr, "%s: Error -- unable to bind external command socket %s: %s\n",
	    program, snapshot_addr, strerror(errno));
    return -1;
  }

  return 0;
}

/* Close everything created above */

private void close_main_comms() {
  zmq_close(reader);
  zmq_close(writer);
  zmq_close(command);
}

/*
 * Sort out the capabilities required by the process.  (If not running
 * as root, check that we have the capabilities we require.)  Release
 * any capabilities not needed and lock against dropping privilege.
 *
 * The threads need the following capabilities:
 *
 * CAP_IPC_LOCK  (Reader and Writer) -- ability to mmap and mlock pages.
 * CAP_SYS_NICE  (Reader and Writer) -- ability to set RT scheduling priorities
 * CAP_SYS_ADMIN (Reader) -- ability to set (increase) the Comedi buffer maximum size
 * CAP_SYS_ADMIN (Writer) -- ability to set RT IO scheduling priorities (unused at present)
 * CAP_SYS_ADMIN (Tidy)   -- ability to set RT IO scheduling priorities (unused at present)
 *
 * CAP_SETUID (Main)
 * CAP_SETGID (Main)	  -- ability to change user ID
 *
 * Otherwise the main thread and the tidy thread need no special powers.  The ZMQ IO thread
 * is also unprivileged, and is currently spawned during context creation from tidy.
 */

private int snap_adjust_capabilities() {
  cap_t c = cap_get_proc();
  uid_t u = geteuid();
  int ret = 0;

  if( !c )			/* No memory? */
    return -1;

  if( check_permitted_capabilities_ok() < 0 ) {
    fprintf(stderr, "%s: Error -- I do not have the necessary capabilities to operate\n", program);
    return -1;
  }

  if( !u ) {
    const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, CAP_SYS_ADMIN, CAP_SYS_RESOURCE, CAP_SETUID, CAP_SETGID, };

    /* So we are root and have the capabilities we need.  Prepare to drop the others... */
    /* Keep the EFFECTIVE capabilities as long as we stay root */
    cap_clear(c);
    cap_set_flag(c, CAP_PERMITTED, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
    cap_set_flag(c, CAP_EFFECTIVE, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
    if( prctl(PR_SET_KEEPCAPS, 1L) <0 ) {
      cap_free(c);
      fprintf(stderr, "%s: Error -- unable to keep required capabilities on user change\n", program);
      return -1;
    }

    ret = cap_set_proc(c);
  }

  cap_free(c);
  return ret;
}

/*
 * Drop privileges and capabilities when appropriate.
 */

private int main_adjust_capabilities(uid_t uid, gid_t gid) {
  cap_t c = cap_get_proc();
  const cap_value_t vs[] = { CAP_SETUID, CAP_SETGID, };
  
  /* Drop all capabilities except CAP_SETUID/GID and CAP_SYS_RESOURCE from effective set */

  if(c) {
    cap_clear_flag(c, CAP_EFFECTIVE);
    cap_set_flag(c, CAP_EFFECTIVE, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
    if( cap_set_proc(c) < 0 ) {
      cap_free(c);
      fprintf(stderr, "%s: Error -- MAIN thread fails to clear capabilities: %s\n", program, strerror(errno));
      return -1;
    }
    cap_free(c);
  }
  
  /* Drop all user and group privileges:  set all uids to uid and all gids to gid */
  /* Complain if that fails -- we were not root and uid/gid were not in our set */
  if( setresgid(gid, gid, gid) < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread unable to change to gid %d: %s\n", program, gid, strerror(errno));
    return -1;
  }

  /* Retrieve and initialise the subsidiary group memberships for the given user */
  if( initgroups(snapshot_user, gid) < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread unable to set subsidiary groups for uid %d: %s\n", program, uid, strerror(errno));
    return -1;
  }
  
  if( setresuid(uid, uid, uid) < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread unable to change to uid %d: %s\n", program, uid, strerror(errno));
    return -1;
  }

  c = cap_get_proc();
  if(c) {
    cap_set_flag(c, CAP_PERMITTED, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_CLEAR);
    if( cap_set_proc(c) < 0 ) {
      cap_free(c);
      fprintf(stderr, "%s: Error -- MAIN thread keeps setuid/gid capabilities: %s\n", program, strerror(errno));
      return -1;
    }
    cap_free(c);    
  }
  
  /* Now check we still have the required permitted capabilities */
  if( check_permitted_capabilities_ok() < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread lost capabilities on changing user!\n", program);
    return -1;
  }

  return 0;
}

/*
 * Deal nicely with the interrupt signal.
 * Basically, the signal sets the die_die_die_now flag which the various threads notice.
 * CURRENTLY NOT WORKING PROPERLY SO DISABLED
 */

private void intr_handler(int i) {
  die_die_die_now++;
}

private int set_intr_sig_handler() {
  struct sigaction a;

  bzero(&a, sizeof(a));
  a.sa_handler = intr_handler;
  if( sigaction(SIGINT, &a, NULL) < 0 ) {
    fprintf(stderr, "%s: Error -- unable to install INT signal handler: %s\n", program, strerror(errno));
    return -1;
  }
  return 0;
}

/*
 * Process a (possibly multipart) log message.
 * Collect the various pieces and write to stderr
 */

#define LOGBUF_SIZE	MSGBUFSIZE
private char pfx[] = "Log: ";

private int process_log_message(void *s) {
  char log_buffer[MSGBUFSIZE];
  int used;
  
  memcpy(&log_buffer[0], &pfx[0], sizeof(pfx));
  used = sizeof(pfx)-1;
  used += zh_collect_multi(s, &log_buffer[used], LOGBUF_SIZE-1, "");
  if( log_buffer[used-1] != '\n') {
    log_buffer[used] = '\n';
    fwrite(log_buffer, used+1, 1, stderr);
  }
  else {
    fwrite(log_buffer, used, 1, stderr);
  }
  fflush(stderr);
  return 0;
}

/*
 * Handle replies from READER and WRITER threads.  The reply message
 * is a pointer to a set of error strbufs.  We collect and join all
 * the strings in the reply buffer.  The collector maintains as
 * invariant that "used==0 || reply_buffer[used-1] is not NUL" and that
 * "b == &reply_buffer[used]".
 */

#define REPLY_BUFSIZE	MSGBUFSIZE
private char reply_buffer[REPLY_BUFSIZE];

private int process_reply(void *s) {
  strbuf  err;
  char   *b = &reply_buffer[0];
  int     used;
  
  recv_object_ptr(s, (void **)&err);

  /* Establish invariants */
  *b = '\0';  used = 0;
  
  /* Traverse the strbuf chain once collecting data, then release */
  for_nxt_in_Q(queue *q, strbuf2qp(err), (queue *)NULL)
    strbuf  s = qp2strbuf(q);
    int     n = strbuf_used(s);
    if(n) {				/* Empty strbuf, nothing to do */
      strbuf_revert(s);			/* Remove any internal NUL characters */
      if(n > REPLY_BUFSIZE-used) {	/* There is too much data */
	n = REPLY_BUFSIZE-used-1;	/* We can manage this much of it */
      }
      //      fprintf(stderr, "strbuf %p, used %d, ptr %p, string '%s'\n",
      //	      s, n, b, strbuf_string(s));
      memcpy(b, strbuf_string(s), n);	/* Copy the data */
      b += n;  used += n;		/* Now we have used this much space */
      //      while( b[-1] == '\0' ) b--,used--;	/* Skip back over any NULs */
      //      fprintf(stderr, "strbuf %p, ptr now %p, total used now %d\n", s, b, used);
    }
  end_for_nxt;
  
  release_strbuf(err);	/* Free the entire link of strbufs */

  if( b[-1] != '\n' )	/* Ensure final newline */
    *b = '\n';

  /* Send the complete reply */
  used = b - &reply_buffer[0];
  zh_put_msg(command, 0, used, &reply_buffer[0]);
  return 0;
}

/*
 * Handle commands sent to the snapshotter.  These are forwarded
 * either to the reader thread or the writer thread, and their replies
 * are returned to the originator.  Using the REP socket ensures only
 * one outstanding message is in process, so simplifies the reply routing.
 */

private int process_snapshot_command() {
  strbuf c,e;			/* Command and Error buffers */
  char  *buf;
  int   size, ret;
  int   fwd;

  c = alloc_strbuf(2);
  e = strbuf_next(c);

  buf = strbuf_string(c);
  size = zh_get_msg(command, 0, strbuf_space(c), buf);
  if( !size ) {
    ret = zh_put_msg(command, 0, 0, NULL); /* If empty message received, send empty reply at once */
    release_strbuf(c);
    assertv(ret == 0, "Reply to command failed, %d\n", ret);
    return 0;
  }
  strbuf_setpos(c, size);
  buf[size] = '\0';
  // fprintf(stderr, "Msg '%c' (%d)\n", buf[0], buf[0]);
  fwd = 0;
  switch(buf[0]) {
  case 'q':
  case 'Q':			/* Deal specially with Quit command, to close down nicely... */
    send_object_ptr(reader, NULL); /* Forward zero length message to the READER thread */
    send_object_ptr(writer, NULL); /* Forward zero length message to the WRITER thread */
    ret = zh_put_msg(command, 0, 7, "OK Quit"); /* Reply to Quit here */
    assertv(ret == 7, "Quit reply failed, %d\n", ret);
    break;

  case 'g':
  case 'G':
  case 'h':
  case 'H':
  case 'i':
  case 'I':
  case 'p':
  case 'P':
    /* Forward these commands to the READER thread */
    send_object_ptr(reader, (void *)&c);
    fwd++;
    break;

  case 'd':
  case 'D':
  case 's':
  case 'S':
  case 'z':
  case 'Z':
    /* Forward snapshot and dir commands to WRITER */
    send_object_ptr(writer, (void *)&c);
    fwd++;
    break;

  case '?':
    buf[0] = '!';
    ret = zh_put_msg(command, 0, size, buf); /* Reply to 'ping' message */
    assertv(ret > 0, "Reply to ping failed, %d\n", ret);
    break;

  default:
    strbuf_printf(e, "NO: Unknown command: '%s'\n", buf);
    fprintf(stderr, "%s: %s", program, strbuf_string(e));
    ret = zh_put_msg(command, 0, strbuf_used(e) , strbuf_string(e));
    assertv(ret == strbuf_used(e), "Reject unknown reply failed, %d\n", ret);
    break;
  }
  if( !fwd )			/* Didn't use the strbufs */
    release_strbuf(c);
  return 0;
}

/*
 * MAIN thread message loop
 */

#define	MAIN_LOOP_POLL_INTERVAL	20

private void main_thread_msg_loop() {    /* Read and process messages */
  int poll_delay;
  int running;
  zmq_pollitem_t  poll_list[] =
    { { log_socket, 0, ZMQ_POLLIN, 0 },
      { command, 0, ZMQ_POLLIN, 0 },
      { reader, 0, ZMQ_POLLIN, 0 },
      { writer, 0, ZMQ_POLLIN, 0 },
    };
#define  N_POLL_ITEMS  (sizeof(poll_list)/sizeof(zmq_pollitem_t))
  int (*poll_responders[N_POLL_ITEMS])(void *) =
    { process_log_message,
      process_snapshot_command,
      process_reply,
      process_reply,
    };

  fprintf(stderr, "Log: starting MAIN thread polling loop with %d items\n", (int)N_POLL_ITEMS);
  running = true;
  poll_delay = MAIN_LOOP_POLL_INTERVAL;
  while(running) {
    int n;
    int ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, poll_delay);

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      fprintf(stderr, "%s: MAIN thread loop interrupted\n", program);
      continue;
    }
    if(ret < 0)
      break;
    running = reader_parameters.r_running || writer_parameters.w_running;
    if( !running )		/* Flush out last (log) messages */
      poll_delay = 100;
    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
	ret = (*poll_responders[n])(poll_list[n].socket);
	assertv(ret >= 0, "Error in message processing in MAIN poll loop, ret %d\n", ret);
	running = true;
      }
    }
  }
}

/*
 * Snapshot main routine.
 */

public int main(int argc, char *argv[], char *envp[]) {
  char *thread_return = NULL;
  int ret;

  program = argv[0];

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, environment, then command-line argument. */
  set_param_from_env(envp, globals, n_global_params);

  /* 2. Process parameters:  push values out to program globals */
  ret = assign_all_params(globals, n_global_params);
  assertv(ret == 0, "Push parameters failed on param %d out of %d\n", -ret, n_global_params);

  if(verbose > 2) {
    fprintf(stderr, "Params before cmdline...\n");
    debug_params(stderr, globals, n_global_params);
  }

  /* 3. Create and parse the command lines -- installs defaults from parameter table */
  void **cmd_help = arg_make_help();
  void **cmd_main = arg_make_main();

  /* Try first syntax -- reject empty command lines */
  int err_help = arg_parse(argc, argv, cmd_help);
  if( !err_help && (vn1->count || h1->count) ) {	/* Assume this was the desired command syntax */
    if(vn1->count)
      print_version(stdout, v1->count);
    if(h1->count || !vn1->count) {
      print_usage(stdout, cmd_help, v1->count>0, program);
      print_usage(stdout, cmd_main, v1->count, program);
    }
    exit(0);
  }

  /* Try second syntax -- may be empty, means use default or environment variable parameters */
  int err_main = arg_parse(argc, argv, cmd_main);
  if( err_main ) {		/* This is the default desired syntax; give full usage */
    arg_print_errors(stderr, e2, program);
    print_usage(stderr, cmd_help, v2->count>0, program);
    print_usage(stderr, cmd_main, v2->count, program);
    exit(1);
  }

  verbose = v2->count - q2->count;
  if(verbose > 2) {
    fprintf(stderr, "Params before reverse pass...\n");
    debug_params(stderr, globals, n_global_params);
  }
  
  /* 4. Process parameters:  copy argument values back through the parameter table */
  ret = arg_results_to_params(cmd_main, globals, n_global_params);

  /* 5. Process parameters:  deal with non-parameter table arguments where necessary */

  if(verbose > 1) {
    fprintf(stderr, "Params before checking...\n");
    debug_params(stderr, globals, n_global_params);
  }

  /* 5a. Verify parameters required by the main program/thread */
  tmpdir_dirfd = open(tmpdir_path, O_PATH|O_DIRECTORY); /* Verify the TMPDIR path */
  if( tmpdir_dirfd < 0 ) {
    fprintf(stderr, "%s: Error -- cannot access given TMPDIR '%s': %s\n", program, tmpdir_path, strerror(errno));
    exit(2);
  }

  /*
   * Compute the UID and GID for unprivileged operation.
   *
   * If the GID parameter is set, use that for the group; if not, but
   * the UID parameter is set, get the group from that user and set
   * the uid from there too.  If neither is set, use the real uid/gid
   * of the thread.
   *
   * We need the user's name to be able to load subsidiary groups, so
   * if we are using the process owner's UID we retrieve the name and
   * store it in a static buffer.
   */

  gid_t gid = -1;
  if(snapshot_group) {
    struct group *grp = getgrnam(snapshot_group);

    if(grp == NULL) {		/* The group name was invalid  */
      fprintf(stderr, "%s: Error -- given group %s is not recognised\n", program, snapshot_group);
      exit(2);
    }
    gid = grp->gr_gid;
  }

  uid_t uid = -1;
  if(snapshot_user) { /* Got a UID value */
    struct passwd *pwd = getpwnam(snapshot_user);

    if(pwd == NULL) {		/* The user name was invalid */
      fprintf(stderr, "%s: Error -- given user %s is not recognised\n", program, snapshot_user);
      exit(2);
    }

    uid = pwd->pw_uid;	/* Use this user's UID */
    if(gid == -1)
      gid = pwd->pw_gid;	/* Use this user's principal GID */
  }
  else {
    private char user[64];
    uid = getuid();		/* Use the real UID of this thread */
    gid = getgid();		/* Use the real GID of this thread */
    struct passwd *pwd = getpwuid(uid);
    strncpy(&user[0], pwd->pw_name, 64);
    snapshot_user = &user[0];	/* Establish the user's name, for loading groups */
  }

  /* 5b. Check capabilities and drop privileges */
  if( snap_adjust_capabilities() < 0 ) {
    exit(2);
  }
  if( main_adjust_capabilities(uid, gid) < 0 ) {
    exit(2);
  }

  /* Check the supplied parameters;  WRITER must come first as READER needs chunk size */
  strbuf e = alloc_strbuf(1);	/* Catch parameter error diagnostics */

  /* 5c. Verify and initialise parameters for the WRITER thread */
  if( !writer_parameters.w_schedprio )
    writer_parameters.w_schedprio = schedprio;
  strbuf_printf(e, "%s: Error -- WRITER Params: ", program);
  ret = verify_writer_params(&writer_parameters, e);
  if( ret < 0 ) {
    fprintf(stderr, "%s\n", strbuf_string(e));
    exit(3);
  }

   /* 5d. Verify and initialise parameters for the READER thread */
  if( !reader_parameters.r_schedprio )
    reader_parameters.r_schedprio = schedprio;
  strbuf_printf(e, "%s: Error -- READER Params: ", program);
  ret = verify_reader_params(&reader_parameters, e);
  if( ret < 0 ) {
    fprintf(stderr, "%s\n", strbuf_string(e));
    exit(3);
  }

  release_strbuf(e);

  /* Exit nicely on SIGINT:  this is done by setting the die_die_die_now flag. */
  if( set_intr_sig_handler() < 0 ) {
    exit(3);
  }

  /* Create the TIDY thread */
  pthread_attr_init(&tidy_thread_attr);
  if( pthread_create(&tidy_thread, &tidy_thread_attr, tidy_main, &log_socket) < 0 ) {
    fprintf(stderr, "%s: Error -- TIDY   thread creation failed: %s\n", program, strerror(errno));
    exit(4);
  }

  /* Wait here for log_socket */
  while( !die_die_die_now && !log_socket ) {
    usleep(10000);
  }

  if( !die_die_die_now ) {
    pthread_attr_init(&reader_thread_attr);    /* Create the READER thread */
    if( pthread_create(&reader_thread, &reader_thread_attr, reader_main, NULL) < 0 ) {
      fprintf(stderr, "%s: Error -- READER thread creation failed: %s\n", program, strerror(errno));
      exit(4);
    }

    pthread_attr_init(&writer_thread_attr);    /* Create the WRITER thread */
    if( pthread_create(&writer_thread, &writer_thread_attr, writer_main, NULL) < 0 ) {
      fprintf(stderr, "%s: Error -- WRITER thread creation failed: %s\n", program, strerror(errno));
      exit(4);
    }
  }

  /* Wait for the threads to establish comms etc. DON'T WAIT TOO LONG */
  while( !die_die_die_now ) {
    usleep(10000);		/* Wait for 10ms */
    if(reader_parameters.r_running && writer_parameters.w_running)
      break;			/* Now ready to start main loop */
  }

  /* Run the MAIN thread sevice loop here */
  if( create_main_comms() < 0 ) {
    die_die_die_now++;
  }
  main_thread_msg_loop();

  /* Clean up the various threads */
  if(reader_thread) {
    if( pthread_join(reader_thread, (void *)&thread_return) < 0 ) {
      fprintf(stderr, "%s: Error -- READER thread join error: %s\n", program, strerror(errno));
      thread_return = NULL;
    }
    else {
      if( thread_return ) {
	fprintf(stderr, "Log: READER thread rejoined -- %s\n", thread_return);
	thread_return = NULL;
      }
    }
  }
  if(writer_thread) {
    if( pthread_join(writer_thread, (void *)&thread_return) < 0 ) {
      fprintf(stderr, "%s: Error -- WRITER thread join error: %s\n", program, strerror(errno));
      thread_return = NULL;
    }
    else {
      if( thread_return ) {
	fprintf(stderr, "Log: WRITER thread rejoined -- %s\n", thread_return);
	thread_return = NULL;
      }
    }
  }

  if( pthread_join(tidy_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "%s: Error -- TIDY   thread join error: %s\n", program, strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "Log: TIDY   thread rejoined -- %s\n", thread_return);
      thread_return = NULL;
    }
  }

  /* Clean up our ZeroMQ sockets */
  close_main_comms();
  
  /* These were created by the TIDY thread */
  zmq_close(log_socket);
  zmq_ctx_term(snapshot_zmq_ctx);
  exit(0);
}
