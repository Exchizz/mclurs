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
#include "assert.h"
#include "queue.h"

/*
 * Implements a doubly-linked queue in ring form.
 *
 * Invariant:  every q structure is doubly-linked;  new structures are singletons.
 */

public queue *init_queue(queue *p) {
  if( p == NULL ) {
    p = (queue *)calloc(1, sizeof(queue));
    assertv(p != NULL, "Queue alocation failure\n");
  }
  p->q_next = p->q_prev = p;
  return p;
}

/*
 * Remove p from its queue and make it a singleton.  You cannot detach
 * a singleton from its queue.
 */

public queue *de_queue(queue *p) {
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

public queue *splice_queue(queue *q, queue *p) {
  queue *qn, *pp;

  qn = q->q_next;
  q->q_next = p;
  pp = p->q_prev;
  p->q_prev = q;
  qn->q_prev = pp;
  pp->q_next = qn;
  return q;
}

/*
 * Unsplice a queue: cut the ring at start and end and relink.  Also
 * join start and end.
 */
/*
public queue *unsplice_queue(queue *start, queue *end) {
}
*/

/*
 * Apply a function to each queue member in [start,end).  The function
 * is called with arg as its first argument and the queue structure
 * pointer as its second.  The first function, map_queue_nxt,
 * traverses the segment "forward" while the second goes "backward".
 *
 * If start == end or end is not in the list (e.g. end is NULL) the
 * functions traverse the whole list visiting each node exactly once.
 */

public void map_queue_nxt(queue *start, queue *end, void (*fn)(void *, queue *), void *arg) {

  for_nxt_in_Q(queue *p, start, end)
    (*fn)(arg, p);
  end_for_nxt;
}

public void map_queue_prv(queue *start, queue *end, void (*fn)(void *, queue *), void *arg) {

  for_prv_in_Q(queue *p, start, end)
    (*fn)(arg, p);
  end_for_prv;
}
