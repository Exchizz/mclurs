#

#define _GNU_SOURCE	/* Linux-specific code below (O_PATH) */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <zmq.h>
#include <pthread.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sched.h>

#include <comedi.h>
#include <comedilib.h>

#include "util.h"
#include "param.h"
#include "queue.h"
#include "snapshot.h"
#include "reader.h"
#include "writer.h"

/*
 * Snapshot version
 */

#define PROGRAM_VERSION	"1.0"

/*
 * Global parameters for the snapshot program
 */

param_t globals[] ={
  { "tmpdir",     1, { "/tmp", },           PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "directory for creation of temporary files"
  },
  { "freq",       1, { "312.5e3", },          PARAM_DOUBLE, PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "sampling frequency (divided by 8) of the ADC [Hz]"
  },
  { "snapshot",   1, { "ipc://snapshot-CMD", }, PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
  { "snapdir",    1, { "snap", },              PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "directory where samples are written"
  },
  { "dev",	  1, { "/dev/comedi0", },   PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the Comedi device to open"
  },
  { "bufsz",	  1, { "32", },             PARAM_INT32,  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the Comedi buffer [MiB]"
  },
  { "window",	  1, { "10", },             PARAM_INT32,  PARAM_SRC_ENV|PARAM_SRC_ARG|PARAM_SRC_CMD,
    "size of the ring buffer [s]"
  },
  { "rtprio",	  0, { "", },		    PARAM_INT32,  PARAM_SRC_ENV|PARAM_SRC_ARG,
    "priority of real-time reader thread [0-99]"
  },
  { "uid",	  0, { "", },		    PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the writer thread's UID for file creation"
  },
  { "gid",	  0, { "", },		    PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the writer thread's GID for file creation"
  },
  { "permu",	  1, { "500", },	    PARAM_INT32, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "the proportion of the ADC buffer to wait, in millionths"
  },
};

int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int debug_level = 0;
char *program   = NULL;

/*
 * Snapshot globals shared between threads
 */

void      *zmq_main_ctx;	/* ZMQ context for messaging */

int	   reader_thread_running, /* For cleanly stopping main loop */
	   writer_thread_running;

int	   tmpdir_dirfd;	/* The file descriptor obtained for the TMPDIR directory */
char      *tmpdir_path;		/* The path for the file descriptor above */

void      *wr_queue_reader;	/* Pipe between Reader and Writer for queue handling */
void      *wr_queue_writer;

/*
 * Thread handles for reader and writer
 */

static pthread_t  reader_thread,
		  writer_thread;

static pthread_attr_t reader_thread_attr,
		      writer_thread_attr;

/*
 * Public sockets for main thread
 */

static void *log_socket;
static void *reader;
static void *writer;
static void *command;

/*
 * Process a (possibly multipart) log message.
 * Collect the various pieces and write to stderr
 * Use a 1024 byte logging buffer
 */

#define LOGBUF_SIZE	1024

void process_log_message(void *socket) {
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
}

/*
 * Display usage summary
 */

void usage() {
  char buf[1024];

  fprintf(stderr, "%s $s\n\n", program, PROGRAM_VERSION);
  fprintf(stderr, "Usage: ");
  param_brief_usage(&buf[0], 1024, globals, n_global_params);
  fprintf(stderr, "%s [-vqh] %s\n", program, &buf[0]);

  if(debug_level >= 1) {
    fprintf(stderr, "\n");
    fprintf(stderr, "    -v : increase verbosity\n");
    fprintf(stderr, "    -q : decrease verbosity\n");
    fprintf(stderr, "    -h : display usage message\n");
    param_option_usage(stderr, 4, globals, n_global_params);
  }
}

/*
 * Option handling code
 */

int opt_handler(int c, char *arg) {
  switch(c) {
  case 'v':
    debug_level++;
    return 0;

  case 'q':
    debug_level--;
    return 0;

  case 'h':
    usage();
    exit(0);
  }

  return -1;
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
    assert(ret == 0);
    return 0;
  }
  // fprintf(stderr, "Msg '%c' (%d)\n", buf[0], buf[0]);
  switch(buf[0]) {
  case 'q':
  case 'Q':			/* Deal specially with Quit command, to close down nicely... */
    ret = zh_put_msg(reader, 0, size, buf); /* Forward this commands to the reader thread */
    assert(ret > 0);
    ret = zh_put_msg(writer, 0, size, buf); /* Forward this commands to the writer thread */
    assert(ret > 0);
    ret = zh_put_msg(command, 0, 7, "OK Quit"); /* Reply to Quit here */
    assert(ret > 0);
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
    assert(ret > 0);
    break;

  case 's':
  case 'S':
    ret = zh_put_msg(writer, 0, size, buf); /* Forward snapshot command to writer */
    assert(ret > 0);
    break;

  case '?':
    buf[0] = '!';
    ret = zh_put_msg(command, 0, size, buf); /* Reply to 'ping' message */
    assert(ret > 0);
    break;

  default:
    ret = zh_put_multi(command, 2, "Unknown command: ", buf);
    assert(ret == 0);
    break;
  }
  return 0;
}

