#

#ifndef _TIDY_H
#define _TIDY_H

typedef struct {
  void *b_data;
  int   b_bytes;
}
  block;

extern void *tidy_main(void *);

#define TIDY_SOCKET "inproc://snapshot-TIDY"

#endif /* _TIDY_H */
