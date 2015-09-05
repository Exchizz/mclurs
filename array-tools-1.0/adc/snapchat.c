#

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "assert.h"

#include <argtable2.h>
#include <zmq.h>

#include <getopt.h>

#include "util.h"
#include "param.h"
#include "argtab.h"

/*
 * Snapshot version
 */

#define PROGRAM_VERSION	"1.0"
#define VERSION_VERBOSE_BANNER	"MCLURS ADC toolset...\n"

/*
 * Global parameters for the snapshot program
 */

extern char *snapshot_addr;

param_t globals[] ={
  { "snapshot",   "ipc://snapshot-CMD", &snapshot_addr, param_type_string, PARAM_SRC_ENV|PARAM_SRC_ARG,
    "address of snapshot command socket"
  },
};

const int n_global_params =	(sizeof(globals)/sizeof(param_t));

/*
 * Debugging print out control
 */

int   verbose   = 0;
char *program   = NULL;

/* Command line syntax options */

struct arg_lit *h1, *vn1, *v1, *q1;
struct arg_end *e1;

BEGIN_CMD_SYNTAX(help) {
  v1  = arg_litn("v",	"verbose", 0, 2,	"Increase verbosity"),
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
struct arg_str *n2;
struct arg_str *m2;

BEGIN_CMD_SYNTAX(main) {
  v2  = arg_litn("v",	"verbose", 0, 3,	"Increase verbosity"),
  q2  = arg_lit0("q",  "quiet",			"Decrease verbosity"),
  u2  = arg_str0("s",  "snapshot", "<url>",     "URL of snapshotter command socket"),
  m2  = arg_str0("m",   "multi", "<prefix>",	"Send multiple messages if replies begin with <prefix>"),
  n2  = arg_strn(NULL, NULL, "<args>", 1, 30,	"Message content"),
  e2  = arg_end(20)
} APPLY_CMD_DEFAULTS(main) {
  m2->hdr.flag |= ARG_HASOPTVALUE;
  m2->sval[0] = "";
  INCLUDE_PARAM_DEFAULTS(globals, n_global_params); /* Use defaults from parameter table */
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
 * Snapchat globals...
 */

void      *zmq_main_ctx;	/* ZMQ context for messaging */
char	  *snapshot_addr;	/* The URL of the snapshotter */

/*
 * Print a reply message to stdout
 */

int print_message(char *msg, int size) {
  if( msg[size-1] != '\n') {
    msg[size] = '\n';
    fwrite(msg, size+1, 1, stdout);
  }
  else {
    fwrite(msg, size, 1, stdout);
  }
  fflush(stdout);
}

/*
 * Return true if the string p is an initial prefix of str
 */

int checked_prefix(const char *p, const char *str) {
  while(*p && *str && *p == *str) {
    if( *p != *str )		/* Mismatch with prefix */
      return 0;
    p++, str++;
  }
  return *p? 0 : 1;		/* True iff prefix has run out */
}

/*
 * Main entry point
 */

#define LOGBUF_SIZE	1024

int main(int argc, char *argv[], char *envp[]) {
  const char *prefix = NULL;
  char        buf[LOGBUF_SIZE];
  void       *snapshot;
  param_t    *p;
  int         ret, n;

  program = argv[0];

  /* Set up the standard parameters */
  /* 1. Process parameters:  internal default, then environment. */
  set_param_from_env(envp, globals, n_global_params);

  /* 2. Process parameters:  push values out to program globals */
  ret = assign_all_params(globals, n_global_params);
  assertv(ret == n_global_params, "Push parameters missing some %d/%d done\n", ret, n_global_params); /* If not, there is a coding problem */

  /* 3. Create and parse the command lines -- installs defaults from parameter table */
  void **cmd_help = arg_make_help();
  void **cmd_main = arg_make_main();

  /* Try first syntax */
  int err_help = arg_parse(argc, argv, cmd_help);
  if( !err_help ) {		/* Assume this was the desired command syntax */
    if(vn1->count)
      print_version(stdout, v1->count);
    if(h1->count || !vn1->count) {
      int verbose = v1->count - q1->count;
      print_usage(stdout, cmd_help, verbose>0, program);
      print_usage(stdout, cmd_main, verbose, program);
    }
    exit(0);
  }

  /* Try second syntax */
  int err_main = arg_parse(argc, argv, cmd_main);
  verbose = v2->count - q2->count;
  if( err_main ) {		/* This is the default desired syntax; give full usage */
    arg_print_errors(stderr, e2, program);
    print_usage(stderr, cmd_help, verbose>0, program);
    print_usage(stderr, cmd_main, verbose, program);
    exit(1);
  }

  /* 4. Process parameters:  copy argument values back through the parameter table */
  ret = arg_results_to_params(cmd_main, globals, n_global_params);

  /* 5. Process parameters:  deal with non-parameter table arguments where necessary */
  if(m2->count) {		/* Repeat-mode with prefix */
    prefix = m2->sval[0];
  }

  if(verbose > 2)		/* Dump global parameters for debugging purposes */
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

  const char **msg = n2->sval;
  int          parts = n2->count;

  if(prefix && verbose > 0)
    fprintf(stderr, "Sending %d parts in multi-message mode with reply prefix '%s'\n", parts, prefix);

  do {
    int used, left;

    /* Send the message, wait for the reply;  data is in arg_str *n2 */
    if(verbose > 0)
      fprintf(stderr, "Sending message to %s...\n", snapshot_addr);

    if(verbose > 1)
      fprintf(stderr, "Build:");

    if( !prefix ) {
      used = 0;
      left = LOGBUF_SIZE-1;
      for(n=0; n<parts; n++) {
	int  len;
      
	len = snprintf(&buf[used], left, "%s ", msg[n]);
	if(verbose > 1)
	  fprintf(stderr, " [%s]", buf);
	used += len;
	left -= len;
      }
    }
    else {
      used = snprintf(&buf[0], LOGBUF_SIZE-1, "%s", *msg++);
      parts--;
      if(verbose > 1)
	fprintf(stderr, " [%s]", buf);
    }

    if(verbose > 1)
      fprintf(stderr, "\n");

    /* Send the message, omit the final null */
    ret = zh_put_msg(snapshot, 0, used-1, buf);
    if( ret < 0 ) {
      fprintf(stderr, "\n%s: Error -- sending message failed: %s\n", program, strerror(errno));
      zmq_close(snapshot);
      zmq_ctx_term(zmq_main_ctx);
      exit(3);
    }

    /* Wait for reply */
    if(verbose > 0)
      fprintf(stderr, "Awaiting reply from %s...\n", snapshot_addr);
    used = zh_collect_multi(snapshot, &buf[0], LOGBUF_SIZE-1, "");
    buf[LOGBUF_SIZE-1] = '\0';
    if(verbose >= 0)
      print_message(&buf[0], used);

  } while( prefix && parts > 0 && checked_prefix(prefix, &buf[0]) );

  /* Clean up ZeroMQ sockets and context */
  zmq_close(snapshot);
  zmq_ctx_term(zmq_main_ctx);
  exit(0);
}

