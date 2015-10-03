#
#ifndef _ERROR_H
#define _ERROR_H

/*
 * Macros for error reporting and logging.
 */

/* Fatal is used only in the main thread and process */

#define FATAL_ERROR(fmt, ...) do {					\
    fprintf(stderr, "%s: Error -- " fmt, program , ## __VA_ARGS__ );	\
  } while(0)

/*
 * Warning and Log are used in all threads.  In the MAIN thread they
 * just use fprintf(), otherwise they must use the log socket
 */

#ifdef MAIN_THREAD

#define WARNING(thd,fmt, ...) do { if(verbose >= 0) {			\
    fprintf(stderr, "Warn[" #thd "]: " fmt , ## __VA_ARGS__ );		\
  } } while(0)

#define LOG(thd,lvl,fmt, ...) do { if(verbose >= (lvl)) {		\
    fprintf(stderr, "Log[" #thd "]: " fmt , ## __VA_ARGS__ );		\
  } } while(0)

#else

#define WARNING(thd,fmt, ...) do {					\
  import int verbose;							\
  import strbuf  logbuf_ ## thd;					\
  import void   *logskt_ ## thd;					\
									\
  if(verbose >= 0) {							\
    strbuf l = logbuf_ ## thd;						\
    strbuf_clear(l);							\
    strbuf_printf(l, "Warn[" #thd "]: ");				\
    strbuf_appendf(l, fmt , ## __VA_ARGS__ );				\
    zh_put_msg(logskt_ ## thd, 0, strbuf_used(l), strbuf_string(l));	\
  } } while(0)

#define LOG(thd,lvl,fmt, ...) do {					\
  import int verbose;							\
  import strbuf  logbuf_ ## thd;					\
  import void   *logskt_ ## thd;					\
									\
  if(verbose >= 0) {							\
    strbuf l = logbuf_ ## thd;						\
    strbuf_clear(l);							\
    strbuf_printf(l, "Log[" #thd "]: ");				\
    strbuf_appendf(l, fmt , ## __VA_ARGS__ );				\
    zh_put_msg(logskt_ ## thd, 0, strbuf_used(l), strbuf_string(l));	\
  } } while(0)

#endif

#endif /* _ERROR_H */
