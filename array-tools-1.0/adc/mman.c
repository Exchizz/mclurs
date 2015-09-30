#

#include "general.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include "mman.h"

/*
 * Useful utility function to ensure pages are pre-faulted.
 */

public void prefault_pages(void *p, int n, int w) {
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
 * Locate a region of memory where one could map a file of size size.
 */

public void *mmap_locate(size_t length, int flags) {
  void *map;

  if( flags & MAL_DOUBLED ) length *= 2;

  map = mmap(NULL, length, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(map == NULL || map == (void *)-1)
    return NULL;

  return map;
}

/*
 * Map and lock a region of a file into memory at given fixed address.
 */

public void *mmap_and_lock_fixed(int fd, off_t offset, size_t length, int flags, void *fixed) {
  void *map;
  int   mflags = 0;

  if( flags&PROT_RDONLY )
    mflags |= PROT_READ;
  if( flags&PROT_WRONLY )
    mflags |= PROT_WRITE;

  if( !mflags )
    mflags = PROT_NONE;

  map = mmap(fixed, length, mflags, MAP_SHARED|MAP_FIXED, fd, offset);
  if(map == NULL || map == (void *)-1 || map != fixed)
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
 * Map and lock a region of a file into memory, don't care where...
 */

public void *mmap_and_lock(int fd, off_t offset, size_t length, int flags) {
  void *map;

  map = mmap_locate(length, flags);
  if( !map )
    return NULL;

  if( mmap_and_lock_fixed(fd, offset, length, flags, map) == NULL )
    return NULL;

  if( flags & MAL_DOUBLED ) {
    if( mmap_and_lock_fixed(fd, offset, length, flags, map+length) == NULL ) {
      munmap(map, length);
      return NULL;
    }
  }

  return map;
}
