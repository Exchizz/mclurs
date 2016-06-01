#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#ifndef _RTPRIO_H
#define _RTPRIO_H

#include "general.h"

#include <sys/capability.h>

/*
 * Routine(s) for establishing thread real-time scheduling
 */

export int set_rt_scheduling(int);
export int check_permitted_capabilities_ok();
export int check_effective_capabilities_ok();

export uint64_t monotonic_ns_clock();

#endif /* _RTPRIO_H */
