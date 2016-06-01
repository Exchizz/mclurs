#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

/*
 * Program to generate triggered snapshots manually.  This communicates with the
 * snapshotter via ZMQ.
 *
 * Arguments:
 * --verbose|-v         Increase reporting level
 * --quiet|-q           Decrease reporting level
 * --snapshot|-s        The snapshotter socket address
 * --pre                Pre-trigger interval
 * --post               Post-trigger interval
 * --trigger            Timepoint of trigger
 * --wait-for-it|-w     Wait for a key-press to generate trigger
 * --repeat|-r          Generate multiple triggers instead of just one
 * --auto|-a            Generate the snapshot name automatically
 * --help|-h            Print usage message
 * --version            Print program version
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "assert.h"

#include <zmq.h>
#include <argtable2.h>
#include <regex.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include "argtab.h"
#include "util.h"
#include "param.h"

#include "defaults.h"

/*
 *  Program source version
 */

#define PROGRAM_VERSION "1.0"
#define VERSION_VERBOSE_BANNER  "MCLURS ADC toolset...\n"

/*
 * Auto-name format options
 */

#define AUTO_NAME_FORMAT_DEFAULT "iso"
#define AUTO_NAME_FORMAT_REX     "hex|iso|utc|tai|seq"

/*
 * Code for automatic snapshot name generation -- interprets format options
 */

typedef enum {
  SNAPNAME = 0,
  HEXADECIMAL,
  TAI64N,
  ISOUTC,
  ISODATE,
  SEQUENTIAL,
  SPECIAL,
} name_mode;

static name_mode determine_auto_mode(const char *auto_name) {
  if( !auto_name )
    return SNAPNAME;
  if( !strcmp("hex", auto_name) )
    return HEXADECIMAL;
  if( !strcmp("tai", auto_name) )
    return TAI64N;
  if( !strcmp("utc", auto_name) )
    return ISODATE;
  if( !strcmp("iso", auto_name) )
    return ISODATE;
  if( !strcmp("seq", auto_name) )
    return SEQUENTIAL;
  return SPECIAL;
}

/*
 * Global parameters for the snapshot program
 */

extern const char *snapshot_addr;
extern const char *auto_name;
extern uint32_t    window_pre;
extern uint32_t    window_pst;

param_t globals[] ={
  { "snapshot", SNAPSHOT_COMMAND, &snapshot_addr, PARAM_TYPE(string), PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket, default '" SNAPSHOT_COMMAND "'"
  },
  { "pre", "1000", &window_pre, PARAM_TYPE(int32), PARAM_SRC_ARG,
    "pre-trigger duration [ms]"
  },
  { "pst", "500", &window_pst, PARAM_TYPE(int32), PARAM_SRC_ARG,
    "post-trigger duration [ms]"
  },
  { "auto", AUTO_NAME_FORMAT_DEFAULT, &auto_name, PARAM_TYPE(string), PARAM_SRC_ARG,
    "format of auto-generated name"
  },
};

const int n_global_params =     (sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int   verbose = 0;
char *program   = NULL;

/* Command line syntax options */

struct arg_lit *h1, *vn1, *v1, *q1;
struct arg_end *e1;

BEGIN_CMD_SYNTAX(help) {
  v1  = arg_litn("v",   "verbose", 0, 3,        "Increase verbosity"),
  q1  = arg_lit0("q",  "quiet",                 "Decrease verbosity"),
  h1  = arg_lit0("h",   "help",                 "Print usage help message"),
  vn1 = arg_lit0(NULL,  "version",              "Print program version string"),
  e1  = arg_end(20)
} APPLY_CMD_DEFAULTS(help) {
  /* No defaults to apply here */
} END_CMD_SYNTAX(help)

struct arg_lit *v2, *q2, *w2;
struct arg_end *e2;
struct arg_str *u2;
struct arg_int *pb2, *pe2;
struct arg_str *n2;

BEGIN_CMD_SYNTAX(single) {
  v2  = arg_litn("v",   "verbose", 0, 3,        "Increase verbosity"),
  q2  = arg_lit0("q",  "quiet",                 "Decrease verbosity"),
  u2  = arg_str0("s",  "snapshot", "<url>",     "URL of snapshotter command socket"),
  pb2 = arg_int0(NULL, "pre", "<int>",          "Pre-trigger interval [ms]"),
  pe2 = arg_int0(NULL, "pst,post", "<int>",     "Post-trigger interval [ms]"),
  w2  = arg_lit0("w", "wait-for-it",            "Wait for keypress to trigger"),
  n2  = arg_str1(NULL, NULL, "<snapshot name>", "Name of the snapshot file"),
  e2  = arg_end(20)
} APPLY_CMD_DEFAULTS(single) {
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params);
} END_CMD_SYNTAX(single);

