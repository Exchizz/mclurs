#

#ifndef _UTIL_H
#define _UTIL_H

#include <stdarg.h>
#include <unistd.h>
#include "assert.h"

#define true	1
#define false	0

#define WAIT_FOR_CONDITION(cond,limit)					\
  do { double l = (limit); int n = 0, max = 100*l;			\
    while( n<max && !(cond) ) usleep(10000), n++;			\
    assertv((cond), "Waited too long (%g [s]) for condition\n", l);	\
  } while(0)

/* Messaging utilities */

#include <zmq.h>

extern int zh_get_msg(void *, int, size_t, void *);
extern int zh_any_more(void *);

extern int zh_put_msg(void *, int, size_t, void *);
extern int zh_put_multi(void *, int, ...);

extern void *zh_bind_new_socket(void *, int, const char *);
extern void *zh_connect_new_socket(void *, int, const char *);

#endif /* _UTIL_H */
