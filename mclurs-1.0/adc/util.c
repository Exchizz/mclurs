#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "assert.h"

#include <zmq.h>

#include "util.h"

/*
 * Create, open and bind a ZMQ socket.
 */

public void *zh_bind_new_socket(void *ctx, int type, const char *url) {
  void *skt;

  skt = zmq_socket(ctx, type);
  if(skt != NULL) {
    int ret = zmq_bind(skt, url);
    if(ret < 0) {
      int safe_errno = errno;
      (void) zmq_close(skt);
      errno = safe_errno;
      skt = NULL;
    }
  }
  return skt;
}

/*
 * Create, open and connect a ZMQ socket.
 */

public void *zh_connect_new_socket(void *ctx, int type, const char *url) {
  void *skt;

  skt = zmq_socket(ctx, type);
  if(skt != NULL) {
    int ret = zmq_connect(skt, url);
    if(ret < 0) {
      int safe_errno = errno;
      (void) zmq_close(skt);
      errno = safe_errno;
      skt = NULL;
    }
  }
  return skt;
}

/*
 * Retrieve a ZMG message from a socket.  Put it in the buffer buf and
 * transfer at most size bytes.  If size is zero, we care only about
 * the arrival of the message, not its content.
 */

public int zh_get_msg(void *socket, int flags, size_t size, void *buf) {
  zmq_msg_t  msg;
  int ret;
  size_t msg_size;

  ret = zmq_msg_init(&msg);
  assertv(ret == 0, "Message init failed\n");
  ret = zmq_msg_recv(&msg, socket, flags);
  if( ret < 0 )
    return ret;
  if( !size )
    return 0;
  msg_size = zmq_msg_size(&msg);
  if( !msg_size )
    return 0;
  if( msg_size < size )
    size = msg_size;
  assertv(buf != NULL, "Called with null buf argument\n");
  bcopy(zmq_msg_data(&msg), buf, size);
  ret = zmq_msg_close(&msg);
  assertv(ret == 0, "Message close failed\n");
  return size;
}

/*
 * Returns true if there is more of this message, otherwise false
 */

public int zh_any_more(void *socket) {
  int ret, more;
  size_t sz;

  sz = sizeof(more);
  ret = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &sz);
  assertv(ret == 0, "Attempt to get 'more' flag failed\n");
  return more != 0;
}

/*
 * Get a multipart message in a single buffer.  Concatenate the
 * pieces, with `spc' in between.  End with \0.  Return the size.
 */

public int zh_collect_multi(void *socket, char *buf, int bufsz, char *spc) {
 int used = 0,
     left = bufsz-1,
     nspc = strlen(spc);

  do {
    int sz;

    sz = zh_get_msg(socket, 0, left-nspc, &buf[used]);
    assertv(sz >= 0, "Get message error\n");
    used += sz;
    left -= sz;
    if( !zh_any_more(socket) )
      break;
    bcopy(spc, &buf[used], nspc);
    used += nspc;
    left -= nspc;
  } while( left >= 0 );
  buf[used] ='\0';
  return used;
}

/*
 * Send a ZMG message via a socket.  If size is zero, send an empty
 * frame, and buf can be NULL.  If ZMQ_SNDMORE is given as flag, this
 * is part of a multipart message.
 */

public int zh_put_msg(void *socket, int flags, size_t size, void *buf) {
  zmq_msg_t  msg;
  int ret;

  assertv(size >= 0, "Put message with -ve size %d\n", size);
  ret = zmq_msg_init_size(&msg, size);
  assertv(ret == 0, "Message init failed\n");
  if( size ) {
    assertv(buf != NULL, "Non-zero size and NULL buf\n");
    bcopy(buf, zmq_msg_data(&msg), size);
  }
  return zmq_msg_send(&msg, socket, flags);
}

/*
 * Send an n-frame message via a socket given an argument list of strings.
 */

public int zh_put_multi(void *socket, int n, ...) {
  va_list ap;
  int ret;

  va_start(ap,n);
  while( n-- > 0 ) {
    char *next = va_arg(ap, char *);
    int sz = strlen(next);
    ret = zh_put_msg(socket, (n==0? 0 : ZMQ_SNDMORE), sz, next);
    if( ret < 0 )
      return ret;
  }
  va_end(ap);
  return 0;
}

/*
 * Send an object (pointer) via a ZMQ socket.
 */

public void send_object_ptr(void *s, void *o) {
  if(o == NULL) {
    int ret = zh_put_msg(s, 0, 0, NULL);
    assertv(ret==0, "Object Message sent with wrong size %d (not %d)\n", ret, sizeof(void *));
  }
  else {
    int ret = zh_put_msg(s, 0, sizeof(void *), o);
    assertv(ret==sizeof(void *), "Object Message sent with wrong size %d (not %d)\n", ret, sizeof(void *));
  }
}

/*
 * Receive an object (pointer) via a ZMQ socket.
 */

public int recv_object_ptr(void *s, void **op) {
  int ret = zh_get_msg(s, 0, sizeof(void *), (void *)op);
  if(ret != 0)
    assertv(ret==sizeof(void *), "Object Message received with with wrong size %d (not %d)\n", ret, sizeof(void *));
  return ret;
}
