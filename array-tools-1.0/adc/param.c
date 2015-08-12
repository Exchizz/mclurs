#

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <ctype.h>
#include "param.h"

/*
 * Parameter types, represented by constant strings...
 */

PARAM_TYPE_DECL(bool);
PARAM_TYPE_DECL(int16);
PARAM_TYPE_DECL(int32);
PARAM_TYPE_DECL(int64);
PARAM_TYPE_DECL(double);
PARAM_TYPE_DECL(string);

/*
 * Push an extra value for the given parameter onto its value stack,
 * if there is room.  If p_ftop is set, the last (top) value is an
 * allocated copy; free it and overwrite the slot.  Otherwise the
 * value is a permanent buffer, so just push.
 */

int push_param_value(param_t *p, char *v) {
  /*  fprintf(stderr, "Pushing value %s\n", v); */
  if( !p->p_source ) {
    errno = EPERM;
    return -1;
  }
  if( p->p_ftop && p->p_nv ) {
    free(p->p_vals[--p->p_nv]);
    p->p_vals[p->p_nv] = NULL;
  }
  if( p->p_nv < MAX_PARAM_VALS) {
    p->p_vals[p->p_nv++] = v;
    return 0;
  }
  errno = E2BIG;
  return -1;
}

/*
 * Pop the top value from a parameter and return it as a dynamic
 * string, i.e. a buffer allocated using malloc().
 */

char *pop_param_value(param_t *p) {
  if( !p->p_nv ) {
    errno = EINVAL;
    return NULL;
  }
  if( p->p_ftop ) {		/* Top is already a dynamic string */
    p->p_ftop = 0;		/* But only top can be dynamic (see 'push') */
    return p->p_vals[--p->p_nv];
  }
  return strdup( p->p_vals[--p->p_nv] ); /* Make a dynamic copy of the static value */
}

/*
 * Locate the parameter descriptor in the ps array for the named
 * parameter, if it exists.
 */

param_t *find_param_by_name(char *name, int sz, param_t ps[], int nps) {
  int i;

  /*  fprintf(stderr, "Looking for name "); fwrite(name, sz, 1, stderr); fprintf(stderr, "\n"); */
  errno = 0;
  for(i=0; i<nps; i++)
    if( !strncmp(name, ps[i].p_name, sz) )
      return &ps[i];
  errno = EBADSLT;
  return NULL;
}

/*
 * Scan the parameters in the ps array and check whether environment
 * variables provide values for any of them.  The environment variable
 * name must match the parameter using case insensitive matching.  The
 * environment variable's value is pushed onto the parameter's value
 * stack, if there is room for it.
 */

int push_param_from_env(char *env[], param_t ps[], int nps) {
  int i;

  if( !env )
    return 0;

  for(i=0; i<nps; i++) {
    char **e, *p;
    int   sz;

    if( !(ps[i].p_source & PARAM_SRC_ENV) )		/* Only look for params with environment source */
      continue;
    for(e=env; p=*e; e++) {
      for(sz=0; *p && *p != '='; p++, sz++);	/* Find the = */
      if( !strncasecmp(ps[i].p_name, *e, sz) ) {	/* Unless true, it's not this one */
	if( ps[i].p_name[sz] )			/* If true, there is more name left over:  not this one */
	  continue;
	if( push_param_value(&ps[i], (*p ? p+1 : p)) < 0 )
	  return -1;
      }
    }
  }
  return 0;
}

/*
 * Read the presented string and look for Name=Value where Name is the
 * name of a parameter.  If found, push a copy of the Value.
 */

