#

#include "general.h"

#define __GNU_SOURCE

#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/capability.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>

#include "rtprio.h"

#ifdef __GNU_SOURCE

#define  gettid()	(syscall(SYS_gettid)) /* No glibc interface, Linux-only call */

/*
 * Routine(s) for establishing threads in RT FIFO scheduling mode using Linux tricks
 */

public int set_rt_scheduling(int p) {
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

public int set_rt_scheduling(int p) {
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

/*
 * Routine to check we have the permitted capabilities needed for program operations
 *
 * The various threads need the following capabilities:
 *
 * CAP_IPC_LOCK  (READER and WRITER) -- ability to mmap and mlock pages.
 * CAP_SYS_NICE  (READER and WRITER) -- ability to set RT scheduling priorities
 * CAP_SYS_ADMIN (READER) -- ability to set (increase) the Comedi buffer maximum size
 * CAP_SYS_ADMIN (WRITER) -- ability to set RT IO scheduling priorities (unused at present)
 * CAP_SYS_ADMIN (TIDY)   -- ability to set RT IO scheduling priorities (unused at present)
 *
 * Otherwise the MAIN thread and the TIDY thread need no special powers.  The ZMQ IO thread
 * is also unprivileged, and is currently spawned during context creation from TIDY.
 */

public int check_permitted_capabilities_ok() {
  cap_t c = cap_get_proc();
  cap_flag_value_t v = CAP_CLEAR;
  
  if( !c )			/* No memory? */
    return -1;

  if( cap_get_flag(c, CAP_IPC_LOCK,  CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_NICE,  CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_ADMIN, CAP_PERMITTED, &v) < 0 || v == CAP_CLEAR
      ) {
    cap_free(c);
    errno = EPERM;
    return -1;
  }

  cap_free(c);
  return 0;
}

public int check_effective_capabilities_ok() {
  cap_t c = cap_get_proc();
  cap_flag_value_t v = CAP_CLEAR;
  
  if( !c )			/* No memory? */
    return -1;

  if( cap_get_flag(c, CAP_IPC_LOCK,  CAP_EFFECTIVE, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_NICE,  CAP_EFFECTIVE, &v) < 0 || v == CAP_CLEAR ||
      cap_get_flag(c, CAP_SYS_ADMIN, CAP_EFFECTIVE, &v) < 0 || v == CAP_CLEAR
      ) {
    cap_free(c);
    errno = EPERM;
    return -1;
  }

  cap_free(c);
  return 0;
}

/*
 * Get a value from the monotonic krnel clock and express in nanoseconds.
 */

public uint64_t monotonic_ns_clock() {
  uint64_t ret;
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);		/* Timestamp for debugging */
  ret = now.tv_sec;
  ret = ret*1000000000 + now.tv_nsec;
  return ret;
}

