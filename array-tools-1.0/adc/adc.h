#

/*
 * Descriptor structure for Reader ADC interface.
 */

#define N_USBDUX_CHANS	16

#define ERRBUFSZ 128

typedef struct _adc
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
}
  adc;

#define USBDUXFAST_COMEDI_500mV	1 /* Bit 3 control output is 0 iff the CR_RANGE is one */
#define USBDUXFAST_COMEDI_750mV	0 /* Bit 3 control output is 1 iff the CR_RANGE is zero */

adc *adc_new(char *);
int adc_destroy(adc *);
int adc_init(adc *, int, int, int);
int adc_set_ringbuf(adc *, struct readbuf *);
int adc_start(adc *);
int adc_stop(adc *);
int adc_fetch(adc *, int, struct timespec *);
