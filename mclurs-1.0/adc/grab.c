#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

/*
 * Program to grab data from USBDUXfast via Comedi.
 *
 * Arguments:
 * --verbose|-v         Increase reporting level
 * --freq|-f            Sampling frequency in [Hz], default 2.5 [MHz]
 * --range|-r           ADC range 'hi' (750 mVpk) or 'lo' (500 mVpk)
 * --raw                ADC output as raw data
 * --device|-d          Comedi device to use, default /dev/comedi0
 * --bufsz|-B           Comedi buffer size to request [MiB], default 40 [MiB]
 * --help|-h            Print usage message
 * --version            Print program version
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "assert.h"
#include <argtable2.h>
#include <regex.h>
#include <comedi.h>
#include <comedilib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "argtab.h"
#include "mman.h"
#include "lut.h"

#define N_CHANS      16
#define BUFSZ      4096
#define BUFSPSZ    (BUFSZ/sizeof(sampl_t))

char read_buf[BUFSZ];

#define COMEDI_DEVICE   "/dev/comedi0"

#define COMEDIBUFFERSIZE (*40)
#define COMEDIBUFFERSPLS (COMEDIBUFFERSIZE/sizeof(sampl_t))

#define PROGRAM_VERSION         "2.0"
#define VERSION_VERBOSE_BANNER  "MCLURS ADC toolset...\n"

/* Standard arguments + flags */
int   verbose = 0;
char *program = NULL;

/* Command line syntax options */

struct arg_lit *h1, *vn1, *v1;
struct arg_end *e1;

BEGIN_CMD_SYNTAX(help) {
  v1  = arg_litn("v",   "verbose", 0, 2,        "Increase verbosity"),
  h1  = arg_lit0("h",   "help",                 "Print usage help message"),
  vn1 = arg_lit0(NULL,  "version",              "Print program version string"),
  e1  = arg_end(20)
} APPLY_CMD_DEFAULTS(help) {
  /* No defaults to apply here */
} END_CMD_SYNTAX(help)

struct arg_lit *v2, *rw2;
struct arg_int *b2;
struct arg_dbl *f2;
struct arg_str *d2;
struct arg_rex *rn2;
struct arg_end *e2;

BEGIN_CMD_SYNTAX(main) {
  v2  = arg_litn("v",   "verbose", 0, 2,                "Increase verbosity"),
  f2  = arg_dbl0("f",   "freq", "<real>",               "Sampling frequency [Hz], default 2.5[MHz]"),
  b2  = arg_int0("B",   "bufsz", "<int>",               "Comedi buffer size [MiB], default 40[MiB]"),
  d2  = arg_str0("d",   "device", "<path>",             "Comedi device to open, default /dev/comedi0"),
  rn2 = arg_rex0("r",   "range", "hi|lo", NULL, REG_EXTENDED,   "Specify range in {hi, lo}, default hi"),
  rw2 = arg_lit0(NULL,  "raw",                          "Emit raw ADC sample values"),
  e2  = arg_end(20)
} APPLY_CMD_DEFAULTS(main) {
  *f2->dval  = 2.5e6;           /* Default frequency 2.5[MHz] */
  *b2->ival  = 40;              /* Default buffer size 40[MiB] */
  *d2->sval  = COMEDI_DEVICE;   /* Default device for Comedi */
  *rn2->sval = "hi";            /* Default ADC range (hi) */
} END_CMD_SYNTAX(main);

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
 * The main() entry point.
 */

