#

#include <stdio.h>
#include <stdlib.h>

#include <zmq.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <time.h>

#include "util.h"
#include "param.h"
#include "snapshot.h"

/*
 *  Program source version
 */

#define PROGRAM_VERSION	"1.0"

/*
 * Global parameters for the snapshot program
 */

param_t globals[] ={
  { "snapshot",   1, { "ipc://snapshot-CMD", }, PARAM_STRING, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
  { "pre",   1, { "1000", }, PARAM_INT32, PARAM_SRC_ARG,
    "pre-trigger duration [ms]"
  },
  { "pst",   1, { "500", }, PARAM_INT32, PARAM_SRC_ARG,
    "post-trigger duration [ms]"
  },
  { "trigger",   0, { "", }, PARAM_INT64, PARAM_SRC_ARG,
    "timepoint of trigger [ns wrt epoch]"
  },
};

int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int debug_level = 0;
char *program   = NULL;

/*
 * Globals
 */

void   *zmq_main_ctx;		/* ZMQ context for messaging */

int	auto_name = 0;		/* Auto-generate snapshot path value */
int	wait_for_it = 0;	/* Wait for keypress before making message */
int	repeat = 0;		/* Don't just do one, do many triggers */

int   (*auto_name_fn)(char *, int); /* Generate an automatic name */

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

  fprintf(stderr, "Usage: ");
  param_brief_usage(&buf[0], 1024, globals, n_global_params);
  fprintf(stderr, "%s [-vqhrw] %s -a | <snapshot name>\n", program, &buf[0]);

  if(debug_level >= 1) {
    fprintf(stderr, "\n");
    fprintf(stderr, "    -v : increase verbosity\n");
    fprintf(stderr, "    -q : decrease verbosity\n");
    fprintf(stderr, "    -h : display usage message\n");
    fprintf(stderr, "    -V : display program version\n");
    fprintf(stderr, "    -w : wait for keypress\n");
    fprintf(stderr, "    -r : repeated trigger\n");
    fprintf(stderr, "    -a : auto-generate snapshot name\n");
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

  case 'a':
    auto_name = 1;
    return 0;

  case 'r':
    repeat = 1;
    return 0;

  case 'w':
    wait_for_it = 1;
    return 0;

  case 'V':
    fprintf(stderr, "%s: $s\n", program, PROGRAM_VERSION);
    exit(0);
  }

  return -1;
}

uint64_t wait_for_keypress() {
  uint64_t now_as_ns;
  struct timespec now;

  fputc('>', stdout);
  switch( fgetc(stdin) ) {
  case EOF:
  case 'q':
    return 0xFFFFFFFFFFFFFFFF;

  case 's':
    repeat = 0;
    break;

  default:
    break;
  }

  /* Discover the current time, as trigger point */
  clock_gettime(CLOCK_MONOTONIC, &now);
  now_as_ns = now.tv_sec;
  now_as_ns = now_as_ns * 1000000000 + now.tv_nsec;
  return now_as_ns;
}

typedef enum {
  HEXADECIMAL = 1,
  TAI64N = 2,
  ISODATE = 3,
} time_name_mode;

int auto_name_by_time(char *buf, int len, uint64_t trigger, int mode) {
  uint64_t   secs;
  time_t     trig;
  int        ns;
  int        used;
  struct tm *t;

  switch(mode) {
  case TAI64N:
    secs = trigger / 1000000000;
    ns = trigger - secs * 1000000000;
    return snprintf(buf, len, "@%016llx%08lx", secs|0x4000000000000000, ns);

  case ISODATE:
    trig = trigger / 1000000000;
    ns = trigger - trig * 1000000000;
    t = gmtime(&trig);
    used = strftime(buf, len, "%FT%T", t); /* 2015-07-14T16:55:32 */
    if( used ) {			   /* Something was written, buffer was big enough */
      if(used < len) {
	used += snprintf(&buf[used], len-used, ".%0d", ns);
      }
      return used;
    }
    /* FALL THROUGH:  if buffer too small for ISODATE then try HEX */

  case HEXADECIMAL:
    return snprintf(buf, len, "%016llx", trigger);
  }
}

