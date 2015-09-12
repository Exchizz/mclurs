#

#include "general.h"

/*
 * The ZMQ addresses for the reader thread
 */

#define READER_CMD_ADDR	"inproc://Reader-CMD"
#define READER_QUEUE_ADDR "inproc://Reader-Q"

/*
 * Reader parameter structure.
 */

typedef struct {
  double      r_frequency;	/* Per-channel sampling frequency [Hz] */
  int         r_schedprio;	/* Reader real-time priority */
  int         r_bufsz;		/* Reader buffer size [MiB] */
  int	      r_range;		/* ADC full-scale range [mV] */
  double      r_window;		/* Snapshot window [s] (must fit in buffer) */
  const char *r_device;		/* Comedi device to use */

  /* These below are computed by the reader and exported */
  double      r_tot_frequency;	    /* [Hz] total sampling frequency */
  int	      r_inter_sample_ns;    /* [ns] for one sample */
  uint64_t    r_capture_start_time; /* [ns from epoch] */
  int	      r_running;	    /* Thread is running and ready */
  int	      r_state;		    /* Internal READER FSM state */
}
  rparams;

export rparams reader_parameters;
export int     verify_reader_params(rparams *, strbuf);
export void   *reader_main(void *);
