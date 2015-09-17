#

#ifndef _ADC_H
#define _ADC_H

#include "general.h"

/*
 * Descriptor structure for Reader ADC interface.
 */

#define NCHANNELS	8	/* Public number of channels offered */

typedef struct _adc *adc;

export adc  adc_new(const char *, strbuf);
export void adc_destroy(adc);
export int  adc_set_frequency(adc, strbuf, double *);
export int  adc_set_bufsz(adc, strbuf, int);
export int  adc_set_range(adc, strbuf, int);
export int  adc_init(adc, strbuf);
export int  adc_start_data_transfer(adc, strbuf);
export void adc_stop_data_transfer(adc);
export void adc_setup_chunk(adc, chunk_t *);

export uint64_t adc_time_to_sample(adc, uint64_t);
export uint64_t adc_sample_to_time(adc, uint64_t);

export int adc_ns_per_sample(adc);
export double adc_tot_frequency(adc);
export uint64_t adc_capture_start_time(adc);

#endif /* _ADC_H */
