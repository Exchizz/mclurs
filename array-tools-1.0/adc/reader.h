#

/*
 * The main function for thr reader thread
 */

extern int   verify_reader_params(param_t [], int);
extern void *reader_main(void *);

#define READER_CMD_ADDR	"inproc://Reader-CMD"
#define READER_POS_ADDR	"inproc://Reader-POS"

#define READER_MSGBUF	16		/* Size of internal command frame */

extern int      inter_sample_ns;
extern uint64_t capture_start_time; /* [ns from epoch] */

/*
 * Chunks are blocks of data to be copied by the Reader.
 */

#include	"tidy.h"

typedef struct _snap *snapshot;	/* Pointer to snapshot descriptor (defined privately by the Writer) */

typedef struct {
  queue		   ch_Q;	/* Queue structure for the write queue */
  snapshot	  *ch_snapshot;	/* The snapshot to which this chunk belongs */
  block		   ch_data;	/* Where to put the data */
#define ch_addr		ch_data.b_data	 /* Data buffer address */
#define ch_bytes	ch_data.b_bytes	 /* Data buffer size in bytes ... */
  int		   ch_samples;  /* ... and in samples */
  int		   ch_state;	/* State of this chunk */
  uint64_t	   ch_first;	/* First sample to be written */
  uint64_t	   ch_last;	/* Last sample of chunk */
  uint64_t	   ch_deadline;	/* Deadline for completion in [ns] after epoch */
}
  chunk;
