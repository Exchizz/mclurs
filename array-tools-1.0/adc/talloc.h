#

/*
 * Table-based storage allocator.
 */

typedef struct _pool pool;

extern void *talloc(pool *, int);
extern void  tfree(void *);

extern pool *pool_create(void *, int, int, int);
extern int   pool_extend(pool *, void *, int, int);
extern void  pool_destroy(pool *);

#define	T_STATIC	0x1000000 /* Data blocks allocated statically */

extern const int talloc_overhead; /* Size of the item header structure */

/*
 * Routines to allow re-use of item header structure information for
 * state and queue when an object is allocated.  Use with care!
 */

extern void i2free_queue(void *);
extern void tset_state(void *, int);
extern int  tget_state(void *);
extern void  *i2usrq_de_queue(void *);
extern queue *i2usrq_ins_after(queue *, void *);
extern queue *i2usrq_ins_before(queue *, void *);
