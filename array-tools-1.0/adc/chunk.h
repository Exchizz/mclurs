#

#ifndef _CHUNK_H
#define _CHUNK_H

/* Structure for a memory block */

typedef struct {
  void *b_data;
  int   b_bytes;
}
  block;

typedef struct _frame frame;

#include "queue.h"

typedef struct {
  queue	        c_Q[2];	    /* Q header for READER capture queue and WRITER file chunk list*/
#define c_wQ c_Q[0]
#define c_rQ c_Q[1]
  uint32_t      c_samples;  /* The number of samples to copy */
  int16_t      *c_ring;	    /* The ring buffer start for this chunk */
  frame	       *c_frame;    /* The mmap'd file buffer for this chunk */
  int	        c_status;   /* The status of this capture chunk */
  strbuf        c_error;    /* The error buffer, for error messages (copy from snapshot_t origin) */
  uint64_t      c_first;    /* First and last samples of this chunk */
  uint64_t      c_last;
  snapfile_t   *c_parent;   /* The chunk belongs to this file */
}
  chunk_t;

#define q2chunk(q)	((chunk_t *)(q))
#define chunk2q(c)	(&(c)->c_Q[0])

#define chunk2rq(c)	(&(c)->c_rQ)
#define rq2chunk(q)	((chunk_t *)(q[-1]))

extern chunk_t *alloc_chunk(int);
extern void release_chunk(chunk_t *);
extern void setup_chunks(chunk_t *, snapfile_t *);
extern void completed_chunk(chunk_t *);
extern void abort_chunk(chunk_t *);

extern void release_frame(frame *);

#endif /* _CHUNK_H */
