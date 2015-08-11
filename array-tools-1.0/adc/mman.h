#

/* Memory mapping and locking utilities */

extern void  prefault_pages(void *, int, int);
extern void *mmap_and_lock();
extern void *mmap_and_lock_double();

#define PREFAULT_RDONLY	1
#define PREFAULT_WRONLY 2
#define PREFAULT_RDWR   (PREFAULT_RDONLY|PREFAULT_WRONLY)

#define MAL_LOCKED	8

