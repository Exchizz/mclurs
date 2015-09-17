#

#ifndef _CHUNK_H
#define _CHUNK_H

#include "general.h"
#include <comedilib.h>
#include "lut.h"

/* Structure for a memory block */

typedef struct {
  void *b_data;
  int   b_bytes;
}
  block;

typedef struct _frame frame;
typedef struct _sfile snapfile_t;

#include "queue.h"

typedef struct {
  queue	        c_Q[2];	    /* Q header for READER capture queue and WRITER file chunk list*/
#define c_wQ c_Q[0]	    /* Chunk Q linkage associated with the file */
#define c_rQ c_Q[1]	    /* Chunk Q linkage associated with the data flow */
  frame	       *c_frame;    /* Mmap'd file buffer for this chunk */
  strbuf        c_error;    /* Error buffer, for error messages (copy from snapshot_t origin) */
  snapfile_t   *c_parent;   /* Chunk belongs to this file */
  uint64_t      c_first;    /* First sample of this chunk */
  uint64_t      c_last;     /* First sample beyond this chunk */
  int16_t      *c_ring;	    /* Ring buffer start for this chunk */
  convertfn	c_convert;  /* Function to copy samles into frame with conversion */
  uint64_t	c_deadline; /* Time by which this chunk must have been written [ns past epoch] */
  uint32_t      c_samples;  /* Number of samples to copy */
  uint32_t      c_offset;   /* File offset for this chunk */
  int	        c_status;   /* Status of this capture chunk */
  int		c_fd;	    /* File descriptor for this chunk */
  uint16_t	c_name;	    /* Unique name for this chunk */
}
  chunk_t;

#define qp2chunk(q)	((chunk_t *)(q))
#define chunk2qp(c)	(&(c)->c_Q[0])

#define chunk2rq(c)	(&(c)->c_rQ)
#define rq2chunk(q)	((chunk_t *)&((q)[-1]))

export chunk_t *alloc_chunk(int);
export void release_chunk(chunk_t *);
export int map_chunk_to_frame(chunk_t *);

export int debug_chunk(char [], int, chunk_t *);

export void release_frame(frame *);

#endif /* _CHUNK_H */

