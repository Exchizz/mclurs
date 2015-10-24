#

/*
 * Copyright c. John Hallam <sw@j.hallam.dk> 2015.
 *
 * This program is free software licensed under the terms of the GNU General
 * Public License, either version 3 of the License, or (at your option) any
 * later version.  See http://www.gnu.org/licenses/gpl.txt for details.
 */

#include "general.h"

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

PARAM_TYPE_DECL(bool,   int,      "%d",   "%d");
PARAM_TYPE_DECL(int16,  uint16_t, "%hi",  "%hu");
PARAM_TYPE_DECL(int32,  uint32_t, "%li",  "%u");
PARAM_TYPE_DECL(int64,  uint64_t, "%Li",  "%llu");
PARAM_TYPE_DECL(double, double,   "%lg",   "%g");
PARAM_TYPE_DECL(string, char *,   NULL,   "%s");

/*
 * Reset the str and val pointers in a param_t structure.  Free strings
 * as needed, but assume that a string val that is dynamic is dealt with
 * by the caller.
 */

public void reset_param(param_t *p) {
  if(p->p_type == PARAM_TYPE(string)) { /* Special case of dynamic string in two places */
    if( p->p_val && *(char **)p->p_val == p->p_str) { /* String copy is in both places */
      p->p_dyn = 0;                                  /* Ignore dynamic:  caller is responsible */
    }
  }
  if(p->p_dyn)
    free( (void *)p->p_str );
  p->p_str = NULL;
  p->p_dyn = 0;
  p->p_setby = PARAM_SRC_DEF;
  if(p->p_val)
    p->p_val = NULL;
}

/*
 * Set the val pointer for a param.
 */

public void setval_param(param_t *p, void **val) {
  p->p_val = val;
}

/*
 * Push an extra value for the given parameter onto its value stack,
 * if there is room.  If p_dyn is set, the last (top) value is an
 * allocated copy; free it and overwrite the slot.  Otherwise the
 * value is a permanent buffer, so just push.
 */

public int set_param_value(param_t *p, char *v) {
  /*  fprintf(stderr, "Pushing value %s\n", v); */
  if( !p->p_source ) {
    errno = EPERM;
    return -1;
  }
  if( p->p_dyn && p->p_str ) {
    if(p->p_val && *(const char **)p->p_val == p->p_str)
      *(const char **)p->p_val = NULL;
    free((void *)p->p_str);
    p->p_str = NULL;
  }
  p->p_str = v;
  return 0;
}

/*
 * Locate the parameter descriptor in the ps array for the named
 * parameter, if it exists.
 */

