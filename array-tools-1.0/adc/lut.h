#

#ifndef _LUT_H
#define _LUT_H

#include "general.h"

export void populate_conversion_luts();

export void convert_raw_500mV(sampl_t *, sampl_t *, int);
export void convert_raw_750mV(sampl_t *, sampl_t *, int);
export void convert_raw_raw(sampl_t *, sampl_t *, int);

#endif /* _LUT_H */