int push_param_from_cmd(char *cmd, param_t ps[], int nps) {
  char *s, *v, *e;
  param_t *p;
  int ret;

  /*  fprintf(stderr, "Working on cmd %s\n", cmd); */
  if( !cmd )
    return 0;
  for(s=cmd; *s && *s != '='; s++);
  p = find_param_by_name(cmd, s-cmd, ps, nps);
  if( !p )
    return -1;
  if( !(p->p_source & PARAM_SRC_CMD) ) {
    errno = EPERM;
    return -1;
  }
  if( !*s ) {			/* Name=Value string has no '=Value' part */
    s = "UNDEF";
  }
  else {
    if(*s == '=') s++;
    for(e=s; *e && !isspace(*e) && *e != ','; e++);
    v = malloc(e-s+1);
    if( !v )
      return -1;
  }
  bcopy(s, v, e-s);
  v[e-s] = '\0';
  ret = push_param_value(p, v);
  if( ret < 0 )
    return ret;
  p->p_ftop = 1;		/* Top value is now a malloc'd copy */
  return 0;
}

/*
 * Given a string comprising a set of space/comma separated Name=Value pairs,
 * instantiate parameters from them.
 */

int push_params_from_string(char *str, param_t ps[], int nps) {
  char *cur   = str;
  char *first = str;
  int   ret;
  int   n=1;

  /*  fprintf(stderr, "Push param from %s\n", str);*/
  while(*cur) {
      if( !isalpha(*first) ) {
	errno = EBADMSG;
	return -n;
      }
      /* Skip over the first parameter specifier */
      for(first=cur; *cur && !isspace(*cur) && *cur != ','; cur++);
      /*      fprintf(stderr, "Pushing from %s\n", first); */
      ret = push_param_from_cmd(first, ps, nps);
      if( ret < 0 )
	return -n;
      if(*cur == ',')
	cur++;
      for( ; *cur && isspace(*cur); cur++); /* Skip whitespace */
      first = cur;
      n++;
  }
  return 0;
}

/*
 * Retrieve a value from a string and copy it to a suitable buffer.
 */

int assign_value(param_type t, char *s, void *vp) {
  errno = 0;
  switch(t) {
  default:			/* Unknown type */
    errno = ENOSYS;
    return -1;

  case PARAM_INT16:
    if( sscanf(s, " %hi", vp) == 1 )
      break;
    return -1;

  case PARAM_INT32:
    if( sscanf(s, " %li", vp) == 1 )
      break;
    return -1;

  case PARAM_INT64:
    if( sscanf(s, " %Li", vp) == 1 )
      break;
    return -1;

  case PARAM_BOOL:
    if( !*s || !strncasecmp(s, "false", 6) || !strncasecmp(s, "no", 3) || !strncasecmp(s, "off", 4) ) {
      *(int *)vp = 0;
      break;
    }
    if( !strncasecmp(s, "true", 5) || !strncasecmp(s, "yes", 4) || !strncasecmp(s, "on", 3) ) {
      *(int *)vp = 1;
      break;
    }
    errno = EINVAL;
    return -1;

  case PARAM_STRING:
    *(char **)vp = s;
    return 0;

  case PARAM_FLOAT:
    if( sscanf(s, " %g", vp) == 1 )
      break;
    return -1;

  case PARAM_DOUBLE:
    if( sscanf(s, " %lg", vp) == 1 )
      break;
    return -1;
  }
  return 0;
}

/*
 * Retrieve the final value of the parameter and store it in the
 * buffer pointed to be vp, which must besuitable to receive it.
 */

int get_param_value(param_t *p, char **vp) {
  char *v = NULL;
  if( !p->p_nv  ) {
    if( p->p_type == PARAM_BOOL ) {
      *vp = "false";
      return 0;
    }
    errno = EINVAL;
    return -1;
  }
  v = p->p_vals[p->p_nv-1];	/* The final value for the parameter */
  if( !v ){
    errno = EINVAL;
    return -1;
  }
  *vp = v;
  return 0;
 }

/*
 * Generate option structures to match the parameter descriptions, and
 * scan the process arguments using getopt_long to instantiate the
 * parameter values.  We assume that the array of option structures is
 * of the same size as the param_t array.
 */