public param_t *find_param_by_name(const char *name, int sz, param_t ps[], int nps) {
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

public int set_param_from_env(char *env[], param_t ps[], int nps) {
  int i;

  if( !env )
    return 0;

  for(i=0; i<nps; i++) {
    char **e, *p;
    int   sz;

    if( !(ps[i].p_source & PARAM_SRC_ENV) )             /* Only look for params with environment source */
      continue;
    for(e=env; p=*e; e++) {
      for(sz=0; *p && *p != '='; p++, sz++);    /* Find the = */
      if( !strncasecmp(ps[i].p_name, *e, sz) ) {        /* Unless true, it's not this one */
        if( ps[i].p_name[sz] )                  /* If true, there is more name left over:  not this one */
          continue;
        if( set_param_value(&ps[i], (*p ? p+1 : p)) < 0 )
          return -1;
	param_setby(&ps[i], PARAM_SRC_ENV);
      }
    }
  }
  return 0;
}

/*
 * Read the presented string and look for Name=Value where Name is the
 * name of a parameter.  If found, push a pointer to the value.  The
 * cmd string is assumed NUL-terminated after the value.
 */

public int set_param_from_cmd(char *cmd, param_t ps[], int nps) {
  char *s;
  param_t *p;

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
  if( !*s++ ) {           /* If *s non-zero, step over the = */
    errno = EINVAL;       /* Name=Value string has no '=Value' part */
    return -1;
  }
  if( set_param_value(p, s) < 0 )
    return -1;
  param_setby(p, PARAM_SRC_CMD);
  return 0;
}

/*
 * Given a string comprising a set of space/comma/semicolon separated
 * Name=Value pairs, instantiate parameters from them.  Use strtok_r
 * to parse the string, which alters the input string by replacing
 * separators with NUL characters.  Each string returned by strtok_r
 * is a single NUL-terminated Name=Value element.  On error, return
 * the negative of the position in the string of the current token
 * start.
 */

private int do_set_params_from_string(char *str, int opt, param_t ps[], int nps) {  
  char *save;
  char *cur;
  int   done;
  int   ret;

  errno = 0;			/* All OK so far :-)) */
  
  /* Initialise the strtok_r scan: skip to space */
  cur = strtok_r(str, " \t", &save);
  if( cur == NULL ) {
    errno = EINVAL;
    return opt? 0 : -1;         /* If parameters are optional, may succeed here for empty */
  }

  /* First parameter Name=Value should come next */
  done = 0;
  while( (cur = strtok_r(NULL, " \t,;", &save)) != NULL ) {
    if( !isalpha(*cur) ) {
      errno = EBADMSG;
      return str-cur;
    }
    ret = set_param_from_cmd(cur, ps, nps);
    if( ret < 0 )
      return str-cur;
    done++;
  }
  if( !done && !opt )
    errno = EINVAL;
  return (done || opt)? 0 : -1;
}

/* Parameters are compulsory */

public int set_params_from_string(char *str, param_t ps[], int nps) {
  return do_set_params_from_string(str, 0, ps, nps);
}

/* String may be empty of parameters */

public int set_opt_params_from_string(char *str, param_t ps[], int nps) {
  return do_set_params_from_string(str, 1, ps, nps);
}

/*
 * Assign a parameter value, i.e. parse its string value and write the result to
 * the location pointed to by the val pointer, which must be of the correct kind.
 */

public int assign_param(param_t *p) {
  if(p == NULL) {
    errno = EINVAL;
    return -1;
  }
  if( !p->p_val ) {             /* Nowhere to put value */
    errno = EFAULT;
    return -1;
  }

  param_type *pt = p->p_type;
  if(pt == PARAM_TYPE(bool)) {  /* Special cases for booleans */
    const char *s = p->p_str;   /* May be NULL, for a boolean (== false) */

    if( !*s || !strncasecmp(s, "false", 6) || !strncasecmp(s, "no", 3) || !strncasecmp(s, "off", 4) ) {
      *(int *)p->p_val = 0;
      return 0;
    }
    if( !strncasecmp(s, "true", 5) || !strncasecmp(s, "yes", 4) || !strncasecmp(s, "on", 3) ) {
      *(int *)p->p_val = 1;
      return 0;
    }
  }

  if( !p->p_str )               /* No value to put anywhere */
    return 0;

  if(pt == PARAM_TYPE(string)) { /* Special case for strings -- no conversion needed */
    *(const char **)p->p_val = p->p_str;
    return 0;
  }

  // fprintf(stderr, "Scan param %s with str %s to %p using %s\n",
  //      p->p_name, p->p_str, p->p_val, pt->t_scan);
  return sscanf(p->p_str, pt->t_scan, p->p_val) == 1? 0 : -1;
}

/*
 * Scan the parameter table and copy out the values present, converting strings to
 * appropriate types and installing them in the external addresses where provided.
 */

public int assign_all_params(param_t ps[], int nps) {
  int n;

  for(n=0; n<nps; n++) {
    param_t *p = &ps[n];

    if(assign_param(p) < 0)
      return -1-n;
  }
  return 0;
}

/*
 * Same as above but only for parameters sourced from commands.
 */

int assign_cmd_params(param_t ps[], int nps) {
  int n;

  for(n=0; n<nps; n++) {
    param_t *p = &ps[n];

    if( p->p_source & PARAM_SRC_CMD ) {
      if(assign_param(p) < 0)
        return -n-1;
    }
  }
  return 0;
}

/*
 * Retrieve the string value of the parameter and store it in the
 * buffer pointed to by vp, which must be suitable to receive it.
 */

public int get_param_str(param_t *p, const char **vp) {
  const char *v = NULL;
  if( !p->p_str  ) {
    if( p->p_type == PARAM_TYPE(bool) ) {
      *vp = "false";
      return 0;
    }
    errno = EINVAL;
    return -1;
  }
  v = p->p_str;         /* The string value for the parameter */
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

private param_t *arg_param_match(const char *a, param_t ps[], int nps) {
  param_t *p, *ret;
  const char *ap;

  if(a == NULL)                 /* There are no long option names */
    return NULL;
  ret = NULL;
  ap = a;                       /* First option name starts here */
  for(ap=a; *a; a=ap) {

    while(*ap && *ap != ',') ap++; /* Skip to end of (first) option name */

    p = find_param_by_name(a, ap-a, ps, nps);
    if(p == NULL)               /* No match for that name */
      continue;

    if(ret && ret != p) {       /* Multiple matches! */
      errno = EBADSLT;
      return NULL;
    }

    ret = p;                    /* At least one match found */
    if(*ap == ',') ap++;        /* Skip a comma, if more to come */
  }
  return ret;
}


/* ASSUME that the count and 'data' values in every argxxx follow the hdr directly */
/* IF THAT IS TRUE, then we can use the ->count and ->data members of ANY arg_xxx */
#define ARG_COUNT(a)    (((struct arg_int *)(a))->count)
#define ARG_DATA(a)     ((void *)((struct arg_int *)(a))->ival)

/*
 * Install defaults into an argtable from matching parameter structures.  This assumes
 * internal knowledge of the arg_hdr structures to determine the relevant parameter
 * structure etc. to use.  The parameter's string value is converted using the arg_hdr
 * structure's scan function and is pre-installed in the arg_xxx structure, which is then
 * reset.
 */

public int arg_defaults_from_params(void **argtable, int nargs, param_t ps[], int nps) {
  struct arg_hdr **ate = (struct arg_hdr **)&argtable[nargs-1]; /* The arg_end structure slot */

  if( !((*ate)->flag & ARG_TERMINATOR) ) {
    errno = EINVAL;
    return -1;
  }

  struct arg_hdr **atp;
  for(atp=(struct arg_hdr **)argtable; atp<ate; atp++) {
    struct arg_hdr *a = *atp;

    param_t *p = arg_param_match(a->longopts, ps, nps);

    if(p == NULL)               /* No parameter matched this argument */
      continue;
    /* Must be an ARG parameter, or coding problem */
    assertv( (p->p_source & PARAM_SRC_ARG), "Param %s not ARG sourced\n", p->p_name );
    if( !p->p_str )             /* No string value, no default */
      continue;

    //    fprintf(stderr, "Found parameter %s with addr %p, str %s\n", p->p_name, p->p_val, p->p_str);

    /* Copy the parameter's value to the arg structure -- fake an argument parse */
    (*a->resetfn)(a->parent);   /* Reset the counter;  init the structure */
    int ret = (*a->scanfn)(a->parent, p->p_str);
    /* Else value compatibility error:  abort */
    assertv(ret == 0, "Param %s str %s does not pass arg scanfn\n", p->p_name, p->p_str);
    ret = (*a->checkfn)(a->parent);
    /* Else value compatibility error:  abort */
    assertv(ret == 0, "Param %s str %s fails arg checkfn\n", p->p_name, p->p_str);
    ARG_COUNT(a) = 0;           /* This was a default */
  }

  return 0;
}

/*
 * Copy the results from a parsed argtable back to the locations pointed to by their
 * matching param structures; the matching structures are determined as for the
 * arg_defaults_from_params() routine above.  Unfortunately, there is no way to know what
 * kind of value the argxxx structure describes -- we have to assume that the parameter
 * knows the type (and therefore the size to copy).
 *
 * We also copy the value back into the parameter string form -- this might entail some loss
 * of precision for real number values.
 */

public int arg_results_to_params(void **argtable, param_t ps[], int nps) {
  struct arg_hdr **ate =  (struct arg_hdr **)argtable;

  while( (*ate) && !((*ate)->flag & ARG_TERMINATOR) ) ate++; /* Find the end */
  if( !(*ate) || !((*ate)->flag & ARG_TERMINATOR) ) {
    errno = EINVAL;
    return -1;
  }

  struct arg_hdr **atp;
  for(atp=(struct arg_hdr **)argtable; atp<ate; atp++) {
    struct arg_hdr *a = *atp;
    void *av;

    if( ARG_COUNT(a) == 0 )     /* There is no command-line argument value */
      continue;

    param_t *p = arg_param_match(a->longopts, ps, nps);

    if(p == NULL)               /* No parameter matched this argument */
      continue;
    if( !p->p_val )             /* Nowhere to put the value */
      continue;

    av = ARG_DATA(a) + (ARG_COUNT(a)-1)*p->p_type->t_size;
    memcpy(p->p_val, av, p->p_type->t_size);
    param_setby(p, PARAM_SRC_ARG);
    
    /*
     * This one copy back is tricky...  If *p->p_val is not already the
     * same as p->p_str, it must have come from a static string from
     * argument or environment, since only the assign code changes the
     * val content and it copies the str.  Therefore we copy back the
     * val content and turn off the free-it flag.
     *
     * On the other hand, if p->p_str is in fact *p->p_val, there is
     * nothing further to do.
     */
    if(p->p_type == PARAM_TYPE(string)) {
      const char *v = *(char **)p->p_val;
      if(v != p->p_str) {
        if(p->p_dyn)
          free((void *)p->p_str);
        p->p_dyn = 0;
        p->p_str = v;
      }
      continue;                 /* We are done, in this case */
    }
    
    if(p->p_dyn)                /* Free the old str value if necessary */
      free((void *)p->p_str);
    int ret = param_value_to_string(p, &p->p_str);
    p->p_dyn = 1;               /* The new value is a dynamic string */
    assertv(ret >=0, "Update of parameter %s str from val for arg %d failed\n", 
            p->p_name, ate-atp+1); 
  }
  return 0;
}

#undef ARG_COUNT
#undef ARG_DATA

/*
 * Generate part of a usage message based on the parameter structures
 */

public void param_option_usage(FILE *f, int spc, param_t ps[], int nps) {
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

public void param_brief_usage(char *buf, int sz, param_t ps[], int nps) {
  int i,
    used = 0,
    rest = sz-1;

  for(i=0; i<nps && rest > 0; i++) {
    param_t *p = &ps[i];
    int n;
    if( !(p->p_source & PARAM_SRC_ARG) )
      continue;
    n = snprintf(&buf[used], rest, "[ --%s <%s> ] ", p->p_name, p->p_type);
    used += n;
    rest -= n;
  }
  buf[used] ='\0';
}

/*
 * Convert a parameter value and store in a dynamically allocated string
 */

#define LOCALBUF_SIZE 64

public int param_value_to_string(param_t *p, const char **s) {
  param_type *pt = p->p_type;
  char buf[LOCALBUF_SIZE];
  int  used = 0;

  if( !p->p_val )               /* Nowhere to get the value from */
    return 0;

  /* These cases are systematically treatable */
  if(pt == PARAM_TYPE(string)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(char **)p->p_val);
  }
  if(pt == PARAM_TYPE(bool)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(int *)p->p_val);
  }
  if(pt == PARAM_TYPE(int16)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(uint16_t *)p->p_val);
  }
  if(pt == PARAM_TYPE(int32)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(uint32_t *)p->p_val);
  }
  if(pt == PARAM_TYPE(int64)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(uint64_t *)p->p_val);
  }
  if(pt == PARAM_TYPE(double)) {
    used = snprintf(&buf[0], LOCALBUF_SIZE-1, pt->t_show, *(double *)p->p_val);
  }
  if( !(*s = strndup(&buf[0], used)) )
    return -1;

  return used;
}

