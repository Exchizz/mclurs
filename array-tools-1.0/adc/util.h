#

#ifndef _UTIL_H
#define _UTIL_H

#include <zmq.h>
#include <stdarg.h>

#define true	1
#define false	0

extern int zh_get_msg(void *, int, size_t, void *);
extern int zh_any_more(void *);

extern int zh_put_msg(void *, int, size_t, void *);
extern int zh_put_multi(void *, int, ...);

#endif /* _UTIL_H */
