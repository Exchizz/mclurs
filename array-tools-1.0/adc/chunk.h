#

#ifndef _CHUNK_H
#define _CHUNK_H

/* Structure for a memory block */

typedef struct {
  void *b_data;
  int   b_bytes;
}
  block;

#include "queue.h"

typedef struct {
  queue	      rc_Q;	   /* Q header for READER capture queue */
  uint32_t    rc_samples;  /* The number of samples to copy */
  sampl_t    *rc_ring;	   /* The ring buffer start for this chunk */
  block	      rc_memblk;   /* The mmap'd file buffer for this chunk */
  int	      rc_status;   /* The status of this capture chunk */
  strbuf      rc_error;	   /* The error buffer, for error messages (copy from snapshot_t origin) */
}
  rchunk_t;

typedef struct {
  queue	      wc_Q;		/* Queue header for WRITER's file chunk list */
  snapfile_t *wc_parent;	/* The chunk belongs to this file */
  uint64_t    wc_first;		/* First and last samples of this chunk */
  uint64_t    wc_last;
}
  wchunk_t;

typedef struct {
  rchunk_t    ch_reader;	/* The information needed by the reader */
  wchunk_t    ch_writer;	/* The information needed by the writer */
}
  chunk_t;

extern chunk_t *alloc_chunk();
extern void free_chunk(chunk_t *);
extern void setup_chunks(chunk_t *, snapfile_t *);
extern void completed_chunk(chunk_t *);
extern void abort_chunk(chunk_t *);

#endif /* _CHUNK_H */