struct arg_lit *v3, *q3, *w3;
struct arg_end *e3;
struct arg_str *u3;
struct arg_rex *a3;
struct arg_int *pb3, *pe3;

BEGIN_CMD_SYNTAX(autoname) {
  v3  = arg_litn("v",   "verbose", 0, 3,        "Increase verbosity"),
  q3  = arg_lit0("q",  "quiet",                 "Decrease verbosity"),
  u3  = arg_str0("s",  "snapshot", "<url>",     "URL of snapshotter command socket"),
  pb3 = arg_int0(NULL, "pre", "<int>",          "Pre-trigger interval [ms]"),
  pe3 = arg_int0(NULL, "pst,post", "<int>",     "Post-trigger interval [ms]"),
  w3  = arg_lit0("w", "wait-for-it",            "Wait for keypress to trigger"),
  a3  = arg_rex1("a", "auto", AUTO_NAME_FORMAT_REX, "<format>", REG_EXTENDED,   "Automatic snapshot name"),
  e3  = arg_end(20)
} APPLY_CMD_DEFAULTS(autoname) {
  a3->hdr.flag |= ARG_HASOPTVALUE;
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params);
} END_CMD_SYNTAX(autoname);

struct arg_lit *v4, *q4, *r4, *w4;
struct arg_end *e4;
struct arg_str *u4;
struct arg_rex *a4;
struct arg_int *pb4, *pe4;

BEGIN_CMD_SYNTAX(repeat) {
  v4  = arg_litn("v",   "verbose", 0, 3,        "Increase verbosity"),
  q4  = arg_lit0("q",  "quiet",                 "Decrease verbosity"),
  u4  = arg_str0("s",  "snapshot", "<url>",     "URL of snapshotter command socket"),
  pb4 = arg_int0(NULL, "pre", "<int>",          "Pre-trigger interval [ms]"),
  pe4 = arg_int0(NULL, "pst,post", "<int>",     "Post-trigger interval [ms]"),
  a4  = arg_rex0("a", "auto", AUTO_NAME_FORMAT_REX, "<format>", REG_EXTENDED,   "Automatic snapshot name"),
        arg_rem(NULL, "<format> is " AUTO_NAME_FORMAT_REX),
  w4  = arg_lit0("w", "wait-for-it",            "Wait for keypress to trigger"),
  r4  = arg_lit1("r", "repeat",                 "Loop, generating multiple triggers (implies -wa)"),
  e4  = arg_end(20)
} APPLY_CMD_DEFAULTS(repeat) {
  a4->hdr.flag |= ARG_HASOPTVALUE;
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params);
} END_CMD_SYNTAX(repeat);

