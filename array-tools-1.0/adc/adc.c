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
#include "strbuf.h"
#include "chunk.h"
#include "lut.h"
#include "mman.h"
#include "queue.h"
#include "adc.h"

/*
 * Private information for the ADC module.
 */

#define N_USBDUX_CHANS	16

#define MIN_SAMPLING_FREQUENCY	6e4	/* Minimum sampling frequency per channel [Hz] */
#define MAX_SAMPLING_FREQUENCY  3.75e5	/* Maximum sampling frequency per channel [Hz] */
#define MIN_COMEDI_BUF_SIZE	8      	/* Minimum Comedi Buffer size [MiB] */
#define MAX_COMEDI_BUF_SIZE	256    	/* Maximum Comedi Buffer size [MiB] */

struct _adc {
  const char *a_path;			/* The path to the Comedi device (assumed permanent string) */
  comedi_t   *a_device;			/* Comedi device handle */
  int	      a_devflags;		/* Comedi device flags */
  int	      a_fd;			/* Device file descriptor */
  int	      a_req_bufsz_mib;		/* Requested buffer size [MiB] */
  int	      a_bufsz_bytes;		/* Size of the buffer in bytes */
  int	      a_bufsz_samples;		/* Size of the buffer in samples */
  sampl_t    *a_comedi_ring;		/* Ring buffer for the device */
  double      a_totfrequency;		/* Total sampling frequency */
  int	      a_intersample_ns;		/* Time between samples [ns] */
  int	      a_range;			/* Current conversion range */
  int	      a_raw;			/* Don't convert the data, deliver it raw */
  convertfn   a_convert;		/* Current conversion function */
  comedi_cmd  a_command;		/* Comedi command descriptor structure */
  unsigned    a_chans[N_USBDUX_CHANS];	/* Channel descriptors for hardware channels */
  int	      a_running;		/* True when an ADC data conversion is running */
  int	      a_live;			/* True when we have seen data, and a_start_time is set */
  uint64_t    a_start_time;		/* Time the current data conversion stream started */
  uint64_t    a_head_time;		/* Timestamp of latest buffer sample */
  uint64_t    a_head;			/* Latest sample present in the ring buffer */
  uint64_t    a_tail;			/* Earliest sample present in the ring buffer */
};

#define USBDUXFAST_COMEDI_500mV	1 /* Bit 3 control output is 0 iff the CR_RANGE is one */
#define USBDUXFAST_COMEDI_750mV	0 /* Bit 3 control output is 1 iff the CR_RANGE is zero */

/*
 * ADC is a singleton class, so we can get away with defining a single private structure.
 */

private struct _adc snapshot_adc;

/*
 * Error string function for strbuf module.
 */

private int comedi_error_set_up = 0;

private const char *comedi_error() {
  return comedi_strerror( comedi_errno() );
}

/*
 * Allocate and set up a new ADC descriptor.
 */

public adc adc_new(strbuf e) {
  adc ret = &snapshot_adc;

  if( !comedi_error_set_up++ ) {	/* Install the routine to interpolate %C strings */
    int ret = register_error_percent_handler('C', comedi_error);
    assertv(ret==0, "Failed to register handler for Comedi errors (%%C): %m\n");
  }
  ret->a_fd = -1;
  return ret;
}

/*
 * Release ADC resources and free an ADC structure.
 */

public void adc_destroy(adc a) {
  adc_stop_data_transfer(a);

  if(a->a_fd >= 0)
    close(a->a_fd);

  if(a->a_device)
    comedi_close(a->a_device);    

  if(a->a_comedi_ring)
    munmap(a->a_comedi_ring, a->a_bufsz_bytes);  

  /* Zero the structure -- back to initial state */
  bzero(a, sizeof(struct _adc));
}

/*
 * Set the device path
 */

public int adc_set_device(adc a, const char *device) {
  a->a_path = device;
  return 0;
}

/*
 * Set the total capture sampling frequency from the per-channel frequency.
 */

public int adc_set_chan_frequency(adc a, strbuf e, double *freq) {
  double f = *freq;
  
  if(f < MIN_SAMPLING_FREQUENCY || f > MAX_SAMPLING_FREQUENCY) {
    strbuf_appendf(e, "Sampling frequency %g not within compiled-in ADC limits [%g,%g] Hz",
		   f, MIN_SAMPLING_FREQUENCY, MAX_SAMPLING_FREQUENCY);
    return -1;
  }

  int ns   = 1e9 / (f*NCHANNELS); /* Inter-sample period */
  int xtra = ns % 100;
  
  /* Adjust period for 30[MHz] USBDUXfast clock rate */
  ns = 100 * (ns / 100);
  if( xtra > 17 && xtra < 50 )
    ns += 33;
  if( xtra >= 50 && xtra < 83 )
    ns += 67;
  if( xtra >= 84 )
    ns += 100;
  a->a_intersample_ns = ns; /* Need a plausible value at all times for computing snapshot data */
  a->a_totfrequency = 1e9 / ns;
  *freq = a->a_totfrequency / NCHANNELS;
  return 0;
}

