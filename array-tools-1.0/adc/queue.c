#

#include <stdlib.h>
#include <assert.h>
#include "queue.h"

/*
 * Implements a doubly-linked queue in ring form.
 *
 * Invariant:  every q structure is doubly-linked;  new structures are singletons.
 */

queue *init_queue(queue *p) {
  if( p == NULL ) {
    p = (queue *)calloc(1, sizeof(queue));
    assert(p != NULL);
  }
  p->q_next = p->q_prev = p;
  return p;
}

/*
 * Remove p from its queue and make it a singleton.  You cannot detach
 * a singleton from its queue.
 */

queue *de_queue(queue *p) {
  if( p->q_next == p )
    return NULL;
  p->q_prev->q_next = p->q_next;
  p->q_next->q_prev = p->q_prev;
  p->q_next = p->q_prev = p;
  return p;
}

/*
 * Splice q and p together so that p immediately follows q and the
 * next and prev chains continue in the correct senses
 */

queue *splice_queue(queue *q, queue *p) {
  queue *qn, *pp;

  qn = q->q_next;
  q->q_next = p;
  pp = p->q_prev;
  p->q_prev = q;
  qn->q_prev = pp;
  pp->q_next = qn;
  return q;
}