int main(int argc, char *argv[], char *envp[]) {
  char    *snapshot_addr;
  char     buf[LOGBUF_SIZE];
  void    *snapshot;
  param_t *p;
  char    *v;
  int      ret, n;
  int      used, left;
  uint64_t time_start, time_stop;
  uint64_t trigger;
  uint32_t window_pre, window_pst;
  struct timespec now;
  uint64_t now_as_ns;

  /* Discover the current time, as trigger point */
  clock_gettime(CLOCK_MONOTONIC, &now);
  now_as_ns = now.tv_sec;
  now_as_ns = now_as_ns * 1000000000 + now.tv_nsec;

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, environment, then command-line argument. */
  push_param_from_env(envp, globals, n_global_params);
  program = argv[0];
  ret = getopt_long_params(argc, argv, "vqhrwa", globals, n_global_params, opt_handler);
  if( ret < 0 ) {
    if(errno)
      fprintf(stderr, "%s: Problem handling arguments: %s\n", program, strerror(errno));
    exit(1);
  }

  if(debug_level > 1)		/* Dump global parameters for debugging purposes */
    debug_params(globals, n_global_params);

  if( repeat ) {
    if( !auto_name || !wait_for_it ) {
      fprintf(stderr, "%s:  warning -- repeat (-r) implies -a an -w\n", program);
    }
    auto_name = 1;
    wait_for_it = 1;
  }

  if( optind == argc && !auto_name ) {
    fprintf(stderr, "%s: trig requires snapshot path parameter (snapshot name)\n", program);
    usage();
    exit(1);
  }

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

  /* Look at the parameters to construct the snap command */
  p = find_param_by_name("pre", 3, globals, n_global_params);
  assert( p != NULL );		/* Fatal if parameter not found */
  if( get_param_value(p, &v) < 0 ) {
    fprintf(stderr, "%s: Cannot get value for pre-trigger window parameter\n", program);
    exit(2);
  }
  if( assign_value(p->p_type, v, &window_pre) < 0 ) {
    fprintf(stderr, "%s: Cannot assign value for pre-trigger window parameter\n", program);
    exit(2);
  }

  p = find_param_by_name("pst", 3, globals, n_global_params);
  assert( p != NULL );		/* Fatal if parameter not found */
  if( get_param_value(p, &v) < 0 ) {
    fprintf(stderr, "%s: Cannot get value for post-trigger window parameter\n", program);
    exit(2);
  }
  if( assign_value(p->p_type, v, &window_pst) < 0 ) {
    fprintf(stderr, "%s: Cannot assign value for post-trigger window parameter\n", program);
    exit(2);
  }

  if(window_pre + window_pst > 8000) {
    fprintf(stderr, "%s: Maximum allowed capture window is 8000 [ms]\n");
    exit(3);
  }

  p = find_param_by_name("trigger", 7, globals, n_global_params);
  assert( p != NULL );		/* Fatal if parameter not found */
  if( get_param_value(p, &v) < 0 ) {
    trigger = now_as_ns;
  }
  else {
    if( wait_for_it || repeat ) {
      fprintf(stderr, "%s: Trigger point given with -r\n", program);
      exit(2);
    }
    if( assign_value(p->p_type, v, &trigger) < 0 ) {
      fprintf(stderr, "%s: Cannot assign value for trigger point parameter\n", program);
      exit(2);
    }
  }

  do {
    char *path = argv[optind];
    char  path_buf[64];

    if( wait_for_it )
      trigger = wait_for_keypress();

    if( trigger == 0xFFFFFFFFFFFFFFFF ) {
      break;
    }

    if( auto_name ) {
      snprintf(&path_buf[0], 64, "%016llx", trigger);
      path = &path_buf[0];
    }

    time_start = trigger - 1000000 * (uint64_t) window_pre;
    time_stop  = trigger + 1000000 * (uint64_t) window_pst;

    /* Send the message, wait for the reply */
    left = LOGBUF_SIZE-1;
    used = snprintf(&buf[0], left, "snap begin=%lld,end=%lld,path=%s", time_start, time_stop, path);
    buf[used] = '\0';
    if(debug_level > 1)
      fprintf(stderr, "Sending:  %s\n", &buf[0]);
    ret = zh_put_msg(snapshot, 0, used, buf);
    if( ret < 0 ) {
      fprintf(stderr, "\n%s: sending message failed\n", program);
    }

    /* Wait for reply */
    if(debug_level > 1)
      fprintf(stderr, "Awaiting reply...\n");
    print_message(snapshot);

  } while(repeat);

  /* Clean up ZeroMQ sockets and context */
  zmq_close(snapshot);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}
