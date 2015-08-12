#

/*
 * Public definitions for a int16_t argument consistent with argtable2.
 */

#ifndef _ARGTAB_INT16_H
#define _ARGTAB_INT16_H

#include <stdint.h>
#include <argtable2.h>

struct arg_int16
{
  struct arg_hdr hdr;      /* The mandatory argtable header struct */
  int count;               /* Number of matching command line arguments found */
  int16_t *data;           /* Array of matching command line argument data  */
};

struct arg_int16* arg_int640(const char* shortopts, const char* longopts, const char *datatype,
                             const char *glossary);

struct arg_int16* arg_int641(const char* shortopts, const char* longopts, const char *datatype,
                             const char *glossary);

struct arg_int16* arg_int64n(const char* shortopts, const char* longopts, const char *datatype,
                             int mincount, int maxcount, const char *glossary);

#endif /* _ARGTAB_INT16_H */
