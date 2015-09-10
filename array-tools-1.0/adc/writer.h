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

  /* These values come from environment and/or argument parameters */
  const char  *w_snapdir;
  int	       w_schedprio;
  int	       w_lockedram;
  int	       w_chunksize;
  double       w_writeahead;
  
  /* The values below are computed and exported */
  int	       w_nframes;	/* Number of transfer frames prepared */
  int	       w_chunksamples;	/* Number of samples in a chunk */
  int	       w_snap_dirfd;	/* Snapdir path fd */
  int	       w_snap_curfd;	/* Path fd of the 'working' directory */
  int	       w_running;	/* Thread is running and ready */
  int	       w_totxfrbytes;	/* Total schedules transfer bytes remaining */
  int	       w_nfiles;	/* Number of files in progress */
}
  wparams;

extern int   verify_writer_params(wparams *, strbuf);
extern void *writer_main(void *);

#define FILE_NAME_SIZE		32

#define	SNAPSHOT_INIT		0 /* Structure just created */
#define SNAPSHOT_ERROR		1 /* Error found during checking or execution */
#define SNAPSHOT_PREPARE	2 /* Structure filled in, but files/chunks not done yet */
#define SNAPSHOT_READY		3 /* Snapshot etc. is ready, but waiting for READER queue space */
#define SNAPSHOT_WAITING	4 /* Snapshot etc. is ready, but waiting for data */
#define SNAPSHOT_WRITING	5 /* Snapshot file's chunks are being written */
#define SNAPSHOT_COMPLETE	6 /* Snapshot written correctly (off queue) */
#define SNAPSHOT_DONE		7 /* Structure is finished with */

extern const char *snapshot_status(int);

#if 0
#define SNAPSHOT_FREE		0 /* Structure is not in use */
#define SNAPSHOT_ALLOC		1 /* Structure is allocated and being filled */
#define SNAPSHOT_CHECK		2 /* Structure initialised, reader asked to check */
#define SNAPSHOT_REPLIED	3 /* Reader confirmation received, reply sent to command source */
#define SNAPSHOT_PUBLISHED	4 /* Reader completed or aborted snapshot */
#define SNAPSHOT_REPEAT		6 /* Snapshot is repeating with count > 0 */
#define SNAPSHOT_STOPPED	6 /* Snapshot structure OK but reader not running */
#endif
