#

#include <stdio.h>
#include <stdlib.h>

#include <zmq.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>

#include "util.h"
#include "param.h"
#include "snapshot.h"

/*
 * Snapshot version
 */

#define PROGRAM_VERSION	"1.0"

/*
 * Global parameters for the snapshot program
 */

param_t globals[] ={
  { "snapshot",   1, { "ipc://snapshot-CMD", }, PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
};

int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int debug_level = 0;
char *program   = NULL;

/*
 * Snapchat globals shared between threads
 */

void      *zmq_main_ctx;	/* ZMQ context for messaging */

/*
 * Process a (possibly multipart) log message.
 * Collect the various pieces and write to stderr
 * Use a 1024 byte logging buffer
 */

#define LOGBUF_SIZE	1024

void print_message(void *socket) {
  char log_buffer[LOGBUF_SIZE];
  int used;

  used = zh_collect_multi(socket, &log_buffer[0], LOGBUF_SIZE-1, "");
  if( log_buffer[used-1] != '\n') {
    log_buffer[used] = '\n';
    fwrite(log_buffer, used+1, 1, stdout);
  }
  else {
    fwrite(log_buffer, used, 1, stdout);
  }
  fflush(stdout);
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
    fprintf(stderr, "    -V : display program version\n");
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

  case 'V':
    fprintf(stderr, "%s: $s\n", program, PROGRAM_VERSION);
    exit(0);
  }

  return -1;
}

int main(int argc, char *argv[], char *envp[]) {
  char    *snapshot_addr;
  char     buf[LOGBUF_SIZE];
  void    *snapshot;
  param_t *p;
  int      ret, n;
  int      used, left;

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, environment, then command-line argument. */
  push_param_from_env(envp, globals, n_global_params);
  program = argv[0];
  ret = getopt_long_params(argc, argv, "vqh", globals, n_global_params, opt_handler);
  if( ret < 0 ) {
    if(errno)
      fprintf(stderr, "%s: Problem handling arguments: %s\n", program, strerror(errno));
    exit(1);
  }

  if(debug_level > 1)		/* Dump global parameters for debugging purposes */
    debug_params(globals, n_global_params);

  /* Create the ZMQ contexts */
  zmq_main_ctx  = zmq_ctx_new();
  if( !zmq_main_ctx ) {
    fprintf(stderr, "%s: ZeroMQ context creation failed: %s\n", program, strerror(errno));
    exit(1);
  }

  /* Create the socket to talk to the snapshot program */
  snapshot = zmq_socket(zmq_main_ctx, ZMQ_REQ);
  if( snapshot == NULL ) {
    fprintf(stderr, "Unable to create socket to snapshot: %s\n", strerror(errno));
    zmq_ctx_term(zmq_main_ctx);
    exit(2);
  }

  p = find_param_by_name("snapshot", 8, globals, n_global_params);
  assert( p != NULL );		/* Fatal if parameter not found */
  if( get_param_value(p, &snapshot_addr) < 0 ) {
    fprintf(stderr, "%s: Cannot get value for snapshot address parameter\n", program);
    exit(2);
  }

  /* Connect the socket to talk to snapshot */
  ret = zmq_connect(snapshot, snapshot_addr);
  if( ret < 0) {
    fprintf(stderr, "%s: Connecting snapshot socket to %s failed: %s\n", program, snapshot_addr, strerror(errno));
    zmq_ctx_term(zmq_main_ctx);
    exit(2);
  }

  /* Send the message, wait for the reply */
  if(debug_level > 1)
    fprintf(stderr, "Sending message...\n");
  if(debug_level > 0)
    fprintf(stderr, "Build:");

  used = 0;
  left = LOGBUF_SIZE-1;
  for(n=optind; n<argc; n++) {
    int  len;

    len = snprintf(&buf[used], left, "%s ", argv[n]);
    if(debug_level > 0)
      fprintf(stderr, "[%s]", buf);
    used += len;
    left -= len;
  }
  if(debug_level > 0)
    fprintf(stderr, "\n");  

  ret = zh_put_msg(snapshot, 0, used-1, buf);
  if( ret < 0 ) {
    fprintf(stderr, "\n%s: sending message failed\n", program);
  }

  /* Wait for reply */
  if(debug_level > 1)
    fprintf(stderr, "Awaiting reply...\n");
  print_message(snapshot);

  /* Clean up ZeroMQ sockets and context */
  zmq_close(snapshot);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}
