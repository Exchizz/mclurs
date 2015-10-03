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
    if( (w&PREFAULT_RDONLY) )                   /* Read page */
      ret = *(int *)p;
    if( (w&PREFAULT_WRONLY) )                   /* Write page */
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
  int   pflags = 0;

  if( flags&PROT_RDONLY )
    pflags |= PROT_READ;
  if( flags&PROT_WRONLY )
    pflags |= PROT_WRITE;

  if( !pflags )
    pflags = PROT_NONE;

  mflags = MAP_SHARED;
  if(fixed)
    mflags |= MAP_FIXED;
  if(flags&MAL_LOCKED)
    mflags |= MAP_LOCKED;

  //  fprintf(stderr, "MMLF called map %p fd %d offs %d size %d[B] flags %x\n",
  //      fixed, fd, offset, length, flags);

  map = mmap(fixed, length, pflags, mflags, fd, offset);
  if(map == NULL || map == (void *)-1 || map != fixed)
    return NULL;

  //  fprintf(stderr, "MMLF succeeded for %d[B] at %p\n", length, map);

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
      munmap(map, 2*length);
      return NULL;
    }
  }

  return map;
}
