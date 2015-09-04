#

#define _GNU_SOURCE	/* Linux-specific code below (O_PATH) */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
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

extern rparams     reader_parameters;
extern wparams	   writer_parameters;
extern const char *tmpdir_path;
static const char *snapshot_addr;
static const char *snapshot_user;
static const char *snapshot_group;
static int	   schedprio;

param_t globals[] ={
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
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG/*|PARAM_SRC_CMD*/,
    "directory where samples are written"
  },
  { "dev",	"/dev/comedi0",
    &reader_parameters.r_device,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the Comedi device to open"
  },
  { "bufsz",	"32",
    &reader_parameters.r_bufsz,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the Comedi buffer [MiB]"
  },
  { "window",	"10",
    &reader_parameters.r_window,
    PARAM_TYPE(double),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the ring buffer [s]"
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
    "the user/UID for file system access and creation"
  },
  { "group",	NULL,
    &snapshot_group,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the group/GID for file system access and creation"
  },
  { "ram",	"64",
    &writer_parameters.w_lockedram,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the amount of data RAM to lock [MiB]"
  },
  { "chunk",	"1024",
    &writer_parameters.w_chunksize,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "The size of a transfer chunk [KiB]"
  },
};

const int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int   debug_level = 0;
int   verbose;
char *program   = NULL;

/* Command line syntax options -- there are no mandatory arguments on the main command line! */

struct arg_lit *h1, *vn1, *v1, *q1;
struct arg_end *e1;

BEGIN_CMD_SYNTAX(help) {
  v1  = arg_litn("v",  "verbose", 0, 3,	"Increase verbosity"),
  q1  = arg_lit0("q",  "quiet",		"Decrease verbosity"),
  h1  = arg_lit0("h",  "help",		"Print usage help message"),
  vn1 = arg_lit0(NULL, "version",	"Print program version string"),
  e1  = arg_end(20)
} APPLY_CMD_DEFAULTS(help) {
  /* No defaults to apply here */
} END_CMD_SYNTAX(help)

struct arg_lit *v2, *q2;
struct arg_end *e2;
struct arg_str *u2;

BEGIN_CMD_SYNTAX(main) {
  v2  = arg_litn("v", "verbose", 0, 3,	   "Increase verbosity"),
  q2  = arg_lit0("q", "quiet",		   "Decrease verbosity"),
  u2  = arg_str0("s", "snapshot", "<url>", "URL of snapshotter command socket"),
  e2  = arg_end(20)
} APPLY_CMD_DEFAULTS(main) {
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params);
} END_CMD_SYNTAX(main);

/* Standard help routines: display the version banner */
void print_version(FILE *fp, int verbosity) {
  fprintf(fp, "%s: Vn. %s\n", program, PROGRAM_VERSION);
  if(verbosity > 0) {		/* Verbose requested... */
    fprintf(fp, VERSION_VERBOSE_BANNER);
  }
}