/* Standard help routines: display the version banner */
void print_version(FILE *fp, int verbosity) {
  fprintf(fp, "%s: Vn. %s\n", program, PROGRAM_VERSION);
  if(verbosity > 0) {           /* Verbose requested... */
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
 * Globals
 */

void   *zmq_main_ctx;           /* ZMQ context for messaging */

const char *auto_name;          /* Auto-generate snapshot path value */
name_mode   auto_mode;          /* The basis for snapshot name generation */
const char *snap_name;          /* The base name if not auto */
const char *snapshot_addr;      /* URL of the snapshotter program */
int         wait_for_it;        /* Wait for keypress before making message */
int         repeat;             /* Don't just do one, do many triggers */
uint32_t    window_pre;         /* Window pre-trigger interval [ms] */
uint32_t    window_pst;         /* Window post-trigger interval [ms] */

/*
 * Process a (possibly multipart) log message.
 * Collect the various pieces and write to stderr
 * Use a 1024 byte logging buffer
 */

#define LOGBUF_SIZE     1024

void print_message(void *socket) {
  char log_buffer[LOGBUF_SIZE];
  int used;

  used = zh_collect_multi(socket, &log_buffer[0], LOGBUF_SIZE-1, "");
  /* If not quiet or an error then show the message */
  if(verbose > 0 || !strncmp(&log_buffer[0], "NO:", 3)) {
    if( log_buffer[used-1] != '\n') {
      log_buffer[used] = '\n';
      fwrite(log_buffer, used+1, 1, stdout);
    }
    else {
      fwrite(log_buffer, used, 1, stdout);
    }
    fflush(stdout);
  }
}

/*
 * Wait for a keypress to generate a trigger time.
 *
 */

uint64_t wait_for_keypress(uint64_t *now_as_ns) {
  struct timespec now;

  if(now_as_ns == NULL)
    return -1;

  fputc('>', stdout);
  switch( fgetc(stdin) ) {
  case EOF:
  case 'q':
    return -1;

  case 's':
    repeat = 0;
    break;

  default:
    break;
  }

  /* Discover the current time, as trigger point */
  clock_gettime(CLOCK_MONOTONIC, &now);
  *now_as_ns = now.tv_sec;
  *now_as_ns = *now_as_ns * 1000000000 + now.tv_nsec;
  return 0;
}

/*
 * Construct the name of a snapshot file.  Use the supplied name unless in auto mode.
 */

char *make_path_value(char buf[], int size, const char *snapname, uint64_t trigger, name_mode mode) {
  static int counter = 0;
  time_t     trig;
  uint64_t   secs;
  int        ns;
  int        used;
  struct tm *t;

  switch(mode) {

  case SNAPNAME:                /* Use the supplied snapshot name */
    snprintf(&buf[0], size, "%s", snapname);
    break;

  case TAI64N:                  /* Use a TAI64N format timestamp */
    assertv(size >= 25, "Buffer too small (%d) for TAI path\n", size);
    secs = trigger / 1000000000;
    ns = trigger - secs * 1000000000;
    snprintf(&buf[0], size, "@%016llx%08lx", secs|0x4000000000000000, ns);
    break;

  case ISODATE:
  case ISOUTC:                  /* Use an ISO standard date with fractional seconds */
    assertv(size >= 26, "Buffer too small (%d) for ISO path\n", size);
    trig = trigger / 1000000000;
    ns = trigger - trig * 1000000000;
    t = (mode==ISOUTC? gmtime(&trig) : localtime(&trig));
    used = strftime(&buf[0], size, "%FT%T", t); /* 2015-07-14T16:55:32.nnnnnn */
    snprintf(&buf[used], size-used, ".%06d", ns/1000);
    break;

  case HEXADECIMAL:             /* Use a hexadecimal print of the trigger time */
    assertv(size >= 16, "Buffer too small (%d) for HEX path\n", size);
    snprintf(&buf[0], size, "%016llx", trigger);
    break;

  case SEQUENTIAL:              /* Generate a sequentially incrementing snapshot name */
    assertv(size >= 10, "Buffer too small (%d) for SEQ path\n", size);
    snprintf(&buf[0], size, "snap%06d", counter++);
    break;

  case SPECIAL:                 /* User-supplied format, not yet implemented */
  default:
    snprintf(&buf[0], size, "%s%d", "unimplemented", counter++);
    break;
  }
  return &buf[0];
}

/*
 * Main entry point
 */

#define PATHBUF_SIZE 128

int main(int argc, char *argv[], char *envp[]) {
  char     buf[LOGBUF_SIZE];
  void    *snapshot;
  int      ret;
  int      used, left;
  uint64_t trigger;
  struct timespec now;
  uint64_t now_as_ns;

  /* Discover the current time, as trigger point */
  clock_gettime(CLOCK_MONOTONIC, &now);
  now_as_ns = now.tv_sec;
  now_as_ns = now_as_ns * 1000000000 + now.tv_nsec;

  program = argv[0];

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, then environment. */
  set_param_from_env(envp, globals, n_global_params);

  /* 2. Process parameters:  push values out to program globals */
  ret = assign_all_params(globals, n_global_params);
  assertv(ret == 0, "Push parameters failed on param %d out of %d\n", -ret, n_global_params);

  //  fprintf(stderr, "Before command line processing, after environment\n");
  //  debug_params(stderr, globals, n_global_params);

  /* 3. Create and parse the command lines -- installs defaults from parameter table */
  void **cmd_help     = arg_make_help();
  void **cmd_single   = arg_make_single();
  void **cmd_autoname = arg_make_autoname();
  void **cmd_repeat   = arg_make_repeat();

  /* Try first syntax */
  int err_help = arg_parse(argc, argv, cmd_help);
  if( !err_help ) {             /* Assume this was the desired command syntax */
    int verbose = v1->count - q1->count;
    if(vn1->count)
      print_version(stdout, verbose);
    if(h1->count || !vn1->count) {
      print_usage(stdout, cmd_help, verbose>0, program);
      print_usage(stdout, cmd_single, verbose>0, program);
      print_usage(stdout, cmd_autoname, verbose>0, program);
      print_usage(stdout, cmd_repeat, verbose, program);
    }
    exit(0);
  }

  struct arg_end  *found = NULL;
  void           **table = NULL;
  int errs = 0, min_errs  = 100;

  /* Try remaining syntaxes */
  errs = arg_parse(argc, argv, cmd_single);
  if( !errs || errs < min_errs ) {      /* Choose single trigger manual-named mode */
    found = e2;
    table = cmd_single;
    verbose = v2->count - q2->count;
    min_errs = errs;
    if( !errs ) {
      auto_name = NULL;
      repeat = 0;
      wait_for_it = w2->count;
      snap_name = n2->sval[0];
    }
  }

  if( errs ) {
    errs = arg_parse(argc, argv, cmd_autoname);
    if( !errs || errs < min_errs) {      /* Choose single trigger auto-named mode */
      found = e3;
      table = cmd_autoname;
      verbose = v3->count - q3->count;
      min_errs = errs;
      if( !errs ) {
        repeat = 0;
        wait_for_it = w2->count;
        snap_name = NULL;
      }
    }
  }

  if( errs ) {
    errs = arg_parse(argc, argv, cmd_repeat);
    if( !errs || errs < min_errs ) {    /* Choose multi-trigger mode */
      found = e4;
      table = cmd_repeat;
      verbose = v4->count - q4->count;
      min_errs = errs;
      if( !errs ) {
        repeat = 1;
        wait_for_it = 1;
        if( !a4->count || !w4->count ) {
          if(verbose >= 0)
            fprintf(stderr, "%s: Warning -- repeat (-r) implies -a and -w, using --auto=%s\n", program, auto_name);
        }
        snap_name = NULL;
      }
    }
  }

  /* Now found indicates the command line with minimum errors in parse */

  if( min_errs ) {              /* No command line matched precisely */
    arg_print_errors(stderr, found, program);
    print_usage(stderr, cmd_help, verbose>0, program);
    print_usage(stderr, cmd_single, verbose>0, program);
    print_usage(stderr, cmd_autoname, verbose>0, program);
    print_usage(stderr, cmd_repeat, verbose, program);
    exit(1);
  }

  //  fprintf(stderr, "After commandline choice, before reverse push\n");
  //  debug_params(stderr, globals, n_global_params);

  /* 4. Process parameters:  copy argument values back through the parameter table */
  ret = arg_results_to_params(table, globals, n_global_params);

  //  fprintf(stderr, "After reverse push\n");
  //  debug_params(stderr, globals, n_global_params);
  
  /* Check the auto argument and compute the path generation mode */
  auto_mode = determine_auto_mode(auto_name);

  /* 5. All syntax tables are finished with now: clean up the mess :-)) */
  arg_free(cmd_help);
  arg_free(cmd_single);
  arg_free(cmd_autoname);
  arg_free(cmd_repeat);

  if(verbose > 2)               /* Dump global parameters for debugging purposes */
    debug_params(stderr, globals, n_global_params);

  /* Create the ZMQ contexts */
  zmq_main_ctx  = zmq_ctx_new();
  if( !zmq_main_ctx ) {
    fprintf(stderr, "%s: Error -- ZeroMQ context creation failed: %s\n", program, strerror(errno));
    exit(2);
  }

  /* Create the socket to talk to the snapshot program */
  snapshot = zh_connect_new_socket(zmq_main_ctx, ZMQ_REQ, snapshot_addr);
  if( snapshot == NULL ) {
    fprintf(stderr, "%s: Error -- unable to create socket to snapshot at %s: %s\n",
            program, snapshot_addr, strerror(errno));
    zmq_ctx_term(zmq_main_ctx);
    exit(2);
  }

  /* Look at the parameters to construct the snap command */
  if(window_pre > 10000) {
    fprintf(stderr, "%s: Error -- maximum allowed capture window is 10[s]\n", program);
    exit(3);
  }

  trigger = now_as_ns;
  do {
    uint64_t time_start, time_stop;
    char  path_buf[PATHBUF_SIZE];
    const char *path;
    int   ret = 0;

    if( wait_for_it )
      ret = wait_for_keypress(&trigger);

    if( ret < 0 ) {
      break;
    }

    path = make_path_value(&path_buf[0], PATHBUF_SIZE-1, snap_name, trigger, auto_mode);

    // fprintf(stderr, "Window parameters pre %d post %d\n", window_pre, window_pst);

    time_start = trigger - 1000000 * (uint64_t) window_pre;
    time_stop  = trigger + 1000000 * (uint64_t) window_pst;

    // fprintf(stderr, "Window parameters start %lld stop %lld\n", time_start, time_stop);
    
    /* Send the message, wait for the reply */
    left = LOGBUF_SIZE-1;
    used = snprintf(&buf[0], left, "snap begin=%lld, end=%lld, path=%s", time_start, time_stop, path);
    buf[used] = '\0';
    if(verbose > 1)
      fprintf(stderr, "Sending: %s\n", &buf[0]);
    ret = zh_put_msg(snapshot, 0, used, buf);
    if( ret < 0 ) {
      fprintf(stderr, "%s: Error -- sending message to %s failed\n", program, snapshot_addr);
      break;
    }

    /* Wait for reply */
    if(verbose > 1)
      fprintf(stderr, "Awaiting reply...\n");

    print_message(snapshot);

  } while(repeat);

  /* Clean up ZeroMQ sockets and context */
  zmq_close(snapshot);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}
