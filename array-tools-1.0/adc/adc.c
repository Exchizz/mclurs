#

/*
 * ADC interface module
 *
 * This module provides the interface from the Reader to Comedi.  It
 * handles interaction with the Comedi device and mapping the Comedi
 * data buffer.
 *
 * The routines (apart from adc_new(), which returns a pointer to the
 * semi-opaque adc structure representing this object) in this module
 * return 0 on success and -1 on failure; they leave error information
 * in the adc structure from which it can be retrieved with the
 * adc_error() method.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <comedi.h>
#include <comedilib.h>

#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "assert.h"
#include "lut.h"
#include "ring.h"
#include "mman.h"
#include "queue.h"
#include "adc.h"

/*
 * Private information for the ADC module.
 */

#define N_USBDUX_CHANS	16

#define ERRBUFSZ 128

struct _adc
{ comedi_t  *device;		/* The Comedi device handle */
  int	     devflags;		/* Comedi device flags */
  comedi_cmd command;		/* The command to send to the device */
  int	     fd;		/* The device file descriptor for reading data */
  int	     sample_ns;		/* The ADC inter-sample interval [ns] */
  int	     poll_adc_interval;	/* Interval to wait when ADC is running [ms] */
  unsigned   c[N_USBDUX_CHANS]; /* Command chennel list */
  sampl_t   *comedi_buffer;	/* The memory-mapped Comedi streaming buffer  */
  uint64_t   buffer_length;	/* The size of the Comedi streaming buffer [bytes] */
  uint64_t   buffer_samples;	/* The buffer size in samples */
  uint64_t   head,		/* The current sample number at the front of the Comedi buffer */
	     tail;		/* The current last sample number processed in the buffer */
  struct readbuf *ring_buf;	/* Ring buffer handle for Comedi transfer */
  int	     adc_range;		/* The ADC full-scale range: 500 for 500mV and 750 for 750mV */
  void	   (*convert)(sampl_t *, sampl_t *, int); /* LUT conversion function */
  queue	     write_queue;	/* The queue header for the queue of snapshots */
  int        running;		/* True when the ADC command ↓is running */
  char       errbuf[ERRBUFSZ];
  int	     adc_errno;
};

#define USBDUXFAST_COMEDI_500mV	1 /* Bit 3 control output is 0 iff the CR_RANGE is one */
#define USBDUXFAST_COMEDI_750mV	0 /* Bit 3 control output is 1 iff the CR_RANGE is zero */

/*
 * Allocate and set up a new ADC descriptor.
 */

public adc adc_new(char *device) {
  adc ret = calloc(1, sizeof(adc));

  if(ret) {
    init_queue(&ret->write_queue);
    ret->device = comedi_open(device);
    if(ret->device) {
      ret->fd = comedi_fileno(ret->device);
      ret->devflags = comedi_get_subdevice_flags(ret->device, 0);
    }
    else {
      int saved_errno = errno;
      free(ret);
      errno = saved_errno;
      ret = NULL;
    }
  }
  return ret;
}

/*
 * Release ADC resources and free an ADC structure.
 */

public int adc_destroy(adc a) {
  assert(a != NULL);
  adc_stop(a);
  munmap(a->comedi_buffer, 2*a->buffer_length);
  comedi_close(a->device);
  close(a->fd);
  destroy_ring_buffer(a->ring_buf);
  free(a);
  return 0;
}

/*
 * Initialise the ADC structure for data capture.
 */

