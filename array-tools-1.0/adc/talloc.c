#

/*
 * Table-based storage allocator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "queue.h"
#include "talloc.h"

struct _pool {
  queue		p_Q;		/* The free queue for the pool */
  void	       *p_data;		/* The data block for this pool */
  int		p_size;		/* The size of the data block */
  int		p_objsize;	/* The size of the objects being managed */
  pool	       *p_next;		/* The next pool descriptor in this pool's chain */
};

typedef struct _item {		/* Storage descriptor block */
  queue		i_Q;		/* The item's queue descriptor */
  pool	       *i_pool;		/* The pool this item comes from */
  int		i_state;	/* The state of this item:  zero means FREE, anything else means in use */
}
  item;

const int talloc_overhead = sizeof(item);

#define STATE_FREE	0	/* Zero means free, anything else is busy */
#define STATE_BUSY	1

/*
 * Allocate an item from a pool.  The item is returned with the given
 * state set.  If state is 0 (i.e. STATE_FREE) then state is set to
 * STATE_BUSY.  Returns NULL with ENOMEM if no items available.
 */

void *talloc(pool *p, int state) {
  item *i;

  if( queue_singleton(&p->p_Q) ) { /* The free queue is empty */
    errno = ENOMEM;
    return NULL;
  }

  /* Run forward round the free queue looking for a FREE item */
  for(i=(item *)p->p_Q.q_next;
      i->i_Q.q_next != p->p_Q.q_next && i->i_state != STATE_FREE;
      i=(item *)i->i_Q.q_next);
  if(i->i_state != STATE_FREE) { /* Didn't find one... */
    errno = ENOMEM;
    return NULL;
  }

  /* We have a free item */
  de_queue(&i->i_Q);
  i->i_state = state==STATE_FREE? STATE_BUSY : state;
  return &i[1];
}

/*
 * Allow user to manipulate the item queue header, when allocated:
 * dequeue the item from a user queue.  Use the external object
 * pointers, i.e. the end of the item structure.
 *
 * N.B. Returns the item pointer, not the queue member!
 */

void *i2usrq_de_queue(void *v) {
  item *i = &((item *)v)[-1];
  de_queue(&i->i_Q);
  return v;
}

/*
 * Enqueue an item into a user queue, after the given point. Use the
 * external object pointer, i.e. the end of the item structure.
 *
 * N.B. Returns the queue member, not the item pointer!
 */

queue *i2usrq_ins_after(queue *q, void *v) {
  item *i = &((item *)v)[-1];
  return queue_ins_after(q, &i->i_Q);
}

/*
 * Enqueue an item into a user queue, before the given point. Use the
 * external object pointer, i.e. the end of the item structure.
 *
 * N.B. Returns the queue member, not the item pointer!
 */

queue *i2usrq_ins_before(queue *q, void *v) {
  item *i = &((item *)v)[-1];
  return queue_ins_before(q, &i->i_Q);
}

/*
 * Return an item to the pool's free queue, without changing its state.
 * Items whose state is not STATE_FREE will be skipped by the allocator.
 */

void i2free_queue(void *v) {
  item *i = &((item *)v)[-1];
  pool *p = i->i_pool;

  queue_ins_after(&p->p_Q, &i->i_Q);
}

/*
 * Set the state of an item.
 */

void tset_state(void *v, int state) {
  item *i = &((item *)v)[-1];
  i->i_state = state;
}

/*
 * Get the state of an item.
 */

int tget_state(void *v) {
  item *i = &((item *)v)[-1];
  return i->i_state;
}

/*
 * Free an item:  add it to the pool's free queue and change the state to STATE_FREE.
 * Do not call both this and i2free_queue() on the same item!!
 */

void tfree(void *v) {
  i2free_queue(v);
  tset_state(v, STATE_FREE);
}

/*
 * Create a pool backed by a block of storage of size nb based at addr, for objects of size os.
 * If the storage is not dynamic, then flags should include T_STATIC.
 */

pool *pool_create(void *addr, int nb, int os, int flags) {
  pool *n = (pool *)calloc(1, sizeof(struct _pool));
  int   i, no;

  if(n == NULL)
    return NULL;

  no = nb / (os + talloc_overhead); /* Number of objects we can fit into the block */
  if(no < 1) {
    free(n);
    errno = EINVAL;
    return NULL;
  }

  n->p_data = addr;
  n->p_objsize = os;
  n->p_size = nb | flags;
  for(i=0; i<no; i++) {
    item *ip = (item *)(addr+i*nb);
    ip->i_pool = n;
    ip->i_state = STATE_FREE;
    init_queue(&ip->i_Q);
    queue_ins_after(&n->p_Q, &ip->i_Q);
  }
  return n;
}

/*
 * Add another block of storage to the given pool.
 */

int pool_extend(pool *p, void *addr, int nb, int flags) {
  pool *n = (pool *)calloc(1, sizeof(*p));
  pool *q;
  int   i, no;

  if(n == NULL)
    return -1;

  no = nb / (p->p_objsize + talloc_overhead); /* Number of objects we can fit into the block */
  if(no < 1) {
    free(n);
    errno = EINVAL;
    return -1;
  }

  n->p_data = addr;
  n->p_size = (nb*no) | flags;
  for(q=p; q->p_next != NULL; q=q->p_next);
  q->p_next = n;
  for(i=0; i<no; i++) {
    item *ip = (item *)(addr+i*nb);
    ip->i_pool = p;
    ip->i_state = STATE_FREE;
    init_queue(&ip->i_Q);
    queue_ins_after(&p->p_Q, &ip->i_Q);
  }
  return 0;
}

/*
 * Destroy a pool:  release all non-static storage blocks and release the pool descriptors.
 */

void pool_destroy(pool *p) {
  pool *n;

  do {
    n=p->p_next;
    if( !(p->p_size & T_STATIC) )
      free(p->p_data);
    free(p);
  } while( p=n );
}

