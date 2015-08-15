#

#define NCHAN	8

typedef struct
{ short channels[NCHAN];
} samples_t;

/* Shared globals */

extern void       *zmq_main_ctx;

extern param_t	   globals[];
extern const int   n_global_params;

extern int	   debug_level;

extern int	   reader_thread_running,
		   writer_thread_running;

extern int	   tmpdir_dirfd;
extern const char *tmpdir_path;

/* Common definitions */

#define LOG_SOCKET	"inproc://Main-LOG"
