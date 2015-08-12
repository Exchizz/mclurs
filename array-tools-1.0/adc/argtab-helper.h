#

#ifndef _ARGTAB_HELPER_H
#define _ARGTAB_HELPER_H

/* Simplify definition of command line parsing tables */

#define BEGIN_CMD_SYNTAX(name) void **arg_ ## name () { void **ret, *argtable[] =

#define END_CMD_SYNTAX(name)	;					\
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
									\
  int n = sizeof(argtable)/sizeof(void *);				\
  while( n-- > 0 ) {							\
    ret[n] = argtable[n];						\
  }									\
  return ret;								\
}

#endif /* _ARGTAB_HELPER_H */
