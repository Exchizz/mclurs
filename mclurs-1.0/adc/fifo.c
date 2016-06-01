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
#include <unistd.h>
#include <errno.h>

#include "general.h"

#include "assert.h"
#include "mman.h"

#include "fifo.h"

/*
 * Create a new FIFO
 *
 * A FIFO is an array of slots, each slot of width stride, stored in a
 * doubly mapped set of memory pages.  (This means that the space to
 * be mapped (bsize) must be suitable for the mmap call.)  The double
 * mapping makes it easy to do block insert and removal since one can
 * always obtain a pointer to a contiguous block of memory from which
 * to read or to which to write the data.
 */

public fifo *fifo_create(int slots, int stride, int flags) {
  fifo *f;
  int   bsize = slots*stride;
  
  /* Argument sanity checks */
  if(slots <= 0  || stride <= 0) {
    errno = EINVAL;
    return NULL;
  }

  /* Check suitability for contiguous double mapping */
  if( (bsize % sysconf(_SC_PAGESIZE)) != 0 ) {
    errno = EBADSLT;
    return NULL;
  }
  
  /* Allocate and initialise */
  f = (fifo *)calloc(1, sizeof(fifo));
  if(f) {
    f->ff_slots  = slots;	/* Copy in descriptor contents */
    f->ff_stride = stride;
    f->ff_bsize  = bsize*2;	/* Doubled contiguous mmap */
    f->ff_flags  = FIFO_EMPTY;	/* New FIFO is empty */

    /* Allocate the ff_map data area */
    f->ff_map = mmap_and_lock(-1, 0, bsize, PROT_RDWR|PREFAULT_RDWR|MAL_LOCKED|MAL_DOUBLED);
    if( !f->ff_map ) {		/* Failed to get data space */
      free( (void *)f );
      f = NULL;
    }
  }
  return f;
}

/*
 * Clean up and destroy a FIFO
 */

public void fifo_destroy(fifo *f) {
  if(f) {
    munmap(f->ff_map, f->ff_bsize);
    free( (void *)f );
  }
  return;
}

/*
 * Number of readable data in a FIFO
 */

public int fifo_available(fifo *f) {
  return (f->ff_flags&FIFO_EMPTY) ? 0 : (f->ff_insptr + f->ff_slots - f->ff_remptr)%f->ff_slots;
}

/*
 * Number of writable slots remaining in the FIFO
 */

public int fifo_space(fifo *f) {
  return f->ff_slots - fifo_available(f);
}

/*
 * Open the FIFO for inserting n data
 */

public void *fifo_ins_open(fifo *f, int n) {
  if( n <= 0 ) {
    errno = EINVAL;
    return NULL;
  }
  if(f->ff_flags & FIFO_INSPENDING) { /* Insertions don't nest */
    errno = EBUSY;
    return NULL;
  }
  if( n > fifo_space(f) ) { /* There is insufficient space for the data */
    errno = ENOSPC;
    return NULL;
  }
  f->ff_insnum = n;
  f->ff_flags |= FIFO_INSPENDING;
  /* Obtain a pointer to the position the n items will occupy */
  return f->ff_map + f->ff_insptr*f->ff_stride;
}

/*
 * Close the FIFO for insertion
 *
 * The insertion pointer has advanced n (== ff_insnum) slots.
 *
 * The FIFO cannot be empty now.
 */

public void fifo_ins_close(fifo *f) {
  f->ff_insptr = (f->ff_insptr + f->ff_insnum) % f->ff_slots;
  f->ff_insnum = 0;
  f->ff_flags &= ~(FIFO_INSPENDING|FIFO_EMPTY);
  return;
}

/*
 * Open the FIFO for insertion and force space
 */

public void *fifo_ins_forced_open(fifo *f, int n) {
  if( n <= 0 ) {
    errno = EINVAL;
    return NULL;
  }
  if(f->ff_flags & FIFO_INSPENDING) { /* Insertions don't nest */
    errno = EBUSY;
    return NULL;
  }

  int s = fifo_space(f);
  if( n > s ) {		/* There is insufficient space for the data -- remove some items */
    if( fifo_rem_open(f, n-s) == NULL ) { /* Can't remove any -- busy or no space */
      return NULL;
    }
    fifo_rem_close(f);			  /* Else remove the reserved items */
  }
  f->ff_insnum = n;
  f->ff_flags |= FIFO_INSPENDING;
  /* Obtain a pointer to the position the n items will occupy */
  return f->ff_map + f->ff_insptr*f->ff_stride;
}
 
/*
 * Open the FIFO for removing n data
 */

public void *fifo_rem_open(fifo *f, int n) {
  if( n >= 0 ) {
    errno = EINVAL;
    return NULL;
  }
  if(f->ff_flags & FIFO_REMPENDING) { /* Removals don't nest */
    errno = EBUSY;
    return NULL;
  }
  if( n > fifo_available(f) ) { /* There is not enough data to take */
    errno = ENOSPC;
    return NULL;
  }
  f->ff_remnum = n;
  f->ff_flags |= FIFO_REMPENDING;
  /* Obtain a pointer to the position the n items occupy now */
  return f->ff_map + f->ff_remptr*f->ff_stride;
}

/*
 * Close the FIFO for removal
 *
 * The remval pointer has advanced n (== ff_remnum) slots.
 *
 * The FIFO is empty at this point if the insertion and removal
 * pointers are coincident.
 */

public void fifo_rem_close(fifo *f) {
  f->ff_remptr = (f->ff_remptr + f->ff_remnum) % f->ff_slots;
  f->ff_remnum = 0;
  f->ff_flags &= ~FIFO_REMPENDING;
  if(f->ff_insptr == f->ff_remptr)
    f->ff_flags |= FIFO_EMPTY;
  return;
}

