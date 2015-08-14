#

#ifndef _LOCAL_ASSERT_H
#define _LOCAL_ASSERT_H

/*
 * Local version of assert, bit more informative than system version
 */

#ifdef USE_SYSTEM_ASSERT

#include <assert.h>

#define assertv(cond, ...) assert(cond)

#else

#include <stdio.h>
#include <stdlib.h>

#define assertv(cond,fmt, ...) do {			\
  if(!(cond)) { \
    fprintf(stderr, "FAILED ASSERTION -- %s:%d %s %s\n" fmt, __FILE__, __LINE__, __FUNCTION__, "'" #cond "'" , ## __VA_ARGS__ ); \
    abort(); \
  } } while(0)

#endif

#endif /* _LOCAL_ASSERT_H */
