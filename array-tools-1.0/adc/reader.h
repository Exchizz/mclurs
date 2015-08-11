#

/*
 * The main function for thr reader thread
 */

extern int   verify_reader_params(param_t [], int);
extern void *reader_main(void *);

#define READER_CMD_ADDR	"inproc://Reader-CMD"
#define READER_POS_ADDR	"inproc://Reader-POS"

#define READER_MSGBUF	16		/* Size of internal command frame */

extern int      inter_sample_ns;
extern uint64_t capture_start_time; /* [ns from epoch] */
