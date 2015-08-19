#

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "strbuf.h"
#include "queue.h"

#define N_STRBUF_ALLOC	8			/* Allocate this many buffers at one go */
#define MAX_STRBUF_SIZE (256-sizeof(queue))	/* Strbuf will hold 240 characters maximum */

struct _strbuf {
  queue   s_Q;				/* Queue header to avoid malloc() calls */
  char    s_buffer[MAX_STRBUF_SIZE];	/* Buffer space when in use */
};

/*
 * Return the usable string space in a strbuf.
 */

int strbuf_space(strbuf s) {
  return MAX_STRBUF_SIZE;
}

/*
 * Allocate and free strbufs, using a queue to avoid excessive malloc()
 */

static QUEUE_HEADER(sbufQ);

strbuf alloc_strbuf(int nr) {
  if( queue_singleton(&sbufQ) ) {	/* The queue is empty */
    int n;

    for(n=0; n<N_STRBUF_ALLOC; n++) {
      queue *q = (queue *)calloc(1, sizeof(struct _strbuf));

      if( !q ) {			/* Allocation failed */
	if( !queue_singleton(&sbufQ) )
	  break;			/* But we have some anyway */
	return NULL;
      }
      init_queue(q);
      queue_ins_after(&sbufQ, q);
    }
  }
  return (strbuf)de_queue(queue_next(&sbufQ));
}

static void free_strbuf(strbuf s) {
  free( (void *)s );
}

void release_strbuf(strbuf s, int drop) {
  if(drop) {
    free_strbuf(s);
    return;
  }
  init_queue(&s->s_Q);
  queue_ins_before(&sbufQ, &s->s_Q);
}

/*
 * Get the string pointer from an strbuf (since the latter is opaque, we need a function for this).
 */

char *strbuf_string(strbuf s) {
  return &s->s_buffer[0];
}

/*
 * Do a formatted print into an strbuf, starting at pos.
 */

static int strbuf_vprintf(strbuf s, int pos, const char *fmt, va_list ap) {
  int   rest = MAX_STRBUF_SIZE - pos;
  char *buf  = &s->s_buffer[pos];

  if(rest < 0) {
    errno = EINVAL;
    return -1;
  }
  return vsnprintf(buf, rest, fmt, ap);
}

/*
 * Do fixup of the format: deal with all standard printf % options,
 * plus any extras.  When we see a pc_flag in one of the structures in
 * the list below, then we call the associated pc_func and interpolate
 * the string it returns.
 */

typedef struct _percent *percent;
struct _percent {
  percent      pc_link;
  char         pc_flag;		/* If we see this flag */
  const char (*pc_func)();	/* then this function gives us the string */
};

static percent percent_list = NULL;

static percent find_in_list(char c) {
  percent p = percent_list;

  for( ; p; p=p->pc_link) {
    if(p->pc_flag == c)
      return p;
  }
  return NULL;
}

static void do_extra_percents(char *buf, int size, const char *fmt) {

  /* Copy the format into the buffer, checking each % modifier against the list */
  for( ; size > 1 && *fmt; size-- ) {
    if( (*buf++ = *fmt++) != '%' )
      continue;

    size--;			    /* Count the previous % that was copied */
    percent p = find_in_list(*buf++ = *fmt++);  /* Look up the next character */

    if(p == NULL)		    /* Not an extension, keep copying */
      continue;

    int used = snprintf(buf-2, size, "%s", (*p->pc_func)()); /* Interpolate at most 'size' chars */
    if(used > size) {		    /* Output was truncated */
      buf += size-1;
      break;
    }
    buf  += used-2;		    /* We copied 'used' characters, overwriting 2 */
    size -= used-3;		    /* The for loop will decerement one more...  */
  }
  *buf = '\0';
}

int strbuf_printf_pos(strbuf s, int pos, const char *fmt, ...) {
  va_list ap;
  int     used;
  char    fmt_buf[MAX_STRBUF_SIZE];

  do_extra_percents(&fmt_buf[0], MAX_STRBUF_SIZE, fmt);
  va_start(ap, fmt);
  used = strbuf_vprintf(s, pos, fmt_buf, ap);
  va_end(ap);
  return used;
}

int strbuf_printf(strbuf s, const char *fmt, ...) {
  va_list ap;
  int     used;
  char    fmt_buf[MAX_STRBUF_SIZE];

  do_extra_percents(&fmt_buf[0], MAX_STRBUF_SIZE, fmt);
  va_start(ap, fmt);
  used = strbuf_vprintf(s, 0, fmt_buf, ap);
  va_end(ap);
  return used;
}

/*
 * Register new percent interpreters.
 */

int register_error_percent_handler(char c, const char (*fn)()) {
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
