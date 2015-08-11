#

/*
 * Low-priority thread that unlocks pages after they've been filled.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/mman.h>
#include <pthread.h>
#include <zmq.h>

#include "util.h"
#include "tidy.h"

#define TIDY_SOCKET "inproc://snapshot-TIDY"

/*
 * Unmap data blocks after writing.  The thread continues until a
 * zero-length message is received, which signals the end of the unmap
 * requests.
 */

static void *tidy(void *s) {
  int ret;
  block b;

  while( ret = zh_get_msg(s, 0, sizeof(block), &b) ) {
    assert(ret > 0);
    if(b.b_data == NULL)
      break;
    munmap(b.b_data, b.b_bytes);
  }
  zmq_close(s);
  return (void *) "Tidy thread terminates normally";
}

/*
 * Create the tidy thread and its socket pair.  Return the thread
 * identifier and the socket handle.
 */

int create_tidy_thread(void *ctx, pthread_t *tp, void **sp) {
  void *in, *out;
  pthread_attr_t  tidy_attr;
  int   ret;

  in = zmq_socket(ctx, ZMQ_PAIR);
  assert(in != NULL);
  ret = zmq_bind(in, TIDY_SOCKET);
  assert(ret >= 0);

  out = zmq_socket(ctx, ZMQ_PAIR);
  assert(out != NULL);
  ret = zmq_connect(out, TIDY_SOCKET);
  assert(ret >= 0);

  pthread_attr_init(&tidy_attr);
  if( pthread_create(tp, &tidy_attr, tidy, out) < 0 )
    return -1;
  *sp = in;
  return 0;
}

