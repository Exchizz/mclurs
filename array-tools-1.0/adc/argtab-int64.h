#

/*
 * Public definitions for a int64_t argument consistent with argtable2.
 */

#ifndef _ARGTAB_INT64_H
#define _ARGTAB_INT64_H

#include <stdint.h>
#include <argtable2.h>

struct arg_64b
{
  struct arg_hdr hdr;      /* The mandatory argtable header struct */
  int count;               /* Number of matching command line arguments found */
  int64_t *data;           /* Array of matching command line argument data  */
};

struct arg_64b* arg_64b0(const char* shortopts, const char* longopts, const char *datatype,
                             const char *glossary);

struct arg_64b* arg_64b1(const char* shortopts, const char* longopts, const char *datatype,
                             const char *glossary);

struct arg_64b* arg_64bn(const char* shortopts, const char* longopts, const char *datatype,
                             int mincount, int maxcount, const char *glossary);

#endif /* _ARGTAB_INT64_H */
