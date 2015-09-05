#

#ifndef _ARGTAB_HELPER_H
#define _ARGTAB_HELPER_H

#include "assert.h"
#include "param.h"

/*
 * Simplify definition of command line parsing tables 
 *
 *   The argument syntax definition, in the form of calls to argtable2
 * constructors, goes between the BEGIN_CMD_SYNTAX() and APPLY_CMD_DEFAULTS()
 * macro calls, enclosed in { } as an initialiser list (comma-separated).  The
 * default assignments are placed between { } as a statement list, following
 * APPLY_CMD_DEFAULTS() and finishing with END_CMD:SYNTAX().  The result is that
 * the default assignments are placed inside the constructor built by the whole
 * macro set.
 *
 * Keeping the { } outside the macros, and following the END_CMD_SYNTAX() call
 * with a semicolon (null statement) allows emacs font-lock to keep up :-))
 */

#define BEGIN_CMD_SYNTAX(name) void **arg_make_ ## name () { void **ret, *argtable[] =

#define APPLY_CMD_DEFAULTS(name)	;				\
									\
  if( arg_nullcheck(argtable) ) {					\
    arg_freetable(argtable, sizeof(argtable)/sizeof(void *));		\
    return NULL;							\
  }									\
									\
  ret = malloc(sizeof(argtable));					\
  if( !ret ) {								\
    arg_freetable(argtable, sizeof(argtable)/sizeof(void *));		\
    return NULL;							\
  }									\
  do

#define INCLUDE_PARAM_DEFAULTS(ps,nps)					\
  int rv = arg_defaults_from_params(argtable,				\
				    sizeof(argtable)/sizeof(void *),	\
				    (ps), (nps));			\
  assertv(rv == 0, "Argtable has no end mark\n");

#define END_CMD_SYNTAX(name)						\
									\
  while(0);								\
									\
  int n = sizeof(argtable)/sizeof(void *);				\
  while( n-- > 0 ) {							\
    ret[n] = argtable[n];						\
  }									\
  return ret;								\
}

#endif /* _ARGTAB_HELPER_H */
