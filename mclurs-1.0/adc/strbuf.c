#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "strbuf.h"
#include "queue.h"

#define N_STRBUF_ALLOC  8                       /* Allocate this many buffers at one go */
#define MAX_STRBUF_SIZE (512-sizeof(queue)-2*sizeof(int))       /* Strbuf will hold 496 characters maximum */

struct _strbuf {
  queue   s_Q;                          /* Queue header to avoid malloc() calls */
  int     s_used;                       /* Pointer to next free space in buffer */
  int     s_space;                      /* Size of the string space in the strbuf */
  char    s_buffer[MAX_STRBUF_SIZE];    /* Buffer space when in use */
};

/*
 * Return the usable string space in a strbuf.
 */

public int strbuf_space(strbuf s) {
  return s->s_space;
}

/*
 * Allocate and free strbufs, using a queue to avoid excessive malloc()
 */

private QUEUE_HEADER(sbufQ);
private int N_in_Q = 0;

public strbuf alloc_strbuf(int nr) {
  queue *ret;

  if( N_in_Q < nr ) {   /* The queue doesn't have enough */
    int n;

    for(n=0; n<N_STRBUF_ALLOC; n++) {
      queue *q = (queue *)calloc(1, sizeof(struct _strbuf));

      if( !q ) {                        /* Allocation failed */
        if( N_in_Q >= nr )
          break;                        /* But we have enough now anyway */
        return NULL;
      }
      init_queue(q);
      queue_ins_after(&sbufQ, q);
      qp2strbuf(q)->s_space = MAX_STRBUF_SIZE;
      N_in_Q++;
    }
  }
  ret = de_queue(queue_next(&sbufQ));
  while(--nr > 0) {             /* Collect enough to satisfy request */
    queue *p = de_queue(queue_next(&sbufQ));

    init_queue(p);
    qp2strbuf(p)->s_used = 0;
    qp2strbuf(p)->s_space = MAX_STRBUF_SIZE;
    qp2strbuf(p)->s_buffer[0] = '\0';
    queue_ins_before(ret, p);
  }
  return (strbuf)ret;
}

public void release_strbuf(strbuf s) {
  queue *p;

  while( (p = de_queue(queue_next(&s->s_Q))) != NULL ) {
    init_queue(p);
    queue_ins_before(&sbufQ, p);
    N_in_Q++;
  }
  queue_ins_before(&sbufQ, &s->s_Q);
  N_in_Q++;
}

/* Create a big one:  it's up to user to keep track of these.  They should not be released. */
public strbuf alloc_big_strbuf(int space) {
  strbuf s = (strbuf)calloc(1, space);
  if(s)
    s->s_space = space+MAX_STRBUF_SIZE-sizeof(struct _strbuf);
  return s;
}

/*
 * Get the string pointer from an strbuf (since the latter is opaque, we need a function for this).
 */

public char *strbuf_string(strbuf s) {
  return &s->s_buffer[0];
}

/*
 * Return the number of characters printed into a strbuf so far
 */

public int strbuf_used(strbuf s) {
  return s->s_used;
}

/*
 * Mark the current used position.
 */

public int strbuf_setpos(strbuf s, int pos) {
  if( !s  ) {
    errno = EINVAL;
    return -1;
  }
  if(pos < 0 || pos > s->s_space) {
    errno = ERANGE;
    return -1;
  }
  int used = s->s_used;
  s->s_used = pos;
  return used;
}

/*
 * Do a formatted print into an strbuf, starting at pos.
 */

private int strbuf_vprintf(strbuf s, int pos, const char *fmt, va_list ap) {
  int   rest;
  int   used;
  char *buf;

  if(pos < 0)                   /* Position one character back from end (i.e. skip NULL) or at start */
    pos = s->s_used ? s->s_used : 0;
  buf  = &s->s_buffer[pos];
  rest = s->s_space - pos;      /* There should be this much space remaining */
  if(rest < 0) {
    errno = EINVAL;
    return -1;
  }
  used = vsnprintf(buf, rest, fmt, ap);
  s->s_used = used>=rest? s->s_space : s->s_used + used;
  return used;
}

/*
 * Do fixup of the format: deal with all standard printf % options,
 * plus any extras.  When we see a pc_flag in one of the structures in
 * the list below, then we call the associated pc_func and interpolate
 * the string it returns.
 */

