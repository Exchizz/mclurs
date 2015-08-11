#

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include "mman.h"

/*
 * Useful utility function to ensure pages are pre-faulted.
 */

void prefault_pages(void *p, int n, int w) {
  int ret = 0;

  while( n-- > 0 ) {
    if( (w&PREFAULT_RDONLY) )			/* Read page */
      ret = *(int *)p;
    if( (w&PREFAULT_WRONLY) )			/* Write page */
      *(int *)p = ret;
    p += sysconf(_SC_PAGESIZE);
  }
}

/*
 * Map and lock a region of a file into memory, don't care where...
 */

void *mmap_and_lock(int fd, off_t offset, size_t length, int flags) {
  void *map;

  map = mmap(map, length, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
  if(map == NULL || map == (void *)-1)
    return NULL;

  if( (flags&MAL_LOCKED) && mlock(map, length) < 0 ) {
    munmap(map, length);
    return NULL;
  }

  if( flags & PREFAULT_RDWR )
    prefault_pages(map, length / sysconf(_SC_PAGESIZE), (flags & PREFAULT_RDWR));

  return map;
}

/*
 * Map and lock a region of a file into memory twice contiguously, don't care where...
 */

void *mmap_and_lock_double(int fd, off_t offset, size_t length, int flags) {
  void *map;
  void *ms;

  map = mmap(NULL, 2*length, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(map == NULL || map == (void *)-1)
    return NULL;

  ms = mmap(map, length, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
  if(ms != map)
    return NULL;

  ms = mmap(map+length, length, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
  if(ms != (map+length)) {
    munmap(map, length);
    return NULL;
  }

  if( (flags&MAL_LOCKED) && mlock(map, 2*length) < 0 ) {
    munmap(map, 2*length);
    return NULL;
  }

  if( flags & PREFAULT_RDWR )
    prefault_pages(map, 2*length / sysconf(_SC_PAGESIZE), (flags & PREFAULT_RDWR));

  return map;
}
