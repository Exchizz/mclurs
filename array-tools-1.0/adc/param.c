#

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "assert.h"
#include <ctype.h>
#include "argtab.h"
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
  if( p->p_ftop && p->p_str ) {
    if(p->p_val == p->p_str)
      p->p_val = NULL;
    free(p->p_str);
    p->p_str = NULL;
  }
  p->p_str = v;
  return 0;
}

/*
 * Copy the string value from a parameter and return it as a dynamic
 * string, i.e. a buffer allocated using malloc().
 */

char *pop_param_value(param_t *p) {
  if( !p->p_str ) {
    errno = EINVAL;
    return NULL;
  }
  return strdup( p->p_str ); /* Make a dynamic copy of the value */
}

/*
 * Locate the parameter descriptor in the ps array for the named
 * parameter, if it exists.
 */

param_t *find_param_by_name(const char *name, int sz, param_t ps[], int nps) {
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
 * environment variable's value replaces the parameter's value, if any.
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
 * Scan the parameter table and copy out the values present, converting strings to
 * appropriate types and installing them in the external addresses where provided.
 */

int assign_param_values(param_t ps[], int nps) {
  int n, done;

  done = 0;
  for(n=0; n<nps; n++) {
    param_t *p = &ps[n];

    if(!p->p_val)		/* Nothing to do: nowhere to store result */
      continue;

    if(p->p_type == PARAM_TYPE(bool)) {
      char *s = p->p_str;	/* May be NULL, for a boolean (== false) */

      if( !*s || !strncasecmp(s, "false", 6) || !strncasecmp(s, "no", 3) || !strncasecmp(s, "off", 4) ) {
	*(int *)p->p_val = 0;
	done++;
	continue;
      }
      if( !strncasecmp(s, "true", 5) || !strncasecmp(s, "yes", 4) || !strncasecmp(s, "on", 3) ) {
	*(int *)p->p_val = 1;
	done++;
	continue;
      }
    }

    if(!p->p_str)		/* Parameter has no value, nothing to do */
      continue;

    if(p->p_type == PARAM_TYPE(int16)) {
      if(sscanf(p->p_str, " %hi", p->p_val) == 1)
	done++;
      continue;
    }

    if(p->p_type == PARAM_TYPE(int32)) {
      if(sscanf(p->p_str, " %li", p->p_val) == 1)
	done++;
      continue;
    }

    if(p->p_type == PARAM_TYPE(int64)) {
      if(sscanf(p->p_str, " %Li", p->p_val) == 1)
	done++;
      continue;
    }

    if(p->p_type == PARAM_TYPE(string)) {
      *(char **)p->p_val = p->p_str;
      done++;
      continue;
    }

    if(p->p_type == PARAM_TYPE(double)) {
      if(sscanf(p->p_str, " %lg", p->p_val) == 1)
	done++;
      continue;
    }
  }

  return done;
}

/*
 * Retrieve the string value of the parameter and store it in the
 * buffer pointed to be vp, which must be suitable to receive it.
 */

int get_param_value(param_t *p, char **vp) {
  char *v = NULL;
  if( !p->p_str  ) {
    if( p->p_type == PARAM_TYPE(bool) ) {
      *vp = "false";
      return 0;
    }
    errno = EINVAL;
    return -1;
  }
  v = p->p_str;		/* The string value for the parameter */
  if( !v ){
    errno = EINVAL;
    return -1;
  }
  *vp = v;
  return 0;
 }

/*
 * Find parameters that match an argxxx structure.  An arg_xxx structure matches a
 * parameter if (one of) its long name(s) matches a parameter name for which an ARG
 * source has been set.  The long option names are tried in order; only one may match!
 */

static param_t *arg_param_match(const char *a, param_t ps[], int nps) {
  param_t *p, *ret;
  const char *ap;

  if(a == NULL)			/* There are no long option names */
    return NULL;
  ret = NULL;
  ap = a;			/* First option name starts here */
  for(ap=a; *a; a=ap) {

    while(*ap && *ap != ',') ap++; /* Skip to end of (first) option name */

    p = find_param_by_name(a, ap-a, ps, nps);
    if(p == NULL)		/* No match for that name */
      continue;

    if(ret && ret != p) {	/* Multiple matches! */
      errno = EBADSLT;
      return NULL;
    }

    ret = p;			/* At least one match found */
    if(*ap == ',') ap++;	/* Skip a comma, if more to come */
  }
  return ret;
}


/* ASSUME that the count and 'data' values in every argxxx follow the hdr directly */
/* IF THAT IS TRUE, then we can use the ->count and ->data members of ANY arg_xxx */
#define ARG_COUNT(a)	(((struct arg_int *)(a))->count)
#define ARG_DATA(a)	((void *)((struct arg_int *)(a))->ival)

/*
 * Install defaults into an argtable from matching parameter structures.  This assumes
 * internal knowledge of the arg_hdr structures to determine the relevant parameter
 * structure etc. to use.  The parameter's string value is converted using the arg_hdr
 * structure's scan function and is pre-installed in the arg_xxx structure, which is then
 * reset.
 */

int arg_defaults_from_params(void **argtable, int nargs, param_t ps[], int nps) {
  struct arg_hdr **ate = (struct arg_hdr **)&argtable[nargs-1];	/* The arg_end structure slot */
  param_t *endp = &ps[nps];

  if( !((*ate)->flag & ARG_TERMINATOR) ) {
    errno = EINVAL;
    return -1;
  }

  struct arg_hdr **atp;
  for(atp=(struct arg_hdr **)argtable; atp<ate; atp++) {
    struct arg_hdr *a = *atp;

    param_t *p = arg_param_match(a->longopts, ps, nps);

    if(p == NULL)		/* No parameter matched this argument */
      continue;
    /* Must be an ARG parameter, or coding problem */
    assertv( (p->p_source & PARAM_SRC_ARG), "Param %s not ARG sourced\n", p->p_name );
    if( !p->p_str )		/* No string value, no default */
      continue;

    /* Copy the parameter's value to the arg structure -- fake an argument parse */
    (*a->resetfn)(a->parent);	/* Reset the counter;  init the structure */
    int ret = (*a->scanfn)(a->parent, p->p_str);
    /* Else value compatibility error:  abort */
    assertv(ret == 0, "Param %s str %s does not pass arg scanfn\n", p->p_name, p->p_str);
    ret = (*a->checkfn)(a->parent);
    /* Else value compatibility error:  abort */
    assertv(ret == 0, "Param %s str %s fails arg checkfn\n", p->p_name, p->p_str);
    ARG_COUNT(a) = 0;		/* This was a default */
  }

  return 0;
}

/*
 * Copy the results from a parsed argtable back to the locations pointed to by their
 * matching param structures; the matching structures are determined as for the
 * arg_defaults_from_params() routine above.  Unfortunately, there is no way to know what
 * kind of value the argxxx structure describes -- we have to assume that the parameter
 * knows the type (and therefore the size to copy).
 */

int arg_results_to_params(void **argtable, param_t ps[], int nps) {
  struct arg_hdr **ate =  (struct arg_hdr **)argtable;
  param_t *endp = &ps[nps];
  int n = 0;

  while( (*ate) && !((*ate)->flag & ARG_TERMINATOR) ) ate++; /* Find the end */
  if( !(*ate) || !((*ate)->flag & ARG_TERMINATOR) ) {
    errno = EINVAL;
    return -1;
  }

  struct arg_hdr **atp;
  for(atp=(struct arg_hdr **)argtable; atp<ate; atp++) {
    struct arg_hdr *a = *atp;
    n++;
    if( ARG_COUNT(a) == 0 )	/* There is no command-line argument value */
      continue;

    param_t *p = arg_param_match(a->longopts, ps, nps);

    if(p == NULL)		/* No parameter matched this argument */
      continue;
    if( !p->p_val )		/* Nowhere to put the value */
      continue;

    fprintf(stderr, "Param %d %s has argcount %d\n", n, p->p_name, ARG_COUNT(a));

    void *av = ARG_DATA(a) + (ARG_COUNT(a)-1)*p->p_type->t_size;
    memcpy(p->p_val, av, p->p_type->t_size);
  }
  return 0;
}

#undef ARG_COUNT
#undef ARG_DATA

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
    if( !(p->p_source & PARAM_SRC_ARG) )
      continue;
    fprintf(f, "%s--%s <%s> : %s\n", buf, p->p_name, p->p_type, p->p_gloss);
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
    n = snprintf(&buf[used], rest, "[ --%s <%s> ] ", p->p_name, p->p_type);
    used += n;
    rest -= n;
  }
  buf[used] ='\0';
}

void debug_params(FILE *fp, param_t ps[], int nps) {
  int i;

  for(i=0; i<nps; i++) {
    param_t *p = &ps[i];

    fprintf(fp, "Parameter '%s': type %s addr %p str '%s'",
	    p->p_name, p->p_type->t_name, p->p_val, p->p_str);
    if(p->p_val) {
      fprintf(fp, " val ");
      if(p->p_type == PARAM_TYPE(bool)) {
	fprintf(fp, "'%d'", *(int *)p->p_val);
      }
      if(p->p_type == PARAM_TYPE(int16)) {
	fprintf(fp, "'%hu'", *(int16 *)p->p_val);
      }
      if(p->p_type == PARAM_TYPE(int32)) {
	fprintf(fp, "'%u'", *(int32 *)p->p_val);
      }
      if(p->p_type == PARAM_TYPE(int64)) {
	fprintf(fp, "'%llu'", *(int64 *)p->p_val);
      }
      if(p->p_type == PARAM_TYPE(string)) {
	fprintf(fp, "'%s'", *(char **)p->p_val);
      }
      if(p->p_type == PARAM_TYPE(double)) {
	fprintf(fp, "'%lg'", *(double *)p->p_val);
      }
    }
    fprintf(fp, "\n");
  }
}
