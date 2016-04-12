#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "general.h"

#include "assert.h"
#include "strbuf.h"
#include "chunk.h"
#include "adc.h"
#include "lut.h"
#include "fifo.h"
#include "energy.h"

/*
 * Update the energy_calc structure to be ready to compute the next sample and store
 * it into the output FIFO.
 */

private void energy_calc_prepare_sample(energy_calc *e) {
  e->e_current  = e->e_next;
  e->e_clock   += e->e_period;
  e->e_next     = e->e_current + NCHANNELS*(e->e_clock / e->e_rate);
  e->e_clock   %= e->e_rate;
  e->e_sample   = (energy_sample *) fifo_ins_open(e->e_fifo, 1);

  bzero((void *)e->e_sample, sizeof(energy_sample));
  e->e_sample->ec_timestamp = e->e_next;
}

/*
 * Initialise an energy_calc structure given the sub-sampling period
 * and the sample rate for NCHANNELS of data, both in ticks of the
 * USBDUXfast clock (30MHz).
 */

public void energy_calc_init(energy_calc *e, unsigned period, unsigned rate) {
  e->e_next   = 0;
  e->e_clock  = 0;
  e->e_period = period;
  e->e_rate   = rate;
  energy_calc_prepare_sample(e);
}

public void energy_calc_update(energy_calc *e, adc adc) {
  while(adc_ring_head(adc) > e->e_next) {
    /*
     * Process one sub-sample, i.e. from e->e_current until e->e_next:
     *
     * 1. Compute total energy per channel and store in ec_energy
     * 2. Compute logical-or of samples per channel and store in ec_overflow
     */

    fifo_ins_close(e->e_fifo);		/* Push the sample to the FIFO */
    energy_calc_prepare_sample(e);	/* Get ready for the next one */
  }
}

public void energy_calc_finish(energy_calc *e) {
  /* Not implemented for now */
}
