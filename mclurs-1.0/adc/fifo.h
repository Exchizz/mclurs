#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _FIFO_H
#define _FIFO_H

#include "general.h"

/*
 * Descriptor structure for memory FIFO.
 *
 * The FIFO comprises ff_slots of ff_stride bytes each, stored in a
 * memory-mapped block of RAM of size ff_bsize placed at ff_map .
 */

typedef struct {
  int	ff_slots;		/* Number of slots in the FIFO */
  int	ff_stride;		/* Size of a single slot */
  int	ff_bsize;		/* Number of bytes mmap'd by the FIFO */
  int   ff_insptr;		/* The next slot to write */
  int   ff_remptr;		/* The next slot to read */
  int   ff_insnum;		/* Number of items pending insertion */
  int	ff_remnum;		/* Number of items pending removal */
  int   ff_flags;		/* FIFO condition flags */
  void *ff_map;			/* Mapped data buffer */
} fifo;

#define FIFO_EMPTY	0x100	/* FIFO is empty */
#define FIFO_INSPENDING	0x200	/* FIFO is open for insertion */
#define FIFO_REMPENDING	0x400	/* FIFO is open for removal */

/*
 * Interface routines
 */

import fifo *fifo_create(int, int, int);    /* Create a new FIFO */
import void  fifo_destroy(fifo *);	    /* De-allocate a FIFO */

import int   fifo_available(fifo *);        /* Return how many slots can be read */
import int   fifo_space(fifo *);	    /* Return how many slots can be written */

import void *fifo_ins_open(fifo *f, int n); /* Open FIFO for insertion of n items */
import void  fifo_ins_close(fifo *f);	    /* Close FIFO after insertion */

import void *fifo_rem_open(fifo *f, int n); /* Open FIFO for removal of n items */
import void  fifo_rem_close(fifo *f);	    /* Close FIFO after removal */


#endif /* _FIFO_H */