public void debug_params(FILE *fp, param_t ps[], int nps) {
  int i;

  for(i=0; i<nps; i++) {
    param_t *p = &ps[i];
    param_type *pt = p->p_type;

    fprintf(fp, "Parameter '%s': type %s addr %p str %p='%s'",
            p->p_name, pt->t_name, p->p_val, p->p_str, p->p_str);
    if(p->p_val) {
      fprintf(fp, " val '");
      if(pt == PARAM_TYPE(bool)) {
        fprintf(fp, pt->t_show, *(int *)p->p_val);
      }
      if(pt == PARAM_TYPE(int16)) {
        fprintf(fp, pt->t_show, *(uint16_t *)p->p_val);
      }
      if(pt == PARAM_TYPE(int32)) {
        fprintf(fp, pt->t_show, *(uint32_t *)p->p_val);
      }
      if(pt == PARAM_TYPE(int64)) {
        fprintf(fp, pt->t_show, *(uint64_t *)p->p_val);
      }
      if(pt == PARAM_TYPE(string)) {
        fprintf(fp, pt->t_show, *(char **)p->p_val);
      }
      if(pt == PARAM_TYPE(double)) {
        fprintf(fp, pt->t_show, *(double *)p->p_val);
      }
    }
    fprintf(fp, "'\n");
  }
}
