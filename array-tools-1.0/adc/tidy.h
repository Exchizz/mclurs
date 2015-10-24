#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _TIDY_H
#define _TIDY_H

#include "general.h"

export void *tidy_main(void *);

#define TIDY_SOCKET "inproc://snapshot-TIDY"

#endif /* _TIDY_H */
