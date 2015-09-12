#

#include "general.h"

/*
 * Table-based storage allocator.
 */

typedef struct _pool pool;

export void *talloc(pool *, int);
export void  tfree(void *);

export pool *pool_create(void *, int, int, int);
export int   pool_extend(pool *, void *, int, int);
export void  pool_destroy(pool *);

#define	T_STATIC	0x1000000 /* Data blocks allocated statically */

export const int talloc_overhead; /* Size of the item header structure */

/*
 * Routines to allow re-use of item header structure information for
 * state and queue when an object is allocated.  Use with care!
 */

export void i2free_queue(void *);
export void tset_state(void *, int);
export int  tget_state(void *);
export void  *i2usrq_de_queue(void *);
export queue *i2usrq_ins_after(queue *, void *);
export queue *i2usrq_ins_before(queue *, void *);
