#

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "queue.h"
#include "mman.h"
#include "strbuf.h"
#include "chunk.h"

struct _frame {
  queue f_Q;
  block f_map;
};

/*
 * Set up the mmap frames for data transfer to snapshot files.
 */

static int    nframes;	  /* The number of simultaneous mmap frames */
static frame *framelist;  /* The list of mmap frame descriptors */
static int    n_frame_Q = 0;

static QUEUE_HEADER(frameQ);

int init_frame_system(strbuf e, int nfr, int ram, int chunk) {

  framelist = (frame *)calloc(nfr, sizeof(frame));
  if( framelist ) {
    void *map = mmap_locate(ram*1024*1024, 0);
    int   n;

    if(map == NULL) {
      strbuf_appendf(e, "Cannot mmap %d MiB of locked transfer RAM: %m", ram);
      free((void *) framelist );
      return -1;
    }
    for(n=0; n<nfr; n++) { /* Initialise the frame memory pointers, leave sizes as 0 */
      framelist[n].f_map.b_data = map;
      map += chunk;
      init_queue(&framelist[n].f_Q);
      queue_ins_before(&frameQ, &framelist[n].f_Q);
      n_frame_Q++;
    }
  }
  else {
    strbuf_appendf(e, "Cannot allocate frame list memory for %d frames: %m", nfr);
    return -1;
  }
  nframes = nfr;
  return 0;
}

/*
 * Scan the frame list and pull any free frame descriptors into the free queue.
 *
 * A descriptor is free if its byte count is zero, and it is not in
 * the free queue if its queue structure is a singleton.
 */

static void scan_framelist() {
  int    n;
  frame *f;

  for(n=0,f=framelist; n<nframes; n++, f++) {
    if(f->f_map.b_bytes)	/* If non-zero, it's in use */
      continue;
    if( !queue_singleton(&f->f_Q) )
      continue;
    queue_ins_before(&frameQ, &f->f_Q);
    n_frame_Q++;
  }
}

/*
 * Allocate a frame descriptor.
 */

static frame *alloc_frame() {
  frame *f;

  if( !n_frame_Q ) {
    assertv(queue_singleton(&frameQ), "Frame queue count is zero for non-empty queue\n");
    scan_framelist();
  }
  if( !n_frame_Q ) {
    errno = EBUSY;
    return NULL;
  }
  f = (frame *)de_queue(queue_next(&frameQ));
  assertv(f != NULL, "Frame queue count %d but queue is empty\n", n_frame_Q);
  n_frame_Q--;
  f->f_map.b_bytes = 1;		/* In-use;  real size is filled in by caller */
  return f;
}

/*
 * Release a frame descriptor.  N.B.  this is done in the tidy thread, so must be atomic.
 */

void release_frame(frame *f) {
  f->f_map.b_bytes = 0;
}

/*
 * Functions for dealing with transfer chunk descriptors.
 */

#define N_CHUNK_ALLOC	(4096/sizeof(chunk_t))

static QUEUE_HEADER(chunkQ);
static int N_in_chunkQ = 0;

/*
 * Allocate n new chunk descriptors, chained using the writer queue descriptor
 */

chunk_t *alloc_chunk(int nr) {
  queue *ret;

  if( N_in_chunkQ < nr ) {	/* The queue doesn't have enough */
    int n;

    for(n=0; n<N_CHUNK_ALLOC; n++) {
      queue *q = (queue *)calloc(1, sizeof(chunk_t));

      if( !q ) {			/* Allocation failed */
	if( N_in_chunkQ >= nr )
	  break;			/* But we have enough now anyway */
	return NULL;
      }
      init_queue(q);
      queue_ins_after(&chunkQ, q);
      N_in_chunkQ++;
    }
  }
  ret = de_queue(queue_next(&chunkQ));
  while(--nr > 0) {		/* Collect enough to satisfy request */
    queue *p = de_queue(queue_next(&chunkQ));

    init_queue(p);
    queue_ins_before(ret, p);
  }

  chunk_t *c = q2chunk(ret);
  init_queue(&c->c_rQ);		/* Make sure the reader queue dscriptor is initialised */
  return c;
}

/*
 * Finished with chunk descriptors chained using the writer queue descriptor.
 * Assume the reader queue descriptor is detached.
 */

void release_chunk(chunk_t *c) {
  queue *q = chunk2q(c);
  queue *p;

  while( (p = de_queue(queue_next(q))) != NULL ) {
    init_queue(p);
    queue_ins_before(&chunkQ, p);
    N_in_chunkQ++;
  }
  init_queue(q);
  queue_ins_before(&chunkQ, q);
  N_in_chunkQ++;
}

/*
 * Initialise the data structures in a file's chunks
 */

void setup_chunks(chunk_t *c, snapfile_t *f) {
}

/*
 * Completed a chunk
 */

void completed_chunk(chunk_t *c) {
}

/*
 * Abort a chunk
 */

void abort_chunk(chunk_t *c) {
}
