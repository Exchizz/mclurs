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

static int create_tidy_comms() {
  if( !snapshot_zmq_ctx )
    snapshot_zmq_ctx = zmq_ctx_new();
  if( !snapshot_zmq_ctx ) {
    return 1;
  }
  tidy = zh_bind_new_socket(snapshot_zmq_ctx, ZMQ_PAIR, TIDY_SOCKET);
  return tidy != NULL;
}

/*
 * Unmap data blocks after writing.  Runs as a thread which continues
 * until a zero-length message is received signallings the end of the
 * unmap requests.
 */

void *tidy_main(void *arg) {
  int ret;
  block b;

  ret = create_tidy_comms();
  if(ret != 0)
    return (void *) "Comms initialisation failure";

  while( ret = zh_get_msg(tidy, 0, sizeof(block), &b) ) {
    assertv(ret > 0, "Tidy read message error, ret=%d\n", ret);
    if(b.b_data == NULL)
      break;
    munmap(b.b_data, b.b_bytes);
  }
  zmq_close(tidy);
  return (void *) "Tidy thread terminates normally";
}

