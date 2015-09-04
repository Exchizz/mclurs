#

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
#include "tidy.h"

extern void *snapshot_zmq_ctx;

static void *tidy;

/*
 * Establish tidy comms:  this routine gets called first of all threads, so it
 * creates the context.
 */

static char *create_tidy_comms(void **s) {
  if( !snapshot_zmq_ctx )
    snapshot_zmq_ctx = zmq_ctx_new();
  if( !snapshot_zmq_ctx ) {
    return "failed to create ZMQ context";
  }
  /* Create and initialise the sockets: LOG socket */
  *s = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PULL, LOG_SOCKET);
  if( *s == NULL ) {
    return "unable to create MAIN thread log socket";
  }

  tidy = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, TIDY_SOCKET);
  if(tidy == NULL)
    return "unable to create TIDY thread listener";
  return NULL;
}

/*
 * Unmap data blocks after writing.  Runs as a thread which continues
 * until a zero-length message is received signalling the end of the
 * unmap requests.  The argument passed is the address for the MAIN thread's
 * log receiver socket, which is created here along with the context.
 */

/* Add code to deal with the die_die_die_now flags! */

void *tidy_main(void *arg) {
  char *err;
  int   ret;
  block b;

  err = create_tidy_comms((void **)arg);
  if(err) {
    die_die_die_now++;
    return (void *) err;
  }

  while( ret = zh_get_msg(tidy, 0, sizeof(block), &b) ) {
    assertv(ret > 0, "TIDY read message error, ret=%d\n", ret);
    if(b.b_data == NULL)
      break;
    munmap(b.b_data, b.b_bytes);
  }
  zmq_close(tidy);
  return (void *) "TIDY thread terminates normally";
}

