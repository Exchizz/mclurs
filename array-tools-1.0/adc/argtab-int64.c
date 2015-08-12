#

#include <argtable2.h>
#include <stdlib.h>
#include "argtab-int64.h"

#define private static

/*
 * Callback functions for this argument class -- based closely on the
 * argtable2 examples.
 */

/* Private error codes for this type */
enum {OK=0,EMINCOUNT,EMAXCOUNT,EBADVALUE};

/* Reset the parent argument count */
private void resetfn(struct arg_int64 *parent)
{
  parent->count=0;
}

/* Read a value from an argument string */
private int scanfn(struct arg_int64 *parent, const char *argval)
{
  long long int val;
  char *left;

  if (parent->count == parent->hdr.maxcount)
    {
      /* maximum number of arguments exceeded */
      return EMAXCOUNT;
    }
  if (!argval)
    {
      /* an argument with no argument value was given. */
      /* This happens when an optional argument value was invoked. */
      /* leave parent argument value unaltered but still count the argument. */
      parent->count++;
      return 0;
    } 

  /* Try to convert the argument string */
  val = strtoll(argval, &left, 0);

  if (*left == '\0') {
    /* success; value was scanned ok, and it is within our desired range.  */
    parent->data[parent->count++] = val;
    return OK;
  }

  /* failure; command line string was not a valid integer */
  return EBADVALUE;
}

/* Check for presence of required arguments */
private int checkfn(struct arg_int64 *parent)
{
  /* return EMINCOUNT if the minimum argment count has not been satisfied */
  if( parent->count < parent->hdr.mincount )
    return EMINCOUNT;
  else
    return OK;
}

/* Error handler function */
private void errorfn(struct arg_int64 *parent, FILE *fp, int errorcode, const char *argval, const char *progname)
{
  const char *shortopts = parent->hdr.shortopts;
  const char *longopts  = parent->hdr.longopts;
  const char *datatype  = parent->hdr.datatype;

  /* make argval NULL safe */
  argval = argval ? argval : "";

  fprintf(fp,"%s: ",progname);
  switch(errorcode)
    {
    case EMINCOUNT:
      /* We expected more arg_int64 arguments than we received. */
      fputs("missing option \"",fp);
      arg_print_option(fp,shortopts,longopts,datatype,"\"\n");
      break;

    case EMAXCOUNT:
      /* We received more arg_int64 arguments than we expected. */
      fputs("excess option \"",fp);
      arg_print_option(fp,shortopts,longopts,argval,"\"\n");
      break;

    case EBADVALUE:
      /* An arg_int64 option was given with an invalid value */
      fprintf(fp,"invalid argument \"%s\" to option ",argval);
      arg_print_option(fp,shortopts,longopts,datatype,"\n");
      break;
    }
}

/* Generic constructor for an arg_int64 structure */
struct arg_int64* arg_int64n(const char* shortopts, const char* longopts,
			     const char *datatype,
                             int mincount, int maxcount, const char *glossary) {
  int bytes;
  struct arg_int64 *ret;

  bytes = sizeof(struct arg_int64) + maxcount*sizeof(uint64_t);
  ret = (struct arg_int64 *)calloc(1, bytes);
  if( ret ) {
    ret->hdr.flag      = ARG_HASVALUE;
    ret->hdr.shortopts = shortopts;
    ret->hdr.longopts  = longopts;
    ret->hdr.datatype  = datatype ? datatype : "<[u]int64_t>";
    ret->hdr.glossary  = glossary;
    ret->hdr.mincount  = mincount;
    ret->hdr.maxcount  = maxcount;
    ret->hdr.parent    = ret;
    ret->hdr.resetfn   = (arg_resetfn *)resetfn;
    ret->hdr.scanfn    = (arg_scanfn *)scanfn;
    ret->hdr.checkfn   = (arg_checkfn *)checkfn;
    ret->hdr.errorfn   = (arg_errorfn *)errorfn;
    ret->count = 0;
    ret->data = (int64_t *)&ret[1];
  }
  return ret;
}

/* Special case: 0 or 1 arguments */
struct arg_int64* arg_int640(const char* shortopts, const char* longopts,
			     const char *datatype,  const char *glossary) {

  return arg_int64n(shortopts, longopts, datatype, 0, 1, glossary);
}

/* Special case: exactly 1 argument */
struct arg_int64* arg_int641(const char* shortopts, const char* longopts,
			     const char *datatype,  const char *glossary) {

  return arg_int64n(shortopts, longopts, datatype, 1, 1, glossary);
}

