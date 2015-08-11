#

/*
 * The main functino for the writer thread
 */

extern int   verify_writer_params(param_t [], int);
extern void *writer_main(void *);

#define	WRITER_CMD_ADDR	"inproc://Writer-CMD"

#define SNAP_NAME_SIZE	24	/* Big enough to hold a 64 bit integer/pointer as hex */
#define N_SNAP_PARAMS   7

typedef struct _snapr	snapr;		/* Information needed by the Reader thread to snapshot a range of samples */
typedef struct _snapw	snapw;		/* Information needed by the Writer thread to set up a snapshot series */

struct _snapr
{
  queue	     Q;				/* The queue header for enqueuing the snapshot */
  sampl_t   *mmap;			/* The start of the mapped page buffer for the file */
  uint64_t   first,			/* First sample to record */
	     last;			/* The first sample after the recording */
  unsigned   bytes;			/* The size of the snapshot in BYTES */
  unsigned   samples;			/* The size of the snapshot in SAMPLES */
  unsigned   count;			/* The number of repetitions of this snapshot to do */
  sampl_t   *start;		        /* The position that the data will be found in the ring buffer */
  int	     rd_state;			/* The state of the snapshot per the Reader (see below) */
  char       file[SNAP_NAME_SIZE];	/* Current file 'name' = first sample number in file */
  struct timespec ready;		/* Debugging timing data */
  struct timespec written;
  snapw     *parent;			/* The writer structure that owns this descriptor */
};

#define SNAP_WR_PRELOAD	1		/* Queue this many snapr structures in advance in the Reader */

struct _snapw
{
  char	     name[SNAP_NAME_SIZE];	/* Snapshot 'name' = structure address  */
  char	    *params[N_SNAP_PARAMS];	/* Values of command parameters */
  int	     wr_state;			/* The state of the snapshot per the Writer (see below) */
  int	     dirfd;			/* File descriptor for the directory where to write */
  char	    *path;			/* The path component supplied with the command (duplicates param[] entry, don't free) */
  int	     stride;			/* The number of samples to use when refreshing a snapr descriptor */
  int	     preload;			/* The number of descriptors to queue up in advance in the Reader */
  snapr     *this_snap;			/* Reader snapshot structure associated with current snapshot */
};

#define	SNAPSHOT_INIT		0 /* Structure just created */
#define SNAPSHOT_ERROR		1 /* Error found during checking (off queue) */
#define SNAPSHOT_WAITING	2 /* Waiting in Reader ready queue */
#define SNAPSHOT_WRITTEN	4 /* Snapshot written correctly (off queue) */
#define SNAPSHOT_STOPPED	6 /* Snapshot structure OK but reader not running */

#define SNAPSHOT_FREE		0 /* Structure is not in use */
#define SNAPSHOT_ALLOC		1 /* Structure is allocated and being filled */
#define SNAPSHOT_CHECK		2 /* Structure initialised, reader asked to check */
#define SNAPSHOT_REPLIED	3 /* Reader confirmation received, reply sent to command source */
#define SNAPSHOT_PUBLISHED	4 /* Reader completed or aborted snapshot */
#define SNAPSHOT_DONE		5 /* Structure is finished with */
#define SNAPSHOT_REPEAT		6 /* Snapshot is repeating with count > 0 */
