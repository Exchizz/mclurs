#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _MMAN_H
#define _MMAN_H

#include "general.h"
#include <sys/mman.h>

/* Memory mapping and locking utilities */

export void  prefault_pages(void *, int, int);
export void *mmap_locate(size_t, int);
export void *mmap_and_lock_fixed(int, off_t, size_t, int, void *);
export void *mmap_and_lock(int, off_t, size_t, int);

#define PROT_RDONLY     1
#define PROT_WRONLY     2
#define PROT_RDWR       (PROT_RDONLY|PROT_WRONLY)

#define PREFAULT_RDONLY 4
#define PREFAULT_WRONLY 8
#define PREFAULT_RDWR   (PREFAULT_RDONLY|PREFAULT_WRONLY)

#define MAL_LOCKED      16
#define MAL_DOUBLED     32

#endif /* _MMAN_H */

