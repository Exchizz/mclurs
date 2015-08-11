#

#define __GNU_SOURCE

#include <syscall.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>

#include "rtprio.h"

#ifdef __GNU_SOURCE

#define  gettid()	(syscall(SYS_gettid)) /* No glibc interface, Linux-only call */

/*
 * Routine(s) for establishing threads in RT FIFO scheduling mode using Linux tricks
 */

int set_rt_scheduling(int p) {
  pid_t  me = gettid();
  struct sched_param pri;
  int    mode;

  /* Attempt the operation */
  pri.sched_priority = p;
  if( sched_setscheduler(me, SCHED_FIFO, &pri) < 0 ) {
    return -1;		/* Failed for some reason */
  }

  /* Verify the operation */
  mode = sched_getscheduler(me);
  if( mode != SCHED_FIFO ) {	/* Didn't work, despite no errors... */
    errno = ENOSYS;
    return -1;
  }

  pri.sched_priority = -1;	/* Check correct priority was set... */
  if( sched_getparam(me, &pri) < 0
      || pri.sched_priority != p ) {
    errno = ENOSYS;
    return -1;
  }

  /* Successfully applied RT scheduling */
  return 0;
}

#else

/*
 * Routine(s) for establishing threads in RT FIFO scheduling mode using POSIX calls
 */

int set_rt_scheduling(int p) {
  pthread_t me = pthread_self();
  struct sched_param pri;
  int mode;

  /* Attempt the operation */
  pri.sched_priority = p;
  if( pthread_setschedparam(me, SCHED_FIFO, &pri) < 0 ) {
    return -1;		/* Failed for some reason */
  }

  /* Verify the operation */
  pri.sched_priority = -1;
  if( pthread_getschedparam(me, &mode, &pri) < 0
      || mode != SCHED_FIFO
      || pri.sched_priority != p ) {	/* Didn't work, despite no errors... */
    errno = ENOSYS;
    return -1;
  }

  /* Successfully applied RT scheduling */
  return 0;
}

#endif
