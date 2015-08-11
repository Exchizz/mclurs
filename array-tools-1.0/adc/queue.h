#

typedef struct q
{
  struct q  *q_next;
  struct q  *q_prev;
}
  queue;

extern queue *de_queue(queue *);
extern queue *init_queue(queue *);
extern queue *splice_queue(queue *,queue *);

#define queue_next(q)	((q)->q_next)
#define queue_prev(q)	((q)->q_prev)

#define queue_ins_after(q,i)  splice_queue((q), (i))
#define queue_ins_before(q,i)  splice_queue((i), (q))