/*
 * Snapshot main routine.
 *
 */

#define	MAIN_LOOP_POLL_INTERVAL	20

int main(int argc, char *argv[], char *envp[]) {
  char *thread_return = NULL;
  int ret, running, poll_delay;
  char *cmd_addr;
  param_t *p;

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, environment, then command-line argument. */
  push_param_from_env(envp, globals, n_global_params);
  program = argv[0];
  ret = getopt_long_params(argc, argv, "vqh", globals, n_global_params, opt_handler);
  if( ret < 0 ) {
    if(errno)
      fprintf(stderr, "Problem handling arguments: %s\n", strerror(errno));
    exit(1);
  }

  if(debug_level > 1)		/* Dump global parameters for debugging purposes */
    debug_params(globals, n_global_params);

  /* Set up the standard parameters */
  /* 2. Verify parameters required by the main program/thread */
  p = find_param_by_name("snapshot", 8, globals, n_global_params);
  assert(p != NULL);         /* Fatal if parameter not found */
  if( get_param_value(p, &cmd_addr) < 0 ) {
    fprintf(stderr, "%s: Cannot get value for snapshot address parameter\n", program);
    exit(2);
  }

  p = find_param_by_name("tmpdir", 6, globals, n_global_params);
  assert(p != NULL);         /* Fatal if parameter not found */
  if( get_param_value(p, &tmpdir_path) < 0 ) {
    fprintf(stderr, "%s: Cannot get value for TMPDIR parameter\n", program);
    exit(2);
  }
  tmpdir_dirfd = open(tmpdir_path, O_PATH|O_DIRECTORY); /* Verify the TMPDIR path */
  if( tmpdir_dirfd < 0 ) {
    fprintf(stderr, "%s: Cannot access given TMPDIR '%s': %s\n", program, tmpdir_path, strerror(errno));
    exit(2);
  }

  /* Set up the standard parameters */
  /* 3. Verify and initialise parameters for the reader thread */
  ret = verify_reader_params(globals, n_global_params);
  if( ret < 0 ) {
    fprintf(stderr, "Reader parameter checks failed at step %d: %s\n", -ret, strerror(errno));
    exit(2);
  }

  /* Set up the standard parameters */
  /* 4. Verify and initialise parameters for the writer thread */
  ret = verify_writer_params(globals, n_global_params);
  if( ret < 0 ) {
    fprintf(stderr, "Writer parameter checks failed at step %d: %s\n", -ret, strerror(errno));
    exit(2);
  }

  /* Create the ZMQ context */
  zmq_main_ctx  = zmq_ctx_new();
  if( !zmq_main_ctx ) {
    fprintf(stderr, "%s: ZeroMQ context creation failed: %s\n", program, strerror(errno));
    exit(1);
  }

  /* Create and initialise the sockets: LOG socket */
  log_socket = zmq_socket(zmq_main_ctx, ZMQ_PULL);
  if( log_socket == NULL ) {
    fprintf(stderr, "Unable to create internal log socket: %s\n", strerror(errno));
    exit(2);
  }
  ret = zmq_bind(log_socket, LOG_SOCKET);
  if( ret < 0) {
    fprintf(stderr, "Binding internal log socket failed: %s\n", strerror(errno));
    exit(2);
  }

  /* Create and initialise the sockets: reader and writer command sockets */
  reader = zmq_socket(zmq_main_ctx, ZMQ_REQ);
  if( reader == NULL ) {
    fprintf(stderr, "Unable to create internal socket to reader: %s\n", strerror(errno));
    exit(2);
  }
  writer = zmq_socket(zmq_main_ctx, ZMQ_REQ);
  if( writer == NULL ) {
    fprintf(stderr, "Unable to create internal socket to writer: %s\n", strerror(errno));
    exit(2);
  }

  /* Create and initialise the sockets: reader-writer pipe for write queue */
  wr_queue_reader = zmq_socket(zmq_main_ctx, ZMQ_PAIR);
  if( wr_queue_reader == NULL ) {
    fprintf(stderr, "Unable to create internal socket to writer: %s\n", strerror(errno));
    exit(2);
  }

  wr_queue_writer = zmq_socket(zmq_main_ctx, ZMQ_PAIR);
  if( wr_queue_writer == NULL ) {
    fprintf(stderr, "Unable to create internal socket to writer: %s\n", strerror(errno));
    exit(2);
  }

  /* Create and initialise the sockets: connect the reader-writer pipe for write queue */
  ret = zmq_bind(wr_queue_writer, WRITE_QUEUE);
  if( ret < 0 ) {
    fprintf(stderr, "Binding internal write queue (writer) socket failed: %s\n", strerror(errno));
    exit(2);
  }

  ret = zmq_connect(wr_queue_reader, WRITE_QUEUE);
  if( ret < 0 ) {
    fprintf(stderr, "Connecting internal write queue (reader) socket failed: %s\n", strerror(errno));
    exit(2);
  }

  /* Create the reader thread */
  reader_thread_running = true;
  pthread_attr_init(&reader_thread_attr);
  if( pthread_create(&reader_thread, &reader_thread_attr, reader_main, NULL) < 0 ) {
    fprintf(stderr, "Reader thread creation failed: %s\n", strerror(errno));
    exit(2);
  }

  /* Create the writer thread */
  writer_thread_running = true;
  pthread_attr_init(&writer_thread_attr);
  if( pthread_create(&writer_thread, &writer_thread_attr, writer_main, NULL) < 0 ) {
    fprintf(stderr, "Writer thread creation failed: %s\n", strerror(errno));
    exit(2);
  }

  /* Connect the sockets to talk to the threads */
  ret = zmq_connect(reader, READER_CMD_ADDR);
  if( ret < 0) {
    fprintf(stderr, "Connecting internal reader socket failed: %s\n", strerror(errno));
    exit(2);
  }
  ret = zmq_connect(writer, WRITER_CMD_ADDR);
  if( ret < 0) {
    fprintf(stderr, "Connecting internal writer socket failed: %s\n", strerror(errno));
    exit(2);
  }

  /* Create and initialise the sockets: command socket */
  command = zmq_socket(zmq_main_ctx, ZMQ_REP);
  if( command == NULL ) {
    fprintf(stderr, "Unable to create external command socket: %s\n", strerror(errno));
    exit(2);
  }

  /* Bind the socket to enable command reception */
  ret = zmq_bind(command, cmd_addr);
  if( ret < 0) {
    fprintf(stderr, "%s: Binding snapshot socket to %s failed: %s\n", program, cmd_addr, strerror(errno));
    exit(2);
  }

  /* Read and process messages */
  zmq_pollitem_t  poll_list[] =
    { { log_socket, 0, ZMQ_POLLIN, 0 },
      { command, 0, ZMQ_POLLIN, 0 },
      { reader, 0, ZMQ_POLLIN, 0 },
      { writer, 0, ZMQ_POLLIN, 0 },
    };
#define	POLL_NITEMS	(sizeof(poll_list)/sizeof(zmq_pollitem_t))

  fprintf(stderr, "Main thread initialised, starting polling loop with %d items\n", POLL_NITEMS);
  running = true;
  poll_delay = MAIN_LOOP_POLL_INTERVAL;
  while(running) {
    int ret = zmq_poll(&poll_list[0], POLL_NITEMS, poll_delay);

    if( ret < 0 && errno == EINTR ) { /* Interrupted */
      fprintf(stderr, "Main loop interrupted\n");
      break;
    }
    if(ret < 0)
      break;
    running = reader_thread_running || writer_thread_running;
    if( !running )		/* Flush out last messages */
      poll_delay = 1000;
    if( poll_list[0].revents & ZMQ_POLLIN ) { /* Deal with log messages from other threads */
      //      fprintf(stderr, "Main thread loop gets a log message...\n");
      process_log_message(log_socket);
      running = true;
    }
    if( poll_list[1].revents & ZMQ_POLLIN ) { /* Deal with incoming commands */
      fprintf(stderr, "Main thread loop gets a command message...\n");
      process_snapshot_command();
      running = true;
    }
    if( poll_list[2].revents & ZMQ_POLLIN ) { /* Deal with replies from reader */
      fprintf(stderr, "Main thread loop gets a reader message...\n");
      process_reply(reader);
      running = true;
    }
    if( poll_list[3].revents & ZMQ_POLLIN ) { /* Deal with replies from writer */
      fprintf(stderr, "Main thread loop gets a writer message...\n");
      process_reply(writer);
      running = true;
    }
  }

  /* Tidy up threads */
  if( pthread_join(reader_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "Reader thread join error: %s\n", strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "Reader thread rejoined -- %s\n", thread_return);
      thread_return = NULL;
    }
  }

  if( pthread_join(writer_thread, (void *)&thread_return) < 0 ) {
    fprintf(stderr, "Writer thread join error: %s\n", strerror(errno));
    thread_return = NULL;
  }
  else {
    if( thread_return ) {
      fprintf(stderr, "Writer thread rejoined -- %s\n", thread_return);
      thread_return = NULL;
    }
  }

  /* Clean up ZeroMQ sockets and context */
  zmq_close(log_socket);
  zmq_close(reader);
  zmq_close(writer);
  zmq_close(command);
  zmq_close(wr_queue_reader);
  zmq_close(wr_queue_writer);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}
