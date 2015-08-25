#

/*
 * The main function for thr reader thread
 */

#define READER_CMD_ADDR	"inproc://Reader-CMD"
#define READER_QUEUE_ADDR "inproc://Reader-Q"

#define READER_MSGBUF	16		/* Size of internal command frame */

/*
 * Reader parameter structure.
 */

typedef struct {
  double      r_frequency;	/* Total sampling frequency */
  int         r_schedprio;	/* Reader real-time priority */
  int         r_bufsz;		/* Reader buffer size */
  double      r_window;		/* Snapshot window (must fit in buffer) */
  const char *r_device;		/* Comedi device to use */

  /* These below are computed by the reader and exported */
  int	      r_inter_sample_ns;    /* [ns] for one sample */
  uint64_t    r_capture_start_time; /* [ns from epoch] */
  int	      r_running;	    /* Thread is running and ready */
}
  rparams;

extern rparams reader_parameters;
extern int     verify_reader_params(rparams *, strbuf);
extern void   *reader_main(void *);

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
