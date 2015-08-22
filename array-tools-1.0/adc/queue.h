#

#ifndef _QUEUE_H
#define _QUEUE_H

typedef struct q
{
  struct q  *q_next;
  struct q  *q_prev;
}
  queue;

extern queue *de_queue(queue *);
extern queue *init_queue(queue *);
extern queue *splice_queue(queue *,queue *);
extern queue *unsplice_queue(queue *, queue *);
extern void   map_queue_nxt(queue *, queue *, void (*)(void *, queue *), void *);
extern void   map_queue_prv(queue *, queue *, void (*)(void *, queue *), void *);

#define queue_next(q)	((q)->q_next)
#define queue_prev(q)	((q)->q_prev)

#define queue_ins_after(q,i)  splice_queue((q), (i))
#define queue_ins_before(q,i)  splice_queue((i), (q))

#define queue_singleton(q)  ((q)->q_next == (q) && (q)->q_prev == (q))

#define QUEUE_HEADER(name)  queue name = { &name, &name }

#endif /* _QUEUE_H */
