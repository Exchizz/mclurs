#

#define _GNU_SOURCE	/* Linux-specific code below (O_TMPFILE) */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <assert.h>

#include <comedi.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "mman.h"
#include "ring.h"

/*
 * Internal definitions
 */

#define	RING_TMPFILE	"/ring-XXXXXX"

/*
 * Create and mmap() the ring buffer pages
 */

struct readbuf *create_ring_buffer(int size, const char *tmpdir) {
  int fd;
  size_t sz;
  void *map;
  struct readbuf *ret = malloc(sizeof(struct readbuf));

  if( !ret )
    return ret;

  sz = size * sysconf(_SC_PAGESIZE);

  fd = open(tmpdir, O_TMPFILE|O_EXCL|O_RDWR); /* Try to get an anonymous file the easy, Linux-specific, way */
  if(fd < 0) {				      /* Nope, didn't work, use long way instead */
    char *file;

    file = malloc(strlen(tmpdir) + strlen(RING_TMPFILE)+1);
    if( !file ) {
      free(ret);
      return NULL;
    }

    strcpy(file, tmpdir);
    strcat(file, RING_TMPFILE);

    if((fd = mkstemp(file)) < 0) {
      free(file);
      free(ret);
      return NULL;
    }

    unlink(file);
    free(file);
  }

  if(ftruncate(fd, sz) < 0) {
    int e = errno;

    close(fd);
    free(ret);
    errno = e;
    return NULL;
  }
  /* Got an anonymous zero-filled file of the right size */

#ifdef THE_SLOW_HARD_WAY
  map = mmap(NULL, 2*sz, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if( !map ) {
    int e = errno;

    close(fd);
    free(ret);
    errno = e;
    return NULL;
  }
  /* Got an address for mapping that can cope with the needed size */

  ret->rb_start   = map;
  ret->rb_size    = sz;
  ret->rb_samples = sz / sizeof(sampl_t);

  map = mmap(ret->rb_start, sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
  if( !map || map != ret->rb_start ) {
    int e = errno;

    munmap(ret->rb_start, 2*sz);
    close(fd);
    free(ret);
    errno = e;
    return NULL;
  }
  ret->rb_end = ret->rb_start + sz;
  /* First mapping succeeded */

  map = mmap(ret->rb_end, sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
  if( !map || map != ret->rb_end ) {
    int e = errno;

    munmap(ret->rb_start, 2*sz);
    close(fd);
    free(ret);
    errno = e;
    return NULL;
  }
  /* Second mapping succeeded */

  if( mlock(ret->rb_start, 2*sz) < 0 ) {
    int e = errno;

    munmap(ret->rb_start, 2*sz);
    close(fd);
    free(ret);
    errno = e;
    return NULL;
  }
  /* Locked the pages */

  prefault_pages(ret->rb_start, 2*size, PREFAULT_WRONLY);
  /* Pages touched, faults generated */
#else
  map = mmap_and_lock_double(fd, 0, sz, PROT_READ|PROT_WRITE|MAL_LOCKED);
  if(map == NULL) {
    close(fd);
    free(ret);
    return NULL;
  }

  ret->rb_start   = map;
  ret->rb_end = ret->rb_start + sz;
  ret->rb_size    = sz;
  ret->rb_samples = sz / sizeof(sampl_t);

#endif

  close(fd);
  return ret;
}

/*
 * Tidy up a previously-created ring buffer
 */

int destroy_ring_buffer(struct readbuf *rb) {
  int ret;

  if( !rb || !rb->rb_start )
    return 0;

  ret = munmap(rb->rb_start, 2*rb->rb_size);
  free(rb);
  return ret;
}

