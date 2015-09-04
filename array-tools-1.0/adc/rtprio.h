#

#ifndef _RTPRIO_H
#define _RTPRIO_H

#include <sys/capability.h>

/*
 * Routine(s) for establishing thread real-time scheduling
 */

extern int set_rt_scheduling(int);
extern int check_permitted_capabilities_ok();

#endif /* _RTPRIO_H */
