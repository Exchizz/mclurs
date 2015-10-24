#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

/*
 * The ZMQ address for the writer thread
 */

#define WRITER_CMD_ADDR "inproc://Writer-CMD"

#define MIN_RAM_MB      16
#define MAX_RAM_MB      256
#define MIN_CHUNK_SZ    128
#define MAX_CHUNK_SZ    4096
#define MIN_NFRAMES     4

#define WRITER_MAX_CHUNK_DELAY          100 /* [ms] */
#define WRITER_MAX_CHUNKS_TRANSFER      8
#define WRITER_POLL_DELAY               50  /* [ms] */

typedef struct {

  /* These values come from environment and/or argument parameters */
  const char  *w_snapdir;
  int          w_schedprio;
  int          w_lockedram;
  int          w_chunksize;
  double       w_writeahead;
  /* Thread is running and ready -- set by main routine */
  int          w_running;
}
  wparams;

export int   verify_writer_params(wparams *, strbuf);
export void *writer_main(void *);

#define FILE_NAME_SIZE          32


