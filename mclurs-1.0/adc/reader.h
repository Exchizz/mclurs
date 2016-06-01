#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

/*
 * The ZMQ addresses for the reader thread
 */

#define READER_CMD_ADDR "inproc://Reader-CMD"
#define READER_QUEUE_ADDR "inproc://Reader-Q"

/*
 * READER parameter structure.
 *
 * The order of r_range and r_bufsz seems to alter whether the
 * parameters are correctly initialised or not by the param code...
 */

typedef struct {
  int         r_schedprio;         /* Reader real-time priority */
  double      r_frequency;         /* Per-channel sampling frequency [Hz] */
  int         r_range;             /* ADC full-scale range [mV] */
  int         r_bufsz;             /* Reader buffer size [MiB] */
  double      r_window;            /* Snapshot window [s] (must fit in buffer) */
  double      r_buf_hwm_fraction;  /* Ring buffer high-water mark as fraction of size */
  double      r_sync_wait_time;	   /* Time to wait for first data after capture start */
  double      r_sscorrelation;	   /* Correlation coefficient between successive (channel) samples */
  const char *r_device;            /* Comedi device to use */
  int         r_running;           /* Thread is running and ready */
}
  rparams;

export rparams   reader_parameters;

export int       verify_reader_params(rparams *, strbuf);
export void     *reader_main(void *);

