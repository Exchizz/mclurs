#

#ifndef _ERROR_H
#define _ERROR_H

/*
 * Error buffer structure.
 */

typedef union _errbuf *errbuf;	/* Opaque object */

#define MAX_ERRBUF_SIZE 1024	/* Errbuf will hold 1024 characters maximum */

extern errbuf alloc_errbuf();
extern void   release_errbuf(errbuf, int);

#include <stdarg.h>

extern int   errbuf_printf(errbuf, const char *, ...);
extern int   errbuf_printf_pos(errbuf, int, const char *, ...);
extern int   register_error_percent_handler(char, const char (*)());
extern char *errbuf_string(errbuf);

#endif /* _ERROR_H */