static void params_to_options(param_t ps[], int nps, struct option os[]) {
  int i, j;

  for(i=0,j=0; i<nps; i++) {
    if( !(ps[i].p_source & PARAM_SRC_ARG) )
      continue;
    os[j].name = ps[i].p_name;
    os[j].has_arg = (ps[i].p_type == PARAM_BOOL? optional_argument : required_argument);
    os[j].flag = NULL;
    os[j].val  = 1;
    j++;
  }
}

/*
 * Process arguments as getopt_long, except generate long arguments
 * automagically from parameters.  The routine processes all arguments
 * by calling the supplied handle() routine for non-parameter options.
 * Parameter options are dealt with by pushing their value on to their
 * stack.
 *
 * Returns 0 on success, -1 on failure; leaves argv[optind] as first
 * non-option argument.  optstring can be "" if there are no short
 * options.  The only long options are the parameters.
 */

int getopt_long_params(int argc, char *argv[], const char *optstring, param_t ps[], int nps,
		       int (*handle)(int, char *)) {
  struct option *os = calloc(nps+1, sizeof(struct option));
  int ret;
      
  if( os == NULL )
    return -1;
  params_to_options(ps, nps, os); /* Set up the option structures */
  while(1) {
    int pn = 0;

    ret = getopt_long(argc, argv, optstring, os, &pn);
    if( ret < 0 ) {		/* Last option found */
      ret = 0;
      break;
    }
    if( ret == 1 ) {		/* Got a long option */
      param_t *p = &ps[pn];
      if( (ret = push_param_value(p, optarg)) < 0 )
	break;
    }
    else {
      ret = (*handle)(ret, optarg);	/* Got a standard option, call tha handler */
      if( ret < 0 )
	break;
    }
  }
  free(os);
  return ret < 0? ret : 0;
}

/*
 * Generate part of a usage message based on the parameter structures
 */

void param_option_usage(FILE *f, int spc, param_t ps[], int nps) {
  int i;
  char *buf = malloc(spc+1);

  for(i=0; i<spc; i++) buf[i] = ' ';
  buf[spc] = '\0';
  for(i=0; i<nps; i++) {
    param_t *p = &ps[i];
    char *type = "NULL";
    if( !(p->p_source & PARAM_SRC_ARG) )
      continue;
    switch(p->p_type) {
    case PARAM_INT16:  type="int16"; break;
    case PARAM_INT32:  type="int32"; break;
    case PARAM_INT64:  type="int64"; break;
    case PARAM_BOOL:   type="bool"; break;
    case PARAM_STRING: type="string"; break;
    case PARAM_FLOAT:  type="float"; break;
    case PARAM_DOUBLE: type="double"; break;
    default:	   type="unknown"; break;
    }
    fprintf(f, "%s--%s <%s> : %s\n", buf, p->p_name, type, p->p_gloss);
  }
}

void param_brief_usage(char *buf, int sz, param_t ps[], int nps) {
  int i,
    used = 0,
    rest = sz-1;

  for(i=0; i<nps && rest > 0; i++) {
    param_t *p = &ps[i];
    char *type = "NULL";
    int n;
    if( !(p->p_source & PARAM_SRC_ARG) )
      continue;
    switch(p->p_type) {
    case PARAM_INT16:  type="int16"; break;
    case PARAM_INT32:  type="int32"; break;
    case PARAM_INT64:  type="int64"; break;
    case PARAM_BOOL:   type="bool"; break;
    case PARAM_STRING: type="string"; break;
    case PARAM_FLOAT:  type="float"; break;
    case PARAM_DOUBLE: type="double"; break;
    default:	   type="unknown"; break;
    }
    n = snprintf(&buf[used], rest, "[ --%s <%s> ] ", p->p_name, type);
    used += n;
    rest -= n;
  }
  buf[used] ='\0';
}

void debug_params(param_t ps[], int nps) {
  int i;

  for(i=0; i<nps; i++) {
    param_t *p = &ps[i];
    char *s = NULL;

    get_param_value(p, &s);
    fprintf(stderr, "Parameter '%s' has value [%d]='%s'\n",
	    p->p_name, p->p_nv, (s? s : "NULL"));
  }
}
