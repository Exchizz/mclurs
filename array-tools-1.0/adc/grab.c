#

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <assert.h>

#ifdef BYHAND
#include <sys/mman.h>
#else
#include "mman.h"
#endif

#include <comedi.h>
#include <comedilib.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "lut.h"

#define N_CHANS      16
#define BUFSZ      4096
#define BUFSPSZ	   (BUFSZ/sizeof(sampl_t))

char read_buf[BUFSZ];

#define COMEDIBUFFERSIZE (1048576*40)
#define COMEDIBUFFERSPLS (COMEDIBUFFERSIZE/sizeof(sampl_t))

#define PROGRAM_VERSION	"2.0"

char *program;

int main(int argc, char *argv[]) {
  int          ret, i;
  unsigned int chanlist[N_CHANS];
  float        sr_total           = 2.5e6;
  unsigned int convert_arg = (unsigned int) 1e9 / sr_total;
  void        *map;
  sampl_t     *start;
  uint64_t     head, tail;
  int	       data_coming;
  void       (*convert)(sampl_t *, sampl_t *, int);
  int	       range = 0;	/* Default range is +/- 750mV */

  program = argv[0];

  if(argc > 1) {
    if( argv[1][0] == '-' ) {
      switch( argv[1][1] ) {
      case 'V':
	fprintf(stderr, "%s: %s\n", program, PROGRAM_VERSION);
	exit(0);
	/* NOT REACHED */

      case 'r':			/* Range set */
	if(!strcmp(&argv[1][2], "hi")) {
	  range = 0;
	  argc--; argv++;
	  break;
	}
	if(!strcmp(&argv[1][2], "lo")) {
	  range = 1;
	  argc--; argv++;
	  break;
	}
	/* FALL THROUGH */
      default:
	fprintf(stderr, "%s: Unknown argument %s\n\n", program, argv[1]);
	fprintf(stderr, "Usage: %s [-rxx] [fs]\n", program);
	fprintf(stderr, "   xx=hi for 750mV range (default), xx=lo for 500mV range;  fs is sampling frequency in [Hz] (default 2.5 [MHz])\n");
	exit(1);
	/* NOT REACHED */
	break;
      }
    }
  }

  if(argc == 2) {
    float sr = atof(argv[1]);

    if(sr < 5e4 || sr > 4e6) {
      fprintf(stderr, "%s: Total sample rate out of sensible range (50kHz to 4MHz)\n\n", program);
      fprintf(stderr, "Usage: %s [-rxx] [fs]\n", program);
      fprintf(stderr, "   xx=hi for 750mV range (default), xx=lo for 500mV range;  fs is sampling frequency [Hz] (default 2.5 [MHz])\n");
      exit(1);
    }
    sr_total = sr;
  }

  fprintf(stderr, "%s $s\n\n", program, PROGRAM_VERSION);
  fprintf(stderr, "Total sample rate requested = %g Hz\n", sr_total);
  fprintf(stderr, "Using ADC range +/-%s [mV] full-scale\n", range? "500" : "750");

  comedi_cmd *cmd = (comedi_cmd *) calloc(1, sizeof(comedi_cmd));
  assert( cmd != NULL );
  comedi_t   *dev = comedi_open("/dev/comedi0"); 

  if(!dev) {
    comedi_perror("/dev/comedi0");
    exit(2); 
  }

  ret = comedi_get_max_buffer_size(dev, 0);

  if(ret < COMEDIBUFFERSIZE) {
    if(comedi_set_max_buffer_size(dev, 0, COMEDIBUFFERSIZE) < 0 ) {
      fprintf(stderr, "Failed to set max buffer size to %u bytes\n", COMEDIBUFFERSIZE);
      comedi_perror("maxbuf");
      exit(2);
    }
  }
  else {
    fprintf(stderr, "Comedi maximum buffer size requested %d, actual %d\n", COMEDIBUFFERSIZE, ret);
  }

  if(comedi_set_buffer_size(dev, 0, COMEDIBUFFERSIZE) < 0) {
    fprintf(stderr, "Failed to set buffer size to %u bytes\n", COMEDIBUFFERSIZE);
    comedi_perror("setbuf");
    exit(2);
  }

  convert = range? convert_raw_500mV : convert_raw_750mV;
  for(i=0; i<N_CHANS; i++)
    chanlist[i] = CR_PACK(i, range, AREF_GROUND);

  /* Print numbers for clipped inputs */
  comedi_set_global_oor_behavior(COMEDI_OOR_NUMBER);

  /* get the correct command structure to run usbduxfast */
  if((ret = comedi_get_cmd_generic_timed(dev, 0, cmd, N_CHANS, 0)) < 0) {
    fprintf(stderr, "comedi_get_cmd_generic_timed failed\n");
    exit(2);
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
  if( (ret = comedi_command_test(dev, cmd)) != 0 ) {
    fprintf(stderr, "First test, err:");
    fprintf(stderr, "  cmd->convert_arg = %d\n", cmd->convert_arg);
  } 
  if( (ret = comedi_command_test(dev, cmd)) != 0 ) {
    fprintf(stderr, "Second test, err:");
    fprintf(stderr, "  cmd->convert_arg = %d\n", cmd->convert_arg);
  }

#ifdef BYHAND
  map = mmap(NULL, 2*COMEDIBUFFERSIZE, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0); /* Find a suitable address */
  if( map == NULL ) {
    perror("mmap probe");
    exit(3);
  }

  start = mmap(map, COMEDIBUFFERSIZE, PROT_READ, MAP_SHARED|MAP_FIXED, comedi_fileno(dev), 0);
  if( start == NULL || start != map ) {
    perror("mmap buffer");
    exit(3);
  }
  if( mmap(map+COMEDIBUFFERSIZE, COMEDIBUFFERSIZE, PROT_READ, MAP_SHARED|MAP_FIXED, comedi_fileno(dev), 0) == NULL ) {
    perror("mmap buffer again");
    exit(3);
  }
  if( mlock(map, 2*COMEDIBUFFERSIZE) < 0 ) {
    perror("mlock buffer");
    exit(3);
  }
#else
  map = mmap_and_lock_double(comedi_fileno(dev), 0, COMEDIBUFFERSIZE, MAL_LOCKED|PREFAULT_RDONLY);
  if(map == NULL) {
    perror("mmap buffer");
    exit(3);
  }
#endif

  fprintf(stderr, "Comedi buffer (size %u bytes) mapped at 0x%p\n", COMEDIBUFFERSIZE, start);

  if( (ret = comedi_command(dev, cmd))       < 0 ) {
    fprintf(stderr, "command err: %d\n", ret);
    comedi_perror("command");
    exit(4);
  }

  fprintf(stderr, "comedi_command returns %d\n", ret);
  fprintf(stderr, "stop src = %d\n", cmd->stop_src);
  fprintf(stderr, "stop arg = %d\n", cmd->stop_arg);
  fprintf(stderr, "convert arg = %d\n", cmd->convert_arg);

  fprintf(stderr, "Total sample rate allocated = %g Hz\n", 1e9 / cmd->convert_arg);

  head=tail=0;
  data_coming = 1000;		/* Is data arriving? After this many pauses with no data, exit... */
  while( 1 ) {
    int nb  = comedi_get_buffer_contents(dev, 0); /* Find out how many new bytes there are */
    sampl_t *back = &start[ tail % COMEDIBUFFERSPLS ];
    ret = 0;

    if(nb <= 0) {
      usleep(10000);
      if( --data_coming == 0 ) break;
      continue;
    }

    data_coming = 100;		/* Some data has come, use a smaller value henceforth */
    head += nb/sizeof(sampl_t);	/* This many new samples have arrived */
    nb = head - tail;		/* And this is how many remain to process */

    while( nb >= BUFSPSZ ) {
      /* Convert and dump a buffer-full to stdout, repeat while possible */
      (*convert)((sampl_t *)read_buf, back, BUFSPSZ);
      ret  =  comedi_mark_buffer_read(dev, 0, BUFSZ);
      if(ret < 0) {
	comedi_perror("comedi_mark_buffer_read during loop");
	break;
      }
      fwrite(read_buf, sizeof(sampl_t), BUFSPSZ, stdout);
      back += BUFSPSZ;
      tail += BUFSPSZ;
      nb   -= BUFSPSZ;
    }
  }

  comedi_cancel(dev, 0);
  fprintf(stderr, "Data flow from Comedi interrupted for more than 1 second\n");

  exit(0);
}
