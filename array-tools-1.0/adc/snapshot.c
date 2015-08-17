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
  { "tmpdir",   "/tmp",  &tmpdir_path,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "directory for creation of temporary files"
  },
  { "freq",     "312.5e3", &reader_parameters.r_frequency,
    PARAM_TYPE(double), PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "sampling frequency (divided by 8) of the ADC [Hz]"
  },
  { "snapshot", "ipc://snapshot-CMD", &snapshot_addr,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
  { "snapdir",  "snap", &writer_parameters.w_snapdir,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG/*|PARAM_SRC_CMD*/,
    "directory where samples are written"
  },
  { "dev",	"/dev/comedi0", &reader_parameters.r_device,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the Comedi device to open"
  },
  { "bufsz",	"32", &reader_parameters.r_bufsz,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the Comedi buffer [MiB]"
  },
  { "window",	"10", &reader_parameters.r_window,
    PARAM_TYPE(double),  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the ring buffer [s]"
  },
  { "rtprio",	NULL, &schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time threads [0-99]"
  },
  { "rdprio",	NULL, &reader_parameters.r_schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time reader thread [0-99]"
  },
  { "wrprio",	NULL, &writer_parameters.w_schedprio,
    PARAM_TYPE(int32),  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time writer thread [0-99]"
  },
  { "user",	NULL, &snapshot_user,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the user/UID for file system access and creation"
  },
  { "group",	NULL, &snapshot_group,
    PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the group/GID for file system access and creation"
  },
  { "permu",	"500", 0,
    PARAM_TYPE(int32), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the proportion of the ADC buffer to wait, in millionths"
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
  v1  = arg_litn("v",	"verbose", 0, 3,	"Increase verbosity"),
  q1  = arg_lit0("q",  "quiet",			"Decrease verbosity"),
  h1  = arg_lit0("h",	"help",			"Print usage help message"),
  vn1 = arg_lit0(NULL,	"version",		"Print program version string"),
  e1  = arg_end(20)
} APPLY_CMD_DEFAULTS(help) {
  /* No defaults to apply here */
} END_CMD_SYNTAX(help)

struct arg_lit *v2, *q2;
struct arg_end *e2;
struct arg_str *u2;

BEGIN_CMD_SYNTAX(main) {
  v2  = arg_litn("v",	"verbose", 0, 3,	"Increase verbosity"),
  q2  = arg_lit0("q",  "quiet",			"Decrease verbosity"),
  u2  = arg_str0("s",  "snapshot", "<url>",     "URL of snapshotter command socket"),
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

void       *zmq_main_ctx;	/* ZMQ context for messaging */

int	    tmpdir_dirfd;	/* The file descriptor obtained for the TMPDIR directory */
const char *tmpdir_path;		/* The path for the file descriptor above */

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

static void *log_socket;
static void *reader;
static void *writer;
static void *command;

static int create_main_comms() {
  int ret;

  /* Create and initialise the sockets: LOG socket */
  log_socket = zh_bind_new_socket(zmq_main_ctx, ZMQ_PULL, LOG_SOCKET);
  if( log_socket == NULL ) {
    fprintf(stderr, "%s: Error -- unable to create internal log socket: %s\n", program, strerror(errno));
    return -1;
  }

  /* Create and initialise the sockets: reader and writer command sockets */
  reader = zh_connect_new_socket(zmq_main_ctx, ZMQ_REQ, READER_CMD_ADDR);
  if( reader == NULL ) {
    fprintf(stderr, "%s: Error -- unable to cconnect internal socket to reader: %s\n", program, strerror(errno));
    return -1;
  }
  writer = zh_connect_new_socket(zmq_main_ctx, ZMQ_REQ, WRITER_CMD_ADDR);
  if( writer == NULL ) {
    fprintf(stderr, "%s: Error -- unable to connect internal socket to writer: %s\n", program, strerror(errno));
    return -1;
  }

  /* Create and initialise the external command socket */
  command = zh_bind_new_socket(zmq_main_ctx, ZMQ_REP, snapshot_addr);
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
  cap_flag_value_t v = CAP_CLEAR;
  uid_t u = geteuid();
  int ret = 0;

  if( !c )			/* No memory? */
    return -1;

  if( cap_get_flag(c, CAP_IPC_LOCK,  CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_NICE,  CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_ADMIN, CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR
      ) {
    cap_free(c);
    fprintf(stderr, "%s: I do not have the necessary capbilities to operate\n", program);
    errno = EPERM;
    return -1;
  }

  if( !u ) {
    const cap_value_t vs[] = { CAP_IPC_LOCK, CAP_SYS_NICE, CAP_SYS_ADMIN, };

    /* So we are root and have the capabilities we need.  Prepare to drop the others... */
    cap_clear(c);
    cap_set_flag(c, CAP_PERMITTED, sizeof(vs)/sizeof(cap_value_t), &vs[0], CAP_SET);
    if( prctl(PR_SET_KEEPCAPS, 1L) <0 ) {
      cap_free(c);
      fprintf(stderr, "%s: unable to keep required capabilities on user change\n", program);
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
  uid_t u = getuid();
  gid_t g = getgid();
  cap_t c = cap_get_proc();

  if(c) {
    cap_clear(c);
    if( cap_set_proc(c) < 0 ) {
      cap_free(c);
      fprintf(stderr, "%s: Error -- main thread fails to clear capabilities: %s\n", program, strerror(errno));
      return -1;
    }
  }
  if(u == uid && g == gid)
    return 0;

  if( !u ) { /* Running as root */
    if( setgid(gid) == 0 && setuid(uid) == 0 )
      return 0;
  }

  return -1;
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
 * Handle replies from reader thread
 */

#define COPYBUFSIZE	1024

int process_reply(void *socket) {
  char buf[COPYBUFSIZE];
  int size, more, ret;

  do {
    size = zh_get_msg(socket, 0, COPYBUFSIZE, buf);
    if(size) {
      more = zh_any_more(socket);
      ret  = zh_put_msg(command, (more? ZMQ_SNDMORE : 0), size, buf);
      if( ret < 0 )
	return ret;
    }
  } while(more);
  return 0;
}

/*
 * Handle commands sent to the snapshotter.  These are forwarded
 * either to the reader thread or the writer thread, and their replies
 * are returned to the originator.  Using the REP socket ensures only
 * one outstanding message is in process, so simplifies the reply routing.
 */

int process_snapshot_command() {
  char buf[COPYBUFSIZE];
  int size, ret;

  size = zh_get_msg(command, 0, COPYBUFSIZE, buf);
  buf[size] = '\0';
  if( !size ) {
    ret = zh_put_msg(command, 0, 0, NULL); /* If empty message received, send empty reply at once */
    assertv(ret == 0, "Reply to command failed, %d\n", ret);
    return 0;
  }
  // fprintf(stderr, "Msg '%c' (%d)\n", buf[0], buf[0]);
  switch(buf[0]) {
  case 'q':
  case 'Q':			/* Deal specially with Quit command, to close down nicely... */
    ret = zh_put_msg(reader, 0, size, buf); /* Forward this commands to the reader thread */
    assertv(ret > 0, "Quit to reader failed, %d\n", ret);
    ret = zh_put_msg(writer, 0, size, buf); /* Forward this commands to the writer thread */
    assertv(ret > 0, "Quit to writer failed, %d\n", ret);
    ret = zh_put_msg(command, 0, 7, "OK Quit"); /* Reply to Quit here */
    assertv(ret > 0, "Quit reply failed, %d\n", ret);
    break;

  case 'g':
  case 'G':
  case 'h':
  case 'H':
  case 'i':
  case 'I':
  case 'p':
  case 'P':
    ret = zh_put_msg(reader, 0, size, buf); /* Forward these commands to the reader thread */
    assertv(ret > 0, "Forward to reader failed, %d\n", ret);
    break;

  case 'd':
  case 'D':
  case 's':
  case 'S':
    ret = zh_put_msg(writer, 0, size, buf); /* Forward snapshot and dir commands to writer */
    assertv(ret > 0, "Forward to writer failed, %d\n", ret);
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
  while(running) {
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

  if(verbose > 2)		/* Dump global parameters for debugging purposes */
    debug_params(stderr, globals, n_global_params);

  /* 5. Process parameters:  deal with non-parameter table arguments where necessary */

  exit(0);

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

   /* 5b. Verify and initialise parameters for the reader thread */
  if( !reader_parameters.r_schedprio )
    reader_parameters.r_schedprio = schedprio;
  ret = verify_reader_params(&reader_parameters);
  if( ret < 0 ) {
    fprintf(stderr, "Reader parameter checks failed at step %d: %s\n", -ret, strerror(errno));
    exit(2);
  }

  /* 5c. Verify and initialise parameters for the writer thread */
  if( !writer_parameters.w_schedprio)
    writer_parameters.w_schedprio = schedprio;
  ret = verify_writer_params(&writer_parameters);
  if( ret < 0 ) {
    fprintf(stderr, "Writer parameter checks failed at step %d: %s\n", -ret, strerror(errno));
    exit(2);
  }

  /* Create the tidy thread */
  pthread_attr_init(&tidy_thread_attr);
  if( pthread_create(&tidy_thread, &tidy_thread_attr, tidy_main, NULL) < 0 ) {
    fprintf(stderr, "%s: Error -- tidy thread creation failed: %s\n", program, strerror(errno));
    exit(3);
  }

  /* Create the reader thread */
  pthread_attr_init(&reader_thread_attr);
  if( pthread_create(&reader_thread, &reader_thread_attr, reader_main, NULL) < 0 ) {
    fprintf(stderr, "%s: Error -- reader thread creation failed: %s\n", program, strerror(errno));
    exit(3);
  }

  /* Create the writer thread */
  pthread_attr_init(&writer_thread_attr);
  if( pthread_create(&writer_thread, &writer_thread_attr, writer_main, NULL) < 0 ) {
    fprintf(stderr, "%s: Error -- writer thread creation failed: %s\n", program, strerror(errno));
    exit(3);
  }

  /* Wait for the threads to establish comms etc. */

  /* Now ready to start main loop */
  if(reader_parameters.r_running && writer_parameters.w_running) {
    
  }

  /* Tidy up threads */
  if( pthread_join(reader_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "%s: Error -- reader thread join error: %s\n", program, strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "%s: reader thread rejoined -- %s\n", program, thread_return);
      thread_return = NULL;
    }
  }

  if( pthread_join(writer_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "%s: Error -- writer thread join error: %s\n", program, strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "%s: writer thread rejoined -- %s\n", program, thread_return);
      thread_return = NULL;
    }
  }

  if( pthread_join(tidy_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "%s: Error -- tidy thread join error: %s\n", program, strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "%s: tidy thread rejoined -- %s\n", program, thread_return);
      thread_return = NULL;
    }
  }

  /* Clean up ZeroMQ sockets and context */
  zmq_close(log_socket);
  zmq_close(reader);
  zmq_close(writer);
  zmq_close(command);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}
