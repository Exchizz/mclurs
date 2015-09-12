#

#include "general.h"

/*
 * Descriptor structure for Reader ADC interface.
 */

typedef struct _adc *adc;

export adc adc_new(char *);
export int adc_destroy(adc);
export int adc_init(adc, int, int, int);
export int adc_set_ringbuf(adc, struct readbuf *);
export int adc_start(adc);
export int adc_stop(adc);
export int adc_fetch(adc, int, uint64_t *);