typedef struct _percent *percent;
struct _percent {
  percent       pc_link;
  char          pc_flag;        /* If we see this flag ... */
  const char *(*pc_func)();     /* ... then this function gives us the string */
};

private percent percent_list = NULL;

private percent find_in_list(char c) {
  percent p = percent_list;

  for( ; p; p=p->pc_link) {
    if(p->pc_flag == c)
      return p;
  }
  return NULL;
}

private void do_extra_percents(char *buf, int size, const char *fmt) {

  /* Copy the format into the buffer, checking each % modifier against the list */
  for( ; size > 1 && *fmt; size-- ) {
    if( (*buf++ = *fmt++) != '%' )
      continue;

    size--;                         /* Count the previous % that was copied */
    percent p = find_in_list(*buf++ = *fmt++);  /* Look up the next character */

    if(p == NULL)                   /* Not an extension, keep copying */
      continue;

    int used = snprintf(buf-2, size, "%s", (*p->pc_func)()); /* Interpolate at most 'size' chars */
    if(used >= size) {              /* Output was truncated */
      buf += size-1;
      break;
    }
    buf  += used-2;                 /* We copied 'used' characters, overwriting 2 */
    size -= used-3;                 /* The for loop will decerement one more...  */
  }
  *buf = '\0';
}

/* Start printing into the buffer at position pos */

public int strbuf_printf_pos(strbuf s, int pos, const char *fmt, ...) {
  va_list ap;
  int     used;
  char    fmt_buf[MAX_STRBUF_SIZE];

  if( strbuf_setpos(s, pos) < 0 )
    return -1;
  do_extra_percents(&fmt_buf[0], MAX_STRBUF_SIZE, fmt);
  va_start(ap, fmt);
  used = strbuf_vprintf(s, pos, fmt_buf, ap);
  va_end(ap);
  return used;
}

/* Start printing into the buffer at position 0 */

public int strbuf_printf(strbuf s, const char *fmt, ...) {
  va_list ap;
  int     used;
  char    fmt_buf[MAX_STRBUF_SIZE];

  if( strbuf_setpos(s, 0) < 0 )
    return -1;
  do_extra_percents(&fmt_buf[0], MAX_STRBUF_SIZE, fmt);
  va_start(ap, fmt);
  used = strbuf_vprintf(s, 0, fmt_buf, ap);
  va_end(ap);
  return used;
}

/* Start printing into the buffer at the current position */

public int strbuf_appendf(strbuf s, const char *fmt, ...) {
  va_list ap;
  int     used;
  char    fmt_buf[MAX_STRBUF_SIZE];

  do_extra_percents(&fmt_buf[0], MAX_STRBUF_SIZE, fmt);
  va_start(ap, fmt);
  used = strbuf_vprintf(s, -1, fmt_buf, ap);
  va_end(ap);
  return used;
}

/*
 * Register new percent interpreters.
 */

public int register_error_percent_handler(char c, const char *(*fn)()) {
  percent p = calloc(1, sizeof(struct _percent));

  if(p == NULL) {
    return -1;
  }

  p->pc_link = percent_list;
  p->pc_flag = c;
  p->pc_func = fn;
  percent_list = p;
  return 0;
}

/*
 * Revert a strbuf -- remove extra NUL characters inserted by tokenising
 */

public void strbuf_revert(strbuf s) {
  char *p = &s->s_buffer[0];
  int   n;

  for(n=0; n<s->s_used; n++,p++)
    if( !*p ) *p = ' ';
  n = (n == s->s_space)? n-1 : n;
  s->s_buffer[n] = '\0';
}

/*
 * Debug a strbuf
 */

public void debug_strbuf(FILE *fp, strbuf s) {
  char *str = strbuf_string(s);
  char  buf[MAX_STRBUF_SIZE+64];
  char *b;
  int   used;
  int   n;

  used = snprintf(&buf[0], 64, "s=%p, nxt=%p, prv=%p spc=%d: data[0..%d]='",
                  s, s->s_Q.q_next, s->s_Q.q_prev, s->s_space, s->s_used);
  b = &buf[used];
  n = MAX_STRBUF_SIZE+64-used-1;
  if( n > s->s_used )
    n = s->s_used;
  while(n-- > 0) if( (*b++ = *str++) == '\0' ) b[-1] = ' ';
  *b++ ='\'';
  *b = '\0';
  fwrite(&buf[0], 1, b-&buf[0], fp);
}
