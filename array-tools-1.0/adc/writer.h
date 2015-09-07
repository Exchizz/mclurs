#

/*
 * The main function for the writer thread
 */

#define	WRITER_CMD_ADDR	"inproc://Writer-CMD"

#define MIN_RAM_MB	16
#define MAX_RAM_MB	256
#define MIN_CHUNK_SZ	128
#define MAX_CHUNK_SZ	4096
#define MIN_NFRAMES	4

typedef struct {
  const char  *w_snapdir;
  int	       w_schedprio;
  int	       w_lockedram;
  int	       w_chunksize;

  /* The values below are computed and exported */
  int	       w_nframes;	/* The number of transfer frames prepared */
  int	       w_snap_dirfd;	/* The snapdir path fd */
  int	       w_snap_curfd;	/* The path fd of the 'working' directory */
  int	       w_running;	/* Thread is running and ready */
}
  wparams;

extern int   verify_writer_params(wparams *, strbuf);
extern void *writer_main(void *);

#define FILE_NAME_SIZE		32

#define SNAP_WR_PRELOAD	1		/* Queue this many snapr structures in advance in the Reader */

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
