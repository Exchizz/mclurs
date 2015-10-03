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
  queue         c_Q[2];     /* Q header for READER capture queue and WRITER file chunk list*/
#define c_wQ c_Q[0]         /* Chunk Q linkage associated with the file */
#define c_rQ c_Q[1]         /* Chunk Q linkage associated with the data flow */
  frame        *c_frame;    /* Mmap'd file buffer for this chunk */
  strbuf        c_error;    /* Error buffer, for error messages (copy from snapshot_t origin) */
  snapfile_t   *c_parent;   /* Chunk belongs to this file */
  uint64_t      c_first;    /* First sample of this chunk */
  uint64_t      c_last;     /* First sample beyond this chunk */
  uint16_t     *c_ring;     /* Ring buffer start for this chunk */
  convertfn     c_convert;  /* Function to copy samples into frame with conversion */
  uint32_t      c_samples;  /* Number of samples to copy */
  uint32_t      c_offset;   /* File offset for this chunk */
  int           c_status;   /* Status of this capture chunk */
  int           c_fd;       /* File descriptor for this chunk */
  uint16_t      c_name;     /* Unique name for this chunk */
}
  chunk_t;

#define qp2chunk(q)     ((chunk_t *)(q))
#define chunk2qp(c)     (&(c)->c_Q[0])

#define chunk2rq(c)     (&(c)->c_rQ)
#define rq2chunk(q)     ((chunk_t *)&((q)[-1]))

export chunk_t *alloc_chunk(int);
export void     release_chunk(chunk_t *);
export int      map_chunk_to_frame(chunk_t *);
export void     copy_chunk_data(chunk_t *);

export int      debug_chunk(char [], int, chunk_t *);

export int      init_frame_system(strbuf, int, int, int);
export void     release_frame(frame *);

/*
 * Chunk, snapfile and snapshot status codes
 */

#define SNAPSHOT_INIT           0 /* Structure just created */
#define SNAPSHOT_ERROR          1 /* Error found during checking or execution */
#define SNAPSHOT_PREPARE        2 /* Structure filled in, but files/chunks not done yet */
#define SNAPSHOT_READY          3 /* Snapshot etc. is ready, but waiting for READER queue space */
#define SNAPSHOT_WAITING        4 /* Snapshot etc. is ready, but waiting for data */
#define SNAPSHOT_WRITING        5 /* Snapshot file's chunks are being written */
#define SNAPSHOT_WRITTEN        6 /* Snapshot's chunk has been successfully written */
#define SNAPSHOT_COMPLETE       7 /* Snapshot written correctly (off queue) */

#define SNAPSHOT_STATUS_MASK 0xff /* Mask for status values */

export const char *snapshot_status(int);

#define CHUNK_OWNER_READER  0x100 /* This chunk is managed by the READER */
#define CHUNK_OWNER_WRITER  0x200 /* This chunk is managed by the WRITER */
#define CHUNK_OWNER_MASK    0xf00 /* Mask for chunk ownership flags */

/* "Functions" to set and check status and ownership */

#define apply_status_mask(v) ((v)&SNAPSHOT_STATUS_MASK)
#define apply_owner_mask(v)  ((v)&CHUNK_OWNER_MASK)

#define chunk_in_reader(c) (apply_owner_mask((c)->c_status) == CHUNK_OWNER_READER)
#define chunk_in_writer(c) (apply_owner_mask((c)->c_status) == CHUNK_OWNER_WRITER)

#define set_chunk_owner(c,owner) do { chunk_t *__c = (c); \
  __c->c_status = (__c->c_status&~CHUNK_OWNER_MASK)|apply_owner_mask(owner); \
  } while(0)

#define set_chunk_status(c,status) do { chunk_t *__c = (c); \
  __c->c_status = (__c->c_status&~SNAPSHOT_STATUS_MASK)|apply_status_mask(status); \
  } while(0)

#define is_chunk_status(c,status) (apply_status_mask((c)->c_status) == (status))

#endif /* _CHUNK_H */

