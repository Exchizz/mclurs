#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _ADC_H
#define _ADC_H

#include "general.h"

/*
 * Descriptor structure for Reader ADC interface.
 */

#define NCHANNELS       8       /* Public number of channels offered */

typedef struct _adc *adc;

export adc  adc_new(strbuf);
export void adc_destroy(adc);

export int  adc_set_chan_frequency(adc, strbuf, double *);
export int  adc_set_bufsz(adc, strbuf, int);
export int  adc_set_range(adc, strbuf, int);
export int  adc_set_device(adc, const char *);
export void adc_set_raw_mode(adc, int);
export void adc_set_ssc_coeff(adc, double);
export void adc_set_start_sync(adc, int);

export int  adc_init(adc, strbuf);
export int  adc_start_data_transfer(adc, strbuf);
export void adc_stop_data_transfer(adc);
export void adc_setup_chunk(adc, chunk_t *);

export uint64_t adc_time_to_sample(adc, uint64_t);
export uint64_t adc_sample_to_time(adc, uint64_t);

export int adc_ns_per_sample(adc);
export double adc_tot_frequency(adc);
export uint64_t adc_capture_start_time(adc);
export uint64_t adc_capture_head_time(adc);

export uint64_t adc_ring_head(adc);
export uint64_t adc_ring_tail(adc);

export int adc_data_collect(adc);
export int adc_data_purge(adc,int);

export int adc_is_running(adc);
public convertfn adc_convert_func(adc);

#endif /* _ADC_H */
