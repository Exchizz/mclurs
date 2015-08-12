#

#ifndef _LUT_H
#define _LUT_H

extern void populate_conversion_luts();

extern void convert_raw_500mV(sampl_t *, sampl_t *, int);
extern void convert_raw_750mV(sampl_t *, sampl_t *, int);
extern void convert_raw_raw(sampl_t *, sampl_t *, int);

#endif /* _LUT_H */