/* Standard help routines: display the usage summary for a syntax */
void print_usage(FILE *fp, void **argtable, int verbosity, char *program) {
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

static const char *snapshot_addr = NULL;  /* The address of the main command socket */
static const char *snapshot_user = NULL;  /* The user we should run as, after startup */
static const char *snapshot_group = NULL; /* The group to run as, after startup */
static int	   schedprio;		  /* Real-time priority for reader and writer */

/*
 * Snapshot globals shared between threads
 */

void       *snapshot_zmq_ctx;	/* ZMQ context for messaging -- created by the TIDY thread */

int	    tmpdir_dirfd;	/* The file descriptor obtained for the TMPDIR directory */
const char *tmpdir_path;	/* The path for the file descriptor above */

/*
 * Thread handles for reader and writer
 */

static pthread_t  reader_thread,
		  writer_thread,
		  tidy_thread;

static pthread_attr_t reader_thread_attr,
		      writer_thread_attr,
		      tidy_thread_attr;

/*
 * Establish main comms:  this routine runs last, so it mostly does connect() calls.
 * It must run when the other three threads are already active.
 */

static void *log_socket;	/* N.B.  This socket is opened by the TIDY thread, but not used there */
static void *reader;
static void *writer;
static void *command;

static int create_main_comms() {
  int ret;

  /* Create and initialise the sockets: reader and writer command sockets */
  reader = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_REQ, READER_CMD_ADDR);
  if( reader == NULL ) {
    fprintf(stderr, "%s: Error -- unable to cconnect internal socket to reader: %s\n", program, strerror(errno));
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
 * Otherwise the main thread and the tidy thread need no special powers.  The ZMQ IO thread
 * is also unprivileged, and is currently spawned during context creation from tidy.
 */

static int snap_adjust_capabilities() {
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
    const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, CAP_SYS_ADMIN, };

    /* So we are root and have the capabilities we need.  Prepare to drop the others... */
    cap_clear(c);
    cap_set_flag(c, CAP_PERMITTED, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
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

static int main_adjust_capabilities(uid_t uid, gid_t gid) {
  cap_t c = cap_get_proc();

  if(c) {
    cap_clear(c);
    if( cap_set_proc(c) < 0 ) {
      cap_free(c);
      fprintf(stderr, "%s: Error -- MAIN thread fails to clear capabilities: %s\n", program, strerror(errno));
      return -1;
    }
  }

  /* Drop all user and group privileges:  set all uids to uid and all gids to gid */
  /* Complain if that fails -- we were not root and uid/gid were not in our set */
  if( setresgid(gid, gid, gid) < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread unable to change to gid %d: %s\n", program, gid, strerror(errno));
    return -1;
  }
  if( setresuid(uid, uid, uid) < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread unable to change to uid %d: %s\n", program, uid, strerror(errno));
    return -1;
  }

  /* Now check we still have the required permitted capabilities */
  if( check_permitted_capabilities_ok() < 0 ) {
    fprintf(stderr, "%s: Error -- MAIN thread lost capabilities on changing user!\n", program);
    return -1;
  }

  return 0;
}

/*
 * Process a (possibly multipart) log message.
 * Collect the various pieces and write to stderr
 * Use a 1024 byte logging buffer
 */

#define LOGBUF_SIZE	1024

int process_log_message(void *socket) {
  char log_buffer[LOGBUF_SIZE];
  int used;

  used = zh_collect_multi(socket, &log_buffer[0], LOGBUF_SIZE-1, " ");
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
 * is a pointer to a set of error strbufs.  We collect all the
 * strings, joining them with newline, in the reply buffer.  The
 * collector maintains an invariant that reply_buffer[u-1] is not NUL.
 */

#define REPLY_BUFSIZE	4096
static char reply_buffer[REPLY_BUFSIZE];

/* Collect the reply: the block bytes count is the number of bytes */
/* so far, its pointer is where we have got to in the reply_buffer */

void collect_reply_strings(void *ctx, queue *q) {
  strbuf  s = (strbuf)q;
  int     n = strbuf_used(s);

  if( !n ) return;		/* Empty buffer, nothing to do */

  strbuf_revert(s);		/* Remove any internal NUL characters */

  char *b = ((block *)ctx)->b_data;	/* Current data pointer */
  int   u = ((block *)ctx)->b_bytes;	/* Current space used */
  if(n > REPLY_BUFSIZE-u) {		/* There is too much data */
    n = REPLY_BUFSIZE-u-2;		/* We can manage this much of it */
  }

  memcpy(b, strbuf_string(s), n);	/* Copy the data */

  b += n;  u += n;			/* Now we have used this much space */
  while( b[-1] == '\0' ) b--,u--;	/* Skip back over any NULs */

  ((block *)ctx)->b_data  = b;
  ((block *)ctx)->b_bytes = u;
  return;
}

/* Send the collected reply;  ensure a terminating newline */

static int process_reply(void *s) {
  int     size;
  strbuf  err;
  block   b = { &reply_buffer[0], 0 };
  
  size = zh_get_msg(s, 0, sizeof(strbuf), (void *)&err);
  assertv(size==sizeof(err), "Reply message of wrong size %d\n", size);

  /* Traverse the strbuf chain once collecting data, then release */
  map_queue_nxt((queue*)err, NULL, collect_reply_strings, &b);
  release_strbuf(err);

  if( reply_buffer[b.bytes-1] == '\0' )		/* Replace trailing NUL with newline */
    reply_buffer[b.b_bytes-1] = '\n';
  if( reply_buffer[b.b_bytes-1] != '\n' )	/* If last character is not newline, add one */
    reply_buffer[b.b_bytes++] = '\n';

  /* Send the complete reply */
  zh_put_msg(command, 0, b.b_bytes, &reply_buffer[0]);
  return 0;
}

/*
 * Handle commands sent to the snapshotter.  These are forwarded
 * either to the reader thread or the writer thread, and their replies
 * are returned to the originator.  Using the REP socket ensures only
 * one outstanding message is in process, so simplifies the reply routing.
 */

int process_snapshot_command() {
  strbuf c_n_e;			/* Command and Error buffers */
  char  *buf;
  int   size, ret;
  int   fwd;

  c_n_e = alloc_strbuf(2);
  buf = strbuf_string(c_n_e);
  size = zh_get_msg(command, 0, strbuf_space(c_n_e), buf);
  if( !size ) {
    ret = zh_put_msg(command, 0, 0, NULL); /* If empty message received, send empty reply at once */
    release_strbuf(c_n_e);
    assertv(ret == 0, "Reply to command failed, %d\n", ret);
    return 0;
  }
  strbuf_setpos(c_n_e, size);
  buf[size] = '\0';
  // fprintf(stderr, "Msg '%c' (%d)\n", buf[0], buf[0]);
  fwd = 0;
  switch(buf[0]) {
  case 'q':
  case 'Q':			/* Deal specially with Quit command, to close down nicely... */
    ret = zh_put_msg(reader, 0, 0, NULL); /* Forward zero length message to the READER thread */
    assertv(ret == 0, "Quit to READER failed, %d\n", ret);
    ret = zh_put_msg(writer, 0, 0, NULL); /* Forward zero length message to the WRITER thread */
    assertv(ret == 0, "Quit to WRITER failed, %d\n", ret);
    /* PERHAPS MOVE THIS TO END */
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
    ret = zh_put_msg(reader, 0, sizeof(strbuf), (void *)&c_n_e);
    assertv(ret == sizeof(c_n_e), "Forward to READER failed, %d\n", ret);
    fwd++;
    break;

  case 'd':
  case 'D':
  case 's':
  case 'S':
  case 'z':
  case 'Z':
    /* Forward snapshot and dir commands to WRITER */
    ret = zh_put_msg(writer, 0, sizeof(strbuf), (void *)&c_n_e);
    assertv(ret == sizeof(c_n_e), "Forward to WRITER failed, %d\n", ret);
    fwd++;
    break;

  case '?':
    buf[0] = '!';
    ret = zh_put_msg(command, 0, size, buf); /* Reply to 'ping' message */
    assertv(ret > 0, "Reply to ping failed, %d\n", ret);
    break;

  default:
    ret = zh_put_multi(command, 2, "Unknown command: ", buf);
    assertv(ret == 0, "Reject unknown reply failed, %d\n", ret);
    break;
  }
  if( !fwd )
    release_strbuf(c_n_e);
  return 0;
}

/*
 * Main thread message loop
 */

#define	MAIN_LOOP_POLL_INTERVAL	20

static void main_thread_msg_loop() {    /* Read and process messages */
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

  fprintf(stderr, "%s: starting main thread polling loop with %d items\n", program, N_POLL_ITEMS);
  running = true;
  poll_delay = MAIN_LOOP_POLL_INTERVAL;
  while(running && !die_die_die_now) {
    int n;
    int ret = zmq_poll(&poll_list[0], N_POLL_ITEMS, poll_delay);

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      fprintf(stderr, "%s: main thread loop interrupted\n", program);
      break;
    }
    if(ret < 0)
      break;
    running = reader_parameters.r_running || writer_parameters.w_running;
    if( !running )		/* Flush out last messages */
      poll_delay = 1000;
    for(n=0; n<N_POLL_ITEMS; n++) {
      if( poll_list[n].revents & ZMQ_POLLIN ) {
	ret = (*poll_responders[n])(poll_list[n].socket);
	assertv(ret >= 0, "Error in message processing in main poll loop, ret %d\n", ret);
	running = true;
      }
    }
  }
}

/*
 * Snapshot main routine.
 */

int main(int argc, char *argv[], char *envp[]) {
  char *thread_return = NULL;
  int ret, running, poll_delay;
  char *cmd_addr;
  param_t *p;

  program = argv[0];

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, environment, then command-line argument. */
  push_param_from_env(envp, globals, n_global_params);

  /* 2. Process parameters:  push values out to program globals */
  ret = assign_all_params(globals, n_global_params);
  assertv(ret == n_global_params, "Push parameters missing some %d/%d done\n", ret, n_global_params); /* If not, there is a coding problem */

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

  /* 4. Process parameters:  copy argument values back through the parameter table */
  ret = arg_results_to_params(cmd_main, globals, n_global_params);

  /* 5. Process parameters:  deal with non-parameter table arguments where necessary */

  /* 5a. Verify parameters required by the main program/thread */
  tmpdir_dirfd = open(tmpdir_path, O_PATH|O_DIRECTORY); /* Verify the TMPDIR path */
  if( tmpdir_dirfd < 0 ) {
    fprintf(stderr, "%s: Error -- cannot access given TMPDIR '%s': %s\n", program, tmpdir_path, strerror(errno));
    exit(2);
  }

  /* Compute the UID and GID for unprivileged operation.
   *
   * If the GID parameter is set, use that for the group; if not, but
   * the UID parameter is set, get the group from that user and set
   * the uid from there too.  If neither is set, use the real uid/gid
   * of the thread.
   */

  gid_t gid = -1;
  if(snapshot_group) {
    struct group *grp = getgrnam(snapshot_group);

    if(grp == NULL) {		/* The group name was invalid  */
      fprintf(stderr, "%s: Error -- given group %s is not recognised: %s\n", program, snapshot_group, strerror(errno));
      exit(2);
    }
    gid = grp->gr_gid;
  }

  uid_t uid = -1;
  if(snapshot_user) { /* Got a UID value */
    struct passwd *pwd = getpwnam(snapshot_user);

    if(pwd == NULL) {		/* The user name was invalid */
      fprintf(stderr, "%s: Error -- given user %s is not recognised: %s\n", program, snapshot_user, strerror(errno));
      exit(2);
    }

    uid = pwd->pw_uid;	/* Use this user's UID */
    if(gid < 0)
      gid = pwd->pw_gid;	/* Use this user's principal GID */
  }
  else {
    uid = getuid();		/* Use the real UID of this thread */
    gid = getgid();		/* Use the real GID of this thread */
  }

  strbuf e = alloc_strbuf(1);	/* Catch parameter error diagnostics */

   /* 5b. Verify and initialise parameters for the reader thread */
  if( !reader_parameters.r_schedprio )
    reader_parameters.r_schedprio = schedprio;
  strbuf_printf(e, "Reader Params: ");
  ret = verify_reader_params(&reader_parameters, e);
  if( ret < 0 ) {
    fprintf(stderr, "%s\n", strbuf_string(e));
    exit(2);
  }

  /* 5c. Verify and initialise parameters for the writer thread */
  if( !writer_parameters.w_schedprio)
    writer_parameters.w_schedprio = schedprio;
  strbuf_printf(e, "Writer Params: ");
  ret = verify_writer_params(&writer_parameters, e);
  if( ret < 0 ) {
    fprintf(stderr, "%s\n", strbuf_string(e));
    exit(2);
  }

  release_strbuf(e);

  if(snap_adjust_capabilities() < 0) {		/* Drop un-necessary capabilities */
    exit(3);
  }
  if(main_adjust_capabilities(uid, gid) < 0) {	/* Change to target user */
    exit(3);
  }

  /* Create the TIDY thread */
  pthread_attr_init(&tidy_thread_attr);
  if( pthread_create(&tidy_thread, &tidy_thread_attr, tidy_main, &log_socket) < 0 ) {
    fprintf(stderr, "%s: Error -- TIDY thread creation failed: %s\n", program, strerror(errno));
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

  /* Tidy up threads */
  if(reader_thread) {
    if( pthread_join(reader_thread, (void *)&thread_return) < 0 ) {
      fprintf(stderr, "%s: Error -- READER thread join error: %s\n", program, strerror(errno));
      thread_return = NULL;
    }
    else {
      if( thread_return ) {
	fprintf(stderr, "%s: READER thread rejoined -- %s\n", program, thread_return);
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
	fprintf(stderr, "%s: WRITER thread rejoined -- %s\n", program, thread_return);
	thread_return = NULL;
      }
    }
  }

  if( pthread_join(tidy_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "%s: Error -- TIDY thread join error: %s\n", program, strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "%s: TIDY thread rejoined -- %s\n", program, thread_return);
      thread_return = NULL;
    }
  }

  /* Clean up ZeroMQ sockets and context */
  zmq_close(log_socket);
  zmq_close(reader);
  zmq_close(writer);
  zmq_close(command);
  zmq_ctx_term(snapshot_zmq_ctx);
  exit(0);
}
