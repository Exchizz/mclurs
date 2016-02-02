#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _LUT_H
#define _LUT_H

#include "general.h"

export void populate_conversion_luts();
export void lut_set_ssc_coeff(double);
  
export void convert_raw_500mV(sampl_t *, sampl_t *, int, sampl_t);
export void convert_raw_750mV(sampl_t *, sampl_t *, int, sampl_t);
export void convert_raw_raw(sampl_t *, sampl_t *, int, sampl_t);

typedef void (*convertfn)(sampl_t *, sampl_t *, int, sampl_t);

#endif /* _LUT_H */
