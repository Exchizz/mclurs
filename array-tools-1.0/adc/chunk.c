#

#include "stdlib.h"
#include "stdint.h"

#include "mman.h"
#include "strbuf.h"
#include "chunk.h"

/*
 * Set up the mmap frames for data transfer to snapshot files.
 */

static int    nframes;	  /* The number of simultaneous mmap frames */
static block *framelist;  /* The list of mmap frame descriptors */

int init_frame_system(strbuf e, int nfr, int ram, int chunk) {

  framelist = (block *)calloc(nfr, sizeof(block));
  if( framelist ) {
    void *map = mmap_locate(ram*1024*1024, 0);
    int   n;

    if(map == NULL) {
      strbuf_appendf(e, "Cannot mmap %d MiB of locked transfer RAM: %m", ram);
      free((void *) framelist );
      return -1;
    }
    for(n=0; n<nfr; n++) { /* Initialise the frame memory pointers, leave sizes as 0 */
      framelist[n].b_data = map;
      map += chunk;
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
 * --------------------------------------------------------------------------------
 * FUNCTIONS ETC. FOR SNAPSHOT FILE CHUNK STRUCTURES:  MANY OF THESE PER FILE TO CAPTURE.
 *
 * --------------------------------------------------------------------------------
 */

/*
 * Allocate a new chunk descriptor
 */

chunk_t *chunk_t *alloc_chunk() {
}

/*
 * Finished with chunk descriptors
 */

void free_chunk(chunk_t *c) {
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

