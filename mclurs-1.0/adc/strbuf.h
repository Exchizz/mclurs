#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _STRBUF_H
#define _STRBUF_H

#include "general.h"

/*
 * Error buffer structure.
 */

typedef struct _strbuf *strbuf; /* Opaque object */

export strbuf alloc_strbuf();
export strbuf alloc_big_strbuf(int);
export void   release_strbuf(strbuf);
export char  *strbuf_string(strbuf);
export int    strbuf_space(strbuf);
export int    strbuf_used(strbuf);
export int    strbuf_setpos(strbuf,int);

#include <stdio.h>
#include <stdarg.h>

export int  strbuf_printf(strbuf, const char *, ...);
export int  strbuf_appendf(strbuf, const char *, ...);
export int  strbuf_printf_pos(strbuf, int, const char *, ...);
export int  register_error_percent_handler(char, const char *(*)());
export void strbuf_revert(strbuf);
export void debug_strbuf(FILE *, strbuf);

#define strbuf_clear(s) ((void) strbuf_setpos(s, 0))

#define strbuf_next(s)  ((strbuf)queue_next((queue *)(s)))
#define strbuf_prev(s)  ((strbuf)queue_prev((queue *)(s)))

#define strbuf2qp(s)    ((queue *)(s))
#define qp2strbuf(q)    ((strbuf)(q))

#endif /* _STRBUF_H */
