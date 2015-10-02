#

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

#endif /* _RTPRIO_H */
