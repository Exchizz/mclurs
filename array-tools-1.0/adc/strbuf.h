#

#ifndef _STRBUF_H
#define _STRBUF_H

/*
 * Error buffer structure.
 */

typedef struct _strbuf *strbuf;	/* Opaque object */

extern strbuf alloc_strbuf();
extern void   release_strbuf(strbuf);
extern char  *strbuf_string(strbuf);
extern int    strbuf_space(strbuf);
extern int    strbuf_used(strbuf);
extern int    strbuf_setpos(strbuf,int);

#include <stdio.h>
#include <stdarg.h>

extern int  strbuf_printf(strbuf, const char *, ...);
extern int  strbuf_appendf(strbuf, const char *, ...);
extern int  strbuf_printf_pos(strbuf, int, const char *, ...);
extern int  register_error_percent_handler(char, const char (*)());
extern void strbuf_revert(strbuf);
extern void debug_strbuf(FILE *, strbuf);

#define strbuf_clear(s)	((void) strbuf_setpos(s, 0))

#define strbuf_next(s)	((strbuf)queue_next((queue *)(s)))
#define strbuf_prev(s)	((strbuf)queue_prev((queue *)(s)))

#endif /* _STRBUF_H */
