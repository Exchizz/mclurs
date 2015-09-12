#

#include "general.h"

struct readbuf			/* Information for reader ring buffer */
{ char *rb_start,		/* The start address */
       *rb_end;			/* The end address of the writable section */
  int   rb_size;		/* The size in bytes of the writable section */
  int   rb_samples;		/* The size in sampl_t of the writable section */
};

export struct readbuf *create_ring_buffer(int, const char *);
export int destroy_ring_buffer(struct readbuf *);
