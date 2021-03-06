#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _QUEUE_H
#define _QUEUE_H

#include "general.h"

typedef struct q
{
  struct q  *q_next;
  struct q  *q_prev;
}
  queue;

typedef void (*qmapfn)(void *, queue *);

export queue *de_queue(queue *);
export queue *init_queue(queue *);
export queue *splice_queue(queue *,queue *);
//export queue *unsplice_queue(queue *, queue *);
export void   map_queue_nxt(queue *, queue *, qmapfn, void *);
export void   map_queue_prv(queue *, queue *, qmapfn, void *);

#define queue_next(q)   ((q)->q_next)
#define queue_prev(q)   ((q)->q_prev)

#define queue_ins_after(q,i)  splice_queue((q), (i))
#define queue_ins_before(q,i)  splice_queue((i), (q))

#define queue_singleton(q)  ((q)->q_next == (q) && (q)->q_prev == (q))

#define QUEUE_HEADER(name)  queue name = { &name, &name }

/*
 * These macro definitions do essentially the same as the
 * map_queue_nxt and map_queue_prv but they don't leave the current
 * local scope -- so for instance one can break the loop early in this
 * form whereas one cannot in the (default) map function.
 *
 * The var argument is a variable that will hold the current node
 * pointer as the loop proceeds.  It can be declared locally to the
 * for_nxt by including its declaration in the macro call:
 *
 * for_nxt_in_Q(queue *ptr,start,end) ...
 *
 * or it can be a variable declared outside the scope of the for_nxt
 * in which case just its name is given as argument and it will
 * persist after the map-loop ends.
 *
 * The macros evaluate start and end exactly once and execute the User
 * Code once for each list element in the range [start,end) with var
 * set to that element.  If start==end or end is not actually in the
 * list, the loop traverses the whole list exactly once visiting each
 * node exactly once.
 *
 * The strict versions do nothing if start==end.
 *
 * Note that it is also possible to remove node __p during the USER
 * CODE because it is neither the node we are about to work on nor the
 * end point node.  It may be the start node, however: the user should
 * deal with that case!
 */

#define for_nxt_in_Q(var,start,end)                     \
do { queue *__s = (start), *__e = (end);                \
     queue *__p = __s;                                  \
     int    __done = 0;                                 \
     while(!__done) { queue *__n = queue_next(__p);     \
       __done = (__n == __s || __n == __e);             \
       var = __p;  __p = __n;                           \
       /* USER CODE GOES HERE */

#define for_nxt_in_strict_Q(var,start,end)              \
do { queue *__s = (start), *__e = (end);                \
     if(__s == __e) break;                              \
     queue *__p = __s;                                  \
     int    __done = 0;                                 \
     while(!__done) { queue *__n = queue_next(__p);     \
       __done = (__n == __s || __n == __e);             \
       var = __p;  __p = __n;                           \
       /* USER CODE GOES HERE */

#define end_for_nxt                                     \
     } } while(0)

#define for_prv_in_Q(var,start,end)                     \
do { queue *__s = (start), *__e = (end);                \
     queue *__p = __s;                                  \
     int    __done = 0;                                 \
     while(!__done) { queue *__n = queue_prev(__p);     \
       __done = (__n == __s || __n == __e);             \
       var = __p;  __p = __n;
       /* USER CODE GOES HERE */

#define for_prv_in_strict_Q(var,start,end)              \
do { queue *__s = (start), *__e = (end);                \
     if(__s == __e) break;                              \
     queue *__p = __s;                                  \
     int    __done = 0;                                 \
     while(!__done) { queue *__n = queue_prev(__p);     \
       __done = (__n == __s || __n == __e);             \
       var = __p;  __p = __n;
       /* USER CODE GOES HERE */

#define end_for_prv                                     \
     } } while(0)

#endif /* _QUEUE_H */
