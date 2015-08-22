#

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "strbuf.h"
#include "queue.h"

#define N_STRBUF_ALLOC	8			/* Allocate this many buffers at one go */
#define MAX_STRBUF_SIZE (512-sizeof(queue))	/* Strbuf will hold 496 characters maximum */

struct _strbuf {
  queue   s_Q;				/* Queue header to avoid malloc() calls */
  int	  s_used;			/* Pointer to next free space in buffer */
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
static int N_in_Q = 0;

strbuf alloc_strbuf(int nr) {
  queue *ret;

  if( N_in_Q < nr ) {	/* The queue doesn't have enough */
    int n;

    for(n=0; n<N_STRBUF_ALLOC; n++) {
      queue *q = (queue *)calloc(1, sizeof(struct _strbuf));

      if( !q ) {			/* Allocation failed */
	if( N_in_Q >= nr )
	  break;			/* But we have enough now anyway */
	return NULL;
      }
      init_queue(q);
      queue_ins_after(&sbufQ, q);
      N_in_Q++;
    }
  }
  ret = de_queue(queue_next(&sbufQ));
  while(--nr > 0) {		/* Collect enough to satisfy request */
    queue *p = de_queue(queue_next(&sbufQ));

    init_queue(p);
    ((strbuf)p)->s_used = 0;
    ((strbuf)p)->s_buffer[0] = '\0';
    queue_ins_before(ret, p);
  }
  return (strbuf)ret;
}

static void free_strbuf(strbuf s) {
  free( (void *)s );
}

void release_strbuf(strbuf s) {
  queue *p;

  while( (p = de_queue(queue_next(&s->s_Q))) != NULL ) {
    init_queue(p);
    queue_ins_before(&sbufQ, p);
    N_in_Q++;
  }
  queue_ins_before(&sbufQ, &s->s_Q);
  N_in_Q++;
}

/*
 * Get the string pointer from an strbuf (since the latter is opaque, we need a function for this).
 */

char *strbuf_string(strbuf s) {
  return &s->s_buffer[0];
}

/*
 * Return the number of characters printed into a strbuf so far
 */

int strbuf_used(strbuf s) {
  return s->s_used;
}

/*
 * Do a formatted print into an strbuf, starting at pos.
 */

static int strbuf_vprintf(strbuf s, int pos, const char *fmt, va_list ap) {
  int   rest;
  int   used;
  char *buf  = &s->s_buffer[pos];

  if(pos < 0)			/* Position one character back from end (i.e. skip NULL) or at start */
    pos = s->s_used ? s->s_used-1 : 0;
  rest = MAX_STRBUF_SIZE - pos;	/* There should be this much space remaining */
  if(rest < 0) {
    errno = EINVAL;
    return -1;
  }
  used = vsnprintf(buf, rest, fmt, ap);
  s->s_used = used==rest? MAX_STRBUF_SIZE : s->s_used + used;
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
    if(used >= size) {		    /* Output was truncated */
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

int strbuf_appendf(strbuf s, const char *fmt, ...) {
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

/*
 * Debug a strbuf
 */

void debug_strbuf(FILE *fp, strbuf s) {
  char *str = strbuf_string(s);
  char  buf[MAX_STRBUF_SIZE+64];
  int   used;

  used = snprintf(&buf[0], 64, "s=%p, n=%p, q=%p: data='");
  snprintf(&buf[used], sizeof(buf)-used, "%s'\n", str);
  fprintf(fp, "%s", &buf[0]);
}
