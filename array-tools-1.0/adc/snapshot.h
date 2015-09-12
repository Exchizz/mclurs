#

#include "general.h"

/* MAYBE THIS SHOULD BE IN THE ADC MODULE */
#define NCHAN	8

typedef struct { 
  int16_t channels[NCHAN];
} samples_t;

/* Shared globals */

export void       *zmq_main_ctx;

export param_t	   globals[];
export const int   n_global_params;

export int	   verbose;

export int	   die_die_die_now;

export int	   tmpdir_dirfd;
export const char *tmpdir_path;

/* Common definitions */

#define LOG_SOCKET	"inproc://Main-LOG"

#define MSGBUFSIZE	8192
