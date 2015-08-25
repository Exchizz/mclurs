#

#ifndef _MMAN_H
#define _MMAN_H

/* Memory mapping and locking utilities */

extern void  prefault_pages(void *, int, int);
extern void *mmap_locate(size_t, int);
extern void *mmap_and_lock_fixed(int, off_t, size_t, int, void *);
extern void *mmap_and_lock(int, off_t, size_t, int);

#define PREFAULT_RDONLY	1
#define PREFAULT_WRONLY 2
#define PREFAULT_RDWR   (PREFAULT_RDONLY|PREFAULT_WRONLY)

#define MAL_LOCKED	8
#define MAL_DOUBLED	16

#endif /* _MMAN_H */

