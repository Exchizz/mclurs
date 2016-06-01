#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _ENERGY_H
#define _ENERGY_H

#include <stdlib.h>
#include <stdint.h>

#include "general.h"
#include "fifo.h"
#include "adc.h"

/*
 * Energy calculation sample record definition
 */

typedef struct {
  uint64_t	ec_timestamp;
  uint16_t	ec_overflow[NCHANNELS];
  long		ec_energy[NCHANNELS];
} energy_sample;

/*
 * Descriptor structure for energy calculation state.
 */

typedef struct {
  uint64_t 	  e_current;
  uint64_t	  e_next;
  uint32_t	  e_period;
  uint32_t	  e_clock;
  uint32_t	  e_rate;
  fifo           *e_fifo;
  energy_sample  *e_sample;
} energy_calc;

/*
 * Module interface routines
 */

import void energy_calc_init(energy_calc *, unsigned, unsigned);
import void energy_calc_update(energy_calc *, adc);
import void energy_calc_finish(energy_calc *);

#endif /* _ENERGY_H */
