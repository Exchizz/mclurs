#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <comedilib.h>

#include "error.h"
#include "assert.h"
#include "util.h"
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

private int    nframes;    /* The number of simultaneous mmap frames */
private frame *framelist;  /* The list of mmap frame descriptors */
private int    n_frame_Q = 0;

private QUEUE_HEADER(frameQ);

public int init_frame_system(strbuf e, int nfr, int ram, int chunk) {

  LOG(WRITER, 1, "Init %d frames of size %d[kiB] in ram %d[MiB]\n", nfr, chunk, ram);

  framelist = (frame *)calloc(nfr, sizeof(frame));
  if( framelist ) {
    void *map = mmap_locate(ram*1024*1024, 0);
    int   n;

    if(map == NULL) {
      strbuf_appendf(e, "Cannot mmap %d[MiB] of locked transfer RAM: %m", ram);
      free((void *) framelist );
      return -1;
    }
    munmap(map, ram*1024*1024);
    
    for(n=0; n<nfr; n++) { /* Initialise the frame memory pointers, leave sizes as 0 */
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

private void scan_framelist() {
  int    n;
  frame *f;

  for(n=0,f=framelist; n<nframes; n++, f++) {
    if(f->f_map.b_bytes)        /* If non-zero, it's in use */
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

private frame *alloc_frame() {
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
  f->f_map.b_bytes = 1;         /* In-use;  real size is filled in by caller */
  return f;
}

/*
 * Release a frame descriptor.  N.B.  this is done in the TIDY thread,
 * so must be atomic: the frame is released by setting the bytes value
 * to zero.  The munmap() here complements the mmap call in the chunk
 * mapper below.
 */

public void release_frame(frame *f) {
  if(f->f_map.b_data == NULL)
      return;
  munmap(f->f_map.b_data, f->f_map.b_bytes);
  f->f_map.b_bytes = 0;
}

/*
 * Report the index of a frame pointer in the table.
 */

public int frame_nr(frame *f) {
  return f - framelist;
}

/*
 * Functions for dealing with transfer chunk descriptors.
 */

private uint16_t chunk_counter;

#define N_CHUNK_ALLOC   (4096/sizeof(chunk_t))

private QUEUE_HEADER(chunkQ);
private int N_in_chunkQ = 0;

/*
 * Allocate n new chunk descriptors, chained using the WRITER queue descriptor
 */

public chunk_t *alloc_chunk(int nr) {
  queue *ret;

  if( N_in_chunkQ < nr ) {      /* The queue doesn't have enough */
    int n;

    for(n=0; n<N_CHUNK_ALLOC; n++) {
      queue *q = (queue *)calloc(1, sizeof(chunk_t));

      if( !q ) {                        /* Allocation failed */
        if( N_in_chunkQ >= nr )
          break;                        /* But we have enough now anyway */
        return NULL;
      }
      init_queue(q);
      queue_ins_after(&chunkQ, q);
      N_in_chunkQ++;
    }
  }

  ret = de_queue(queue_next(&chunkQ));
  chunk_t *c = qp2chunk(ret);
  init_queue(&c->c_rQ);
  c->c_name = ++chunk_counter;
  
  while(--nr > 0) {             /* Collect enough to satisfy request */
    chunk_t *c = qp2chunk(de_queue(queue_next(&chunkQ)));

    init_queue(&c->c_wQ);       /* Redundant... */
    init_queue(&c->c_rQ);
    c->c_name = ++chunk_counter;
    queue_ins_before(ret, chunk2qp(c));
  }

  return c;
}

/*
 * Finished with chunk descriptors chained using the writer queue descriptor.
 * Assume the reader queue descriptor is detached.
 */

public void release_chunk(chunk_t *c) {
  queue *q = chunk2qp(c);
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
 * FInd a frame for a chunk and map the chunk into memory.  This may
 * take arbitrary time since this is where Linux has to find us new
 * pages.  This code runs in the WRITER thread.
 */

public int map_chunk_to_frame(chunk_t *c) {
  frame *fp = alloc_frame();
  void  *map;
  
  if(fp == NULL) {   /* This is not a fatal error:  there may be no frames available for transient reasons */
    return -1;
  }

  fp->f_map.b_bytes = c->c_samples*sizeof(sampl_t);

  /* Would really like to do WRONLY here, but I *think* that will break */
  map = mmap_and_lock(c->c_fd, c->c_offset, fp->f_map.b_bytes, PROT_RDWR|PREFAULT_RDWR|MAL_LOCKED);

  LOG(WRITER, 3, "Map chunk %s in frame %d from fd %d offs %d with addr %p and size %d[B] gives res %p\n",
      c_nstr(c), frame_nr(fp), c->c_fd, c->c_offset, fp->f_map.b_data, fp->f_map.b_bytes, map);

  fp->f_map.b_data = map;
  if(map == NULL) {
    strbuf_appendf(c->c_error, "Unable to map chunk %s to frame %d: %m", c_nstr(c), frame_nr(fp));
    set_chunk_status(c, SNAPSHOT_ERROR);
    fp->f_map.b_bytes = 0;      /* Mark frame as free */
    return -1;
  }
  c->c_frame = fp;              /* Succeeded, chunk now has a mapped frame */
  return 0;
}

/*
 * Copy the data for a chunk from the ring buffer into the frame.  Apply the
 * appropriate ADC conversion.  Used by the READER thread.
 *
 * This routine assumes that when the chunk's ring buffer pointer is computed by
 * adc_setup_chunk(), it also ensures that the previous sample to the chunk's
 * first sample is available in the ring buffer at the previous address (the
 * sample value is needed for decorrelation processing in the LUT module).  Note
 * the need to handle the edge case where the chunk starts at sample 0.
 */

public void copy_chunk_data(chunk_t *c) {
  convertfn fn = c->c_convert;
  sampl_t   prev;

   LOG(READER, 3, "Copy chunk %s using fn %p from %p to %p size %d[spl]\n", 
       c_nstr(c), fn, c->c_ring, c->c_frame->f_map.b_data, c->c_samples);

   prev = c->c_first > 0? ((sampl_t *)c->c_ring)[-1] : 0;
   (*fn)((sampl_t *)c->c_frame->f_map.b_data, (sampl_t *)c->c_ring, c->c_samples, prev);
   set_chunk_status(c, SNAPSHOT_WRITTEN);
}

/*
 * Generate a debugging line for a chunk desdcriptor.  Put it in the buffer buf.
 * Return the actual size, no greater than the space available.
 */

#define qp2cname(p)     (c_nstr(qp2chunk(p)))

public const char *rq2cname(queue *p) {
  import queue *rcQp, *wcQp;

  if(p == rcQp)
    return "RQhead";
  if(p == wcQp)
    return "WQhead";
  return c_nstr(rq2chunk(p));
}

public int debug_chunk(char buf[], int space, chunk_t *c) {
  import const char *snapshot_status(int);
  import const char *snapfile_name(snapfile_t *);
  int used;

  used = snprintf(buf, space,
                  "chunk %s at %p"
                  "wQ[%s,%s] "
                  "rQ[%s,%s] "
                  "RG %p FR %p PF %s status %s "
                  "S:%08lx F:%016llx L:%016llx\n",
                  c_nstr(c), c,
                  qp2cname(queue_prev(&c->c_wQ)), qp2cname(queue_next(&c->c_wQ)),
                  rq2cname(queue_prev(&c->c_rQ)), rq2cname(queue_next(&c->c_rQ)),
                  c->c_ring, c->c_frame, snapfile_name(c->c_parent), snapshot_status(c->c_status),
                  c->c_samples, c->c_first, c->c_last
                  );
  if(used >= space)
    used = space;
  return used;
}

/*
 * Routines to handle status codes for chunks, snapfiles and snapshots.
 *
 * Display snapshot status codes
 * Return chunk owner
 * Set chunk owner
 */

public const char *snapshot_status(int st) {
  private const char *stab[]
    = {
    "INI", "ERR", "PRP", "RDY", "...", ">>>", "+++", "FIN",
  };
  st &= SNAPSHOT_STATUS_MASK;
  if(st>=0 && st<sizeof(stab)/sizeof(char *))
    return stab[st];
  return "???";
}

/*
 * Make string version of chunk name.
 * Allow up to 16 simultaneous calls with distinct answers.
 */

public const char *c_nstr(chunk_t *c) {
  private int ix = -1;
  private char store[16][8];

  ix = (ix+1)&0xf;
  snprintf(&store[ix][0], 8, "c:%04hx", c->c_name);
  return &store[ix][0];
}