/*
 * Set the desired ring buffer size.
 */

public int adc_set_bufsz(adc a, strbuf e, int bufsz) {
  if(bufsz < MIN_COMEDI_BUF_SIZE || bufsz > MAX_COMEDI_BUF_SIZE) {
    strbuf_appendf(e, "Comedi buffer size %d MiB outwith compiled-in range [%d,%d] MiB",
		   bufsz, MIN_COMEDI_BUF_SIZE, MAX_COMEDI_BUF_SIZE);
    return -1;
  }
  a->a_req_bufsz_mib = bufsz;
  return 0;
}

/*
 * Set the desired ADC range
 */

public int adc_set_range(adc a, strbuf e, int range) {
  /* Set up the conversion function:  500mV or 750mV FSD */
  switch(range) {
  case 500:			/* Narrow FSD range */
    a->a_convert = a->a_raw? convert_raw_raw : convert_raw_500mV;
    a->a_range = USBDUXFAST_COMEDI_500mV;
    break;

  case 750:			/* Wide FSD range */
    a->a_convert = a->a_raw? convert_raw_raw : convert_raw_750mV;
    a->a_range = USBDUXFAST_COMEDI_750mV;
    break;

  default:
    strbuf_appendf(e, "Comedi range spec %d unknown", range);
    return -1;
  }
  return 0;
}

/*
 * Set ADC to raw mode, i.e. don't range-map the incoming data.
 */

public void adc_set_raw_mode(adc a, int on) {
  a->a_raw = (on != 0);
  if(a->a_raw)
    a->a_convert = convert_raw_raw;
  else {
    if(a->a_range == USBDUXFAST_COMEDI_500mV)
      a->a_convert = convert_raw_500mV;
    if(a->a_range == USBDUXFAST_COMEDI_750mV)
      a->a_convert = convert_raw_750mV;
  }
}

/*
 * Initialise the ADC structure for data capture.
 */

public int adc_init(adc a, strbuf e) {
  int ret;
  int i;

  if( !a->a_path ) {
    strbuf_appendf(e, "Comedi device path not set");
    return -1;
  }
  
  /* Open the Comedi device */
  a->a_device = comedi_open(a->a_path);
  if(a->a_device == NULL) {
    strbuf_appendf(e, "Comedi device %s failure setting up ADC structure: %C", a->a_path);
    return -1;
  }
  a->a_fd = comedi_fileno(a->a_device);
  a->a_devflags = comedi_get_subdevice_flags(a->a_device, 0);

  /* Initialise Comedi streaming buffer */
  int request = a->a_req_bufsz_mib * 1024 * 1024;
  ret = comedi_get_buffer_size(a->a_device, 0);
  if( request > ret ) {
    ret = comedi_get_max_buffer_size(a->a_device, 0);  
    if( request > ret ) {
      ret = comedi_set_max_buffer_size(a->a_device, 0, request);
      if( ret < 0 ) {
	strbuf_appendf(e, "Comedi set max buffer to %dMiB failed: %C", a->a_req_bufsz_mib);
	return -1;
      }
    }
    ret = comedi_set_buffer_size(a->a_device, 0, request);
    if( ret < 0 ) {
      strbuf_appendf(e, "Comedi set streaming buffer to %dMiB failed: %C", a->a_req_bufsz_mib);
      return -1;
    }
  }

  a->a_bufsz_bytes = comedi_get_buffer_size(a->a_device, 0);
  a->a_bufsz_samples = a->a_bufsz_bytes / sizeof(sampl_t);
  comedi_set_global_oor_behavior(COMEDI_OOR_NUMBER);

  /* Initialise the command structure */
  ret = comedi_get_cmd_generic_timed(a->a_device, 0, &a->a_command, N_USBDUX_CHANS, 0);
  if(ret < 0) {
    strbuf_appendf(e, "Comedi generic command setup failed: %C");
    return -1;
  }

  /* Set the command parameters from the reader parameter values */
  for(i=0; i<N_USBDUX_CHANS; i++)
    a->a_chans[i] = CR_PACK_FLAGS(i, a->a_range, AREF_GROUND, 0);
  a->a_command.chanlist    = &a->a_chans[0];
  a->a_command.stop_src    = TRIG_NONE;
  a->a_command.stop_arg    = 0;
  a->a_command.convert_arg = a->a_intersample_ns;

  /* Ask the driver to check the command structure and complete any omissions */
  (void) comedi_command_test(a->a_device, &a->a_command);
  ret = comedi_command_test(a->a_device, &a->a_command);
  if( ret < 0 ) {
    strbuf_appendf(e, "Comedi second command test fails: %C");
    return -1;
  }

  /* Check the timing:  a difference here means a problem with the driver */
  if(a->a_command.convert_arg != a->a_intersample_ns) {
    a->a_intersample_ns = a->a_command.convert_arg;
    a->a_totfrequency = 1e9 / a->a_command.convert_arg;
    /* TODO: consider logging a warning here */
  }
  
  /* Map the Comedi buffer into memory, duplicated */
  void *map = mmap_and_lock(a->a_fd, 0, a->a_bufsz_bytes, PROT_RDONLY|PREFAULT_RDONLY|MAL_LOCKED|MAL_DOUBLED);
  if(map == NULL) {
    strbuf_appendf(e, "Unable to mmap Comedi streaming buffer: %m");
    return -1;
  }
  a->a_comedi_ring = map;

  /* Initialise the sample position indices */
  a->a_head = 0;
  a->a_tail = 0;
  a->a_start_time = 0;
  a->a_head_time  = 0;
  a->a_running = 0;
  return 0;
}

