#

#include "general.h"

/*
 * The ZMQ address for the writer thread
 */

#define	WRITER_CMD_ADDR	"inproc://Writer-CMD"

#define MIN_RAM_MB	16
#define MAX_RAM_MB	256
#define MIN_CHUNK_SZ	128
#define MAX_CHUNK_SZ	4096
#define MIN_NFRAMES	4

#define WRITER_MAX_CHUNK_DELAY		100 /* [ms] */
#define WRITER_MAX_CHUNKS_TRANSFER	8
#define WRITER_POLL_DELAY		50  /* [ms] */

typedef struct {

  /* These values come from environment and/or argument parameters */
  const char  *w_snapdir;
  int	       w_schedprio;
  int	       w_lockedram;
  int	       w_chunksize;
  double       w_writeahead;
  /* Thread is running and ready -- set by main routine */
  int          w_running;
}
  wparams;

export int   verify_writer_params(wparams *, strbuf);
export void *writer_main(void *);

#define FILE_NAME_SIZE		32

#define	SNAPSHOT_INIT		0 /* Structure just created */
#define SNAPSHOT_ERROR		1 /* Error found during checking or execution */
#define SNAPSHOT_PREPARE	2 /* Structure filled in, but files/chunks not done yet */
#define SNAPSHOT_READY		3 /* Snapshot etc. is ready, but waiting for READER queue space */
#define SNAPSHOT_WAITING	4 /* Snapshot etc. is ready, but waiting for data */
#define SNAPSHOT_WRITING	5 /* Snapshot file's chunks are being written */
#define SNAPSHOT_WRITTEN	6 /* Snapshot's chunk has been successfully written */
#define SNAPSHOT_COMPLETE	7 /* Snapshot written correctly (off queue) */
#define SNAPSHOT_DONE		8 /* Structure is finished with */

export const char *snapshot_status(int);

