#

#define NCHAN	8

typedef struct { 
  int16_t channels[NCHAN];
} samples_t;

/* Shared globals */

extern void       *zmq_main_ctx;

extern param_t	   globals[];
extern const int   n_global_params;

extern int	   verbose;

extern int	   die_die_die_now;

extern int	   tmpdir_dirfd;
extern const char *tmpdir_path;

/* Common definitions */

#define LOG_SOCKET	"inproc://Main-LOG"

#define MSGBUFSIZE	8192