public int adc_init(adc a, int bufsz, int ns, int range) {
  int ret;
  int i;

  assert(a != NULL);

  /* Initialise Comedi streaming buffer */
  int request = bufsz * 1024 * 1024;
  ret = comedi_get_buffer_size(a->device, 0);
  if( request > ret ) {
    ret = comedi_get_max_buffer_size(a->device, 0);  
    if( request > ret ) {
      ret = comedi_set_max_buffer_size(a->device, 0, request);
      if( ret < 0 ) {
	a->adc_errno = comedi_errno();
	snprintf(&a->errbuf[0], sizeof(a->errbuf), "Comedi set max buffer to %dMiB", bufsz);
	return -1;
      }
    }
    ret = comedi_set_buffer_size(a->device, 0, request);
    if( ret < 0 ) {
      a->adc_errno = comedi_errno();
      snprintf(&a->errbuf[0], sizeof(a->errbuf), "Comedi set streaming buffer to %dMiB", bufsz);
      return -1;
    }
  }

  a->buffer_length = comedi_get_buffer_size(a->device, 0);
  a->buffer_samples = a->buffer_length / sizeof(sampl_t);
  comedi_set_global_oor_behavior(COMEDI_OOR_NUMBER);

  /* Initialise the command structure */
  ret = comedi_get_cmd_generic_timed(a->device, 0, &a->command, N_USBDUX_CHANS, 0);
  if(ret < 0) {
    a->adc_errno = comedi_errno();
    snprintf(&a->errbuf[0], sizeof(a->errbuf), "Comedi set-up-command step 1");
    return -1;
  }

  /* Inter-channel sample period [ns] */
  a->sample_ns = ns;

  /* Set up the conversion function:  500mV or 750mV FSD */
  switch(range) {

  case 1:
  case 500:			/* Narrow FSD range */
    a->convert = convert_raw_500mV;
    range = USBDUXFAST_COMEDI_500mV;
    break;

  case 0:
  case 750:			/* Wide FSD range */
    a->convert = convert_raw_750mV;
    range = USBDUXFAST_COMEDI_750mV;
    break;

  default:
    snprintf(&a->errbuf[0], sizeof(a->errbuf), "Comedi range spec %d unknown", range);
    errno = EINVAL;
    return -1;
  }

  /* Set the command parameters from the reader parameter values */
  for(i=0; i<N_USBDUX_CHANS; i++)
    a->c[i] = CR_PACK_FLAGS(i, range, AREF_GROUND, 0);
  a->command.chanlist    = &a->c[0];
  a->command.stop_src    = TRIG_NONE;
  a->command.stop_arg    = 0;
  a->command.convert_arg = a->sample_ns;

  /* Ask the driver to check the command structure and complete any omissions */
  (void) comedi_command_test(a->device, &a->command);
  ret = comedi_command_test(a->device, &a->command);
  if( ret < 0 ) {
    a->adc_errno = comedi_errno();
    snprintf(&a->errbuf[0], sizeof(a->errbuf), "Comedi set-up-command step 2");
    return -1;
  }

  /* Map the Comedi buffer into memory, twice */
  void *map = mmap_and_lock_double(a->fd, 0, a->buffer_length, PROT_RDONLY|MAL_LOCKED);
  if(map == NULL) {
    a->adc_errno = errno;
    strncpy(&a->errbuf[0], "mmap streaming buffer", sizeof(a->errbuf));
    return -1;
  }
  a->comedi_buffer = map;

  /* Initialise the sample position indices */
  a->head = 0;  /* MAY BE REDUNDANT */
  a->tail = 0;
  return 0;
}

/*
 * Install the ring buffer in the ADC object
 */

public int adc_set_ringbuf(adc a, struct readbuf *r) {
  if(r == NULL) {
    a->adc_errno = errno;
    strncpy(&a->errbuf[0], "set ring buffer", sizeof(a->errbuf));
    return -1;
  }
  a->ring_buf = r;
  return 0;
}
/*
 * Start the ADC data collection.
 */

public int adc_start(adc a) {
  int ret;

  assert(a != NULL);
  /* Execute the command to initiate data acquisition */
  ret = comedi_command(a->device, &a->command);
  if(ret < 0) {
    a->adc_errno = errno;
    strncpy(&a->errbuf[0], comedi_strerror(comedi_errno()), sizeof(a->errbuf));
  }
  else {
    a->running = 1;
  }
  return ret;
}

/*
 * Stop the ADC data collection.
 */

public int adc_stop(adc a) {
  assert(a != NULL);
  if(a->running) {
    comedi_cancel(a->device, 0);
    a->running = 0;
  }
  return 0;
}

/*
 * Fetch up to nsamples samples from Comedi into the ring buffer.
 *
 * If the pointer t is set, then calculate the timestamp of the first
 * sample of the current set, in [ns] after the epoch, and set *t to
 * that value:  used to estimate when ADC conversion begins.
 */

public int adc_fetch(adc a, int nsamples, uint64_t *t) {
  int nb;

  assert(a != NULL);

  /* Retrieve any new data if possible */
  nb = comedi_get_buffer_contents(a->device, 0);
  int ns = nb / sizeof(sampl_t);

  if(nb) {
    int ret;

    /* Used to get first data arrival time */
    if(t != NULL) {
      struct timespec ts;

      clock_gettime(CLOCK_MONOTONIC, &ts);
      *t = ts.tv_sec;
      *t = *t * 1000000000 + ts.tv_nsec;
      *t = *t - a->sample_ns * nb / sizeof(sampl_t);
    }

    /* Limit to maximum requested samples */
    if(nsamples != 0 && ns > nsamples)
      ns = nsamples;

    /* This is the live data start point */
    sampl_t *in  = &a->comedi_buffer[ a->tail % a->buffer_samples ];

    /* This is where to put it, in the ring buffer */
    struct readbuf *r = a->ring_buf;
    sampl_t *out = &((sampl_t *)r->rb_start)[ a->tail % r->rb_samples ];

    a->head += ns;		   /* This many new samples have arrived in the Comedi buffer */
    /* ^^^^^^  MAY BE REDUNDANT */
    (*a->convert)(out, in, ns);    /* Copy the data from in to out with LUT conversion */
    a->tail += ns;		   /* Processed this many new samples */
    ret = comedi_mark_buffer_read(a->device, 0, nb);
    assert(ret == nb);
  }
  return ns;
}

