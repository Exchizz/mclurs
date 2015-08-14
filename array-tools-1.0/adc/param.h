#

#ifndef _PARAM_H
#define _PARAM_H

#include <stdio.h>
#include <stdint.h>

typedef const struct {
  const char *t_name;
  int	      t_size;
}
  param_type;

/* Needed to make sizeof() work in the macros below */
typedef int       bool;
typedef char     *string;
typedef uint64_t  int64;
typedef uint32_t  int32;
typedef uint16_t  int16;

#define PARAM_TYPE(name) param_type_ ## name
#define PARAM_TYPE_DECL(name) param_type PARAM_TYPE(name)[] = { "<" #name ">" , sizeof(name) }
#define PARAM_TYPE_EXPORT(name) extern param_type PARAM_TYPE(name)[];

PARAM_TYPE_EXPORT(bool);
PARAM_TYPE_EXPORT(int16);
PARAM_TYPE_EXPORT(int32);
PARAM_TYPE_EXPORT(int64);
PARAM_TYPE_EXPORT(double);
PARAM_TYPE_EXPORT(string);

typedef struct
{ char		*p_name;			/* Name of this parameter */
  char		*p_str;				/* String value for this parameter */
  void          *p_val;				/* Location where value is to be stored */
  param_type	*p_type;			/* Type of the parameter, for value conversion */
  int		 p_source;			/* Possible sources of the values */
  char          *p_gloss;			/* Explanation of this parameter */
  int		 p_ftop;			/* If true, free and replace str on push */
}
  param_t;

#define	PARAM_SRC_ENV	0x1
#define	PARAM_SRC_ARG	0x2
#define	PARAM_SRC_CMD	0x4

extern int push_param_value(param_t *, char *);
extern param_t *find_param_by_name(const char *, int, param_t [], int);
extern int push_param_from_env(char *[], param_t [], int);
extern int get_param_value(param_t *, char **);
// extern void param_brief_usage(char *, int, param_t [], int);
// extern void param_option_usage(FILE *, int, param_t [], int);
extern char *pop_param_value(param_t *);
extern int assign_param_values(param_t *, int);
extern int arg_defaults_from_params(void **, int, param_t [], int);
extern int arg_results_to_params(void **, param_t [], int);
extern void debug_params(FILE *, param_t [], int);

#endif /* _PARAM_H */