int main(int argc, char *argv[]) {
  float        sr_total;
  int          bufsz;
  char        *device;
  int          range = 0;       /* Default range is +/- 750mV */

  int          buf_samples;
  unsigned int convert_arg;
  comedi_t    *dev;
  int          errs, ret, i;
  unsigned int chanlist[N_CHANS];
  void        *map;
  sampl_t     *start;
  uint64_t     head, tail;
  int          data_coming;
  convertfn    convert;

  program = argv[0];

  /* Create and parse the command lines */
  void **cmd_help = arg_make_help();
  void **cmd_main = arg_make_main();

  /* Try first syntax */
  int err_help = arg_parse(argc, argv, cmd_help);
  if( !err_help && (vn1->count || h1->count) ) {                /* Assume this was the desired command syntax */
    if(vn1->count)
      print_version(stdout, v1->count);
    if(h1->count || !vn1->count) {
      print_usage(stdout, cmd_help, v1->count>0, program);
      print_usage(stdout, cmd_main, v1->count, program);
    }
    exit(0);
  }

  /* Try second syntax */
  int err_main = arg_parse(argc, argv, cmd_main);
  if( err_main ) {              /* This is the default desired syntax; give full usage */
    arg_print_errors(stderr, e2, program);
    print_usage(stderr, cmd_help, v2->count>0, program);
    print_usage(stderr, cmd_main, v2->count, program);
    exit(1);
  }

  /* The second syntax was correctly parsed, so retrieve the important values from the table */
  errs = 0;

  /* Deal with the sampling frequency */
  sr_total    = f2->dval[0];
  if(sr_total < 5e4 || sr_total > 3e6) {
    fprintf(stderr, "%s: Error -- total sample rate %g[Hz] out of sensible range (50[kHz] to 3[MHz])\n", program, sr_total);
    errs++;
  }
  convert_arg = (unsigned int) 1e9 / sr_total;

  /* Deal with the requested buffer size */
  bufsz       = b2->ival[0];
  if(bufsz < 8 || bufsz > 256) {
    fprintf(stderr, "%s: Error -- requested buffer size %d[MiB] out of sensible range (8 to 256[MiB])\n", program, bufsz);
    errs++;
  }
  bufsz *= 1048576;

  /* Deal with the Comedi device */
  device      = (char *) d2->sval[0];
  if( !(dev = comedi_open(device)) ) {
    fprintf(stderr, "%s: Error -- cannot open %s: %s\n", program, device, comedi_strerror(comedi_errno()));
    errs++;
  }

  /* Deal with specification of range and raw */
  range       = !strcmp(rn2->sval[0], "hi") ? 0 : 1;
  if(rw2->count) {              /* Requested raw */
    convert   = convert_raw_raw;
  } else {                      /* Convert from specified range  */
    convert   = range? convert_raw_500mV : convert_raw_750mV;
  }

  /* Record desired verbosity */
  verbose     = v2->count;

  /* All finished with the argument syntax tables */
  arg_free(cmd_main);
  arg_free(cmd_help);

  /* Exit 2 if argument errors */
  if(errs) {
    exit(2);
  }

  fprintf(stderr, "%s %s\n\n", program, PROGRAM_VERSION);
  fprintf(stderr, "Total sample rate requested = %g[Hz]\n", sr_total);
  fprintf(stderr, "Using ADC range +/-%s[mV] full-scale\n", range? "500" : "750");

  comedi_cmd *cmd = (comedi_cmd *) calloc(1, sizeof(comedi_cmd));
  if( !cmd ) {
    fprintf(stderr, "%s: Error -- failed to get memory for Comedi command structure: %s\n", program, strerror(errno));
    exit(3);
  }

  ret = comedi_get_max_buffer_size(dev, 0);

  if(ret < bufsz) {
    if(comedi_set_max_buffer_size(dev, 0, bufsz) < 0 ) {
      fprintf(stderr, "%s: Error -- failed to set %s max buffer size to %d[B]: %s\n", program, device, bufsz, comedi_strerror(comedi_errno()));
      exit(3);
    }
  }
  else {
    if(verbose)
      fprintf(stderr, "%s: Comedi maximum buffer size requested %d[B], actual %d[B]\n", program, bufsz, ret);
  }

  if(comedi_set_buffer_size(dev, 0, bufsz) < 0) {
    fprintf(stderr, "%s: Error -- failed to set %s buffer size to %d[B]: %s\n", program, device, bufsz, comedi_strerror(comedi_errno()));
    exit(3);
  }

  /* Actual buffer may be larger than requested -- buffer does not shrink! */
  bufsz = comedi_get_buffer_size(dev, 0);
  buf_samples = bufsz / sizeof(sampl_t);

  for(i=0; i<N_CHANS; i++)
    chanlist[i] = CR_PACK(i, range, AREF_GROUND);

  /* Print numbers for clipped inputs */
  comedi_set_global_oor_behavior(COMEDI_OOR_NUMBER);

  /* get the correct command structure to run usbduxfast */
  if((ret = comedi_get_cmd_generic_timed(dev, 0, cmd, N_CHANS, 0)) < 0) {
    fprintf(stderr, "%s: Error -- comedi_get_cmd_generic_timed failed for %s: %s\n", program, device, comedi_strerror(comedi_errno()));
    exit(3);
  }

  populate_conversion_luts();

  /* adjust some cmd parameters */
  cmd->chanlist    = chanlist;
  cmd->stop_src    = TRIG_NONE;
  cmd->stop_arg    = 0;
  convert_arg = (unsigned int) 1e9 / sr_total;
  cmd->convert_arg = convert_arg;

  /* call test twice because different things are tested? 
   * if tests are successful run sampling command */
  if( (ret = comedi_command_test(dev, cmd)) != 0 && verbose > 1 ) {
    fprintf(stderr, "First test, err: %s; ", comedi_strerror(comedi_errno()));
    fprintf(stderr, " cmd->convert_arg = %d\n", cmd->convert_arg);
  } 
  if( (ret = comedi_command_test(dev, cmd)) != 0 && verbose > 1 ) {
    fprintf(stderr, "Second test, err: %s; ", comedi_strerror(comedi_errno()));
    fprintf(stderr, " cmd->convert_arg = %d\n", cmd->convert_arg);
  }

  map = mmap_and_lock(comedi_fileno(dev), 0, bufsz, PROT_RDONLY|PREFAULT_RDONLY|MAL_DOUBLED|MAL_LOCKED);
  if(map == NULL) {
    fprintf(stderr, "%s: Error -- failed to map Comedi buffer to RAM: %s\n", program, strerror(errno));
    exit(3);
  }
  start = (sampl_t *) map;

  if(verbose)
    fprintf(stderr, "%s: Comedi buffer (size %u[B]) mapped at %p\n", program, bufsz, start);

  if( (ret = comedi_command(dev, cmd))       < 0 ) {
    fprintf(stderr, "%s: Error -- Comedi command returns %d: %s\n", program, ret, comedi_strerror(comedi_errno()));
    exit(4);
  }

  if(verbose > 1) {
    fprintf(stderr, "comedi_command returns %d\n", ret);
    fprintf(stderr, "stop src = %d\n", cmd->stop_src);
    fprintf(stderr, "stop arg = %d\n", cmd->stop_arg);
    fprintf(stderr, "convert arg = %d\n", cmd->convert_arg);
  }

  if(verbose)
    fprintf(stderr, "%s: Total sample rate allocated = %g[Hz]\n", program, 1e9 / cmd->convert_arg);

  head=tail=0;
  data_coming = 1000;           /* Is data arriving? After this many pauses with no data, exit... */
  while( 1 ) {
    int nb  = comedi_get_buffer_contents(dev, 0); /* Find out how many new bytes there are */
    sampl_t *back = &start[ tail % buf_samples ];
    ret = 0;

    if(nb <= 0) {
      usleep(10000);
      if( --data_coming == 0 ) break;
      continue;
    }

    data_coming = 100;          /* Some data has come, use a smaller value henceforth */
    head += nb/sizeof(sampl_t); /* This many new samples have arrived */
    nb = head - tail;           /* And this is how many remain to process */

    while( nb >= BUFSPSZ ) {
      /* Convert and dump a buffer-full to stdout, repeat while possible */
      (*convert)((sampl_t *)read_buf, back, BUFSPSZ, (sampl_t)0);
      ret  =  comedi_mark_buffer_read(dev, 0, BUFSZ);
      if(ret < 0) {
        fprintf(stderr, "%s: Error -- comedi_mark_buffer_read during loop: %s\n", program, comedi_strerror(comedi_errno()));
        break;
      }
      fwrite(read_buf, sizeof(sampl_t), BUFSPSZ, stdout);
      back += BUFSPSZ;
      tail += BUFSPSZ;
      nb   -= BUFSPSZ;
    }
  }

  fprintf(stderr, "%s: Error? -- Comedi data flow interrupted for more than 1 second\n", program);

  comedi_cancel(dev, 0);
  exit(0);
}
