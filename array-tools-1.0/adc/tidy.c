#

#include "general.h"

/*
 * Low-priority thread that unlocks pages after they've been filled.
 */

#include <stdio.h>
#include <stdlib.h>
#include "assert.h"
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <zmq.h>

#include "util.h"
#include "strbuf.h"
#include "chunk.h"
#include "param.h"
#include "tidy.h"
#include "snapshot.h"

import void *snapshot_zmq_ctx;

private void *tidy;
private void *log;

/*
 * Establish tidy comms:  this routine gets called first of all threads, so it
 * creates the context.
 */

private char *create_tidy_comms(void **s) {
  if( !snapshot_zmq_ctx )
    snapshot_zmq_ctx = zmq_ctx_new();
  if( !snapshot_zmq_ctx ) {
    return "failed to create ZMQ context";
  }
  
  /* Create and initialise the sockets: */

  /* MAIN thread's log socket */
  *s = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PULL, LOG_SOCKET);
  if( *s == NULL ) {
    return "unable to create MAIN thread log socket";
  }

  /* TIDY's socket for work messages */
  tidy = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, TIDY_SOCKET);
  if(tidy == NULL)
    return "unable to create TIDY thread listener";

  /* TIDY's socket for log messages */
  log = zh_connect_new_socket(snapshot_zmq_ctx, ZMQ_PUSH, LOG_SOCKET);
  if(log == NULL)
    return "unable to create TIDY thread log socket";

  return NULL;
}

/* Close the TIDY thread's comms channels */

private void close_tidy_comms() {
  zmq_close(tidy);
  zmq_close(log);
}

/*
 * Unmap data blocks after writing.  Runs as a thread which continues
 * until a zero-length message is received signalling the end of the
 * unmap requests.  The argument passed is the address for the MAIN thread's
 * log receiver socket, which is created here along with the context.
 */

public void *tidy_main(void *arg) {
  char  *err;
  int    ret;
  frame *f;

  err = create_tidy_comms((void **)arg);
  if(err) {
    die_die_die_now++;
    return (void *) err;
  }

  zh_put_multi(log, 1, "TIDY   thread initialised");
  
  while( ret = zh_get_msg(tidy, 0, sizeof(frame *), &f) && !die_die_die_now ) {
    assertv(ret==sizeof(frame *), "TIDY read message error, ret=%d\n", ret);
    release_frame(f);
  }

  zh_put_multi(log, 1, "TIDY   thread terminates by return");
  
  /* Clean up our ZeroMQ sockets */
  close_tidy_comms();
  return (void *) "normal exit";
}