/*
 * Start the ADC data collection.
 */

public int adc_start_data_transfer(adc a, strbuf e) {
  int ret;

  /* Execute the command to initiate data acquisition */
  ret = comedi_command(a->a_device, &a->a_command);
  if(ret < 0) {
    strbuf_appendf(e, "Comedi command failed: %C");
  }
  else {
    a->a_running = 1;
  }
  return ret;
}

/*
 * Stop the ADC data collection.
 */

public void adc_stop_data_transfer(adc a) {
  if(a->a_running) {
    comedi_cancel(a->a_device, 0);
    a->a_running = 0;
    a->a_live = 0;
  }
}

/*
 * Convert a sample index into an ADC ring pointer.  This is used by
 * adc_setup_chunk().  It depends on the fact that the Comedi buffer
 * is double-mapped so the pointer is always the start of a contiguous
 * block of memory that will at some time hold the data for the chunk.
 */

private sampl_t *adc_sample_to_ring_ptr(adc a, uint64_t sample) {
  return &a->a_comedi_ring[sample % a->a_bufsz_samples];
}

/*
 * Set up the ADC-dependent information in a chunk, and determine whether the chunk is recordable.
 * In case of error, set the c_error strbuf and set the c_status code to SNAPSHOT_ERROR.
 */

public void adc_setup_chunk(adc a, chunk_t *c) {
  if(c->c_first < a->a_tail) {	/* Too late */
    strbuf_appendf(c->c_error, "Chunk was %d [us] too late", (int)((a->a_tail - c->c_first)/1000));
    c->c_ring = NULL;
    return;
  }
  c->c_ring = adc_sample_to_ring_ptr(a, c->c_first);  
  return;
}

/*
 * Convert times to sample indices and vice versa
 */

public uint64_t adc_time_to_sample(adc a, uint64_t time) {
  uint64_t ret;

  ret = (time - a->a_start_time) / a->a_intersample_ns;
  return ret;
}

public uint64_t adc_sample_to_time(adc a, uint64_t sample) {
  uint64_t ret;

  ret = a->a_start_time + sample*a->a_intersample_ns;
  return ret;
}

/*
 * Read-only access to some ADC parameters
 */

public int adc_ns_per_sample(adc a) {
  return a->a_intersample_ns;
}

public double adc_tot_frequency(adc a) {
  return a->a_totfrequency;
}

public uint64_t adc_capture_start_time(adc a) {
  return a->a_start_time;
}

public uint64_t adc_capture_head_time(adc a) {
  return a->a_head_time;
}

public int adc_is_running(adc a) {
  return a && a->a_running;
}

public int adc_is_live(adc a) {
  return a && a->a_live;
}

public uint64_t adc_ring_head(adc a) {
  return a->a_head;
}

public uint64_t adc_ring_tail(adc a) {
  return a->a_tail;
}

/*
 * The buffer strategy implied below is an explicit one of
 * periodically advancing the tail to avoid buffer overrun.  The data
 * bounded by the tail and head pointers in the ring buffer is valid,
 * under this explicit stragety.
 */

/*
 * Recognise data in the Comedi buffer: ask Comedi how much new data
 * is available, set the local data structure to match, tell Comedi we
 * have accepted the data.  If this is the first data received this
 * time, we compute the start time, i.e. the timestamp for sample
 * index 0, from the current head timestamp and the amount of data
 * obtained.
 */

public int adc_data_collect(adc a) {
  import uint64_t monotonic_ns_clock();
  uint64_t now;
  int      nb;
  int      ns;
  
  /* Retrieve any new data if possible */
  nb  = comedi_get_buffer_contents(a->a_device, 0);
  now = monotonic_ns_clock();
  if(nb) {
    ns  = nb / sizeof(sampl_t);
    a->a_head_time = now;
    a->a_head = a->a_tail + ns;	/* Assume that nb accumulates if mark read not called */
    if( !a->a_live ) {		/* Estimate the timestamp of sample index 0 */
      a->a_start_time = a->a_head_time - ns*a->a_intersample_ns;
      a->a_live++;
    }
  }
  return nb;
}

/*
 * Purge data from the tail of the ring buffer if explicit data
 * lifetime management is used.
 */

public int adc_data_purge(adc a, int ns) {
  int nb = ns*sizeof(sampl_t);
  int ret;

  ret = comedi_mark_buffer_read(a->a_device, 0, nb);
  if(ret != nb)
    return -1;
  a->a_tail += ns;
  return 0;
}
