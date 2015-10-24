#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

/* Shared globals */

export void       *zmq_main_ctx;

export param_t     globals[];
export const int   n_global_params;

export int         verbose;
export int         die_die_die_now;

export int         tmpdir_dirfd;
export const char *tmpdir_path;

/* Common definitions */

#define LOG_SOCKET      "inproc://Main-LOG"

#define MSGBUFSIZE      8192
