#

#ifndef _PARAM_H
#define _PARAM_H

#include <stdio.h>
#include <stdint.h>

typedef const struct {
  const char *t_name;
  int	      t_size;
  const char *t_scan;
  const char *t_show;
}
  param_type;

#define PARAM_TYPE(name) param_type_ ## name
#define PARAM_TYPE_DECL(name,size,scan,show) param_type PARAM_TYPE(name)[] = { "<" #name ">" , sizeof(size), scan, show, }
#define PARAM_TYPE_EXPORT(name) extern param_type PARAM_TYPE(name)[];

PARAM_TYPE_EXPORT(bool);
PARAM_TYPE_EXPORT(int16);
PARAM_TYPE_EXPORT(int32);
PARAM_TYPE_EXPORT(int64);
PARAM_TYPE_EXPORT(double);
PARAM_TYPE_EXPORT(string);

typedef struct
{ const char	*p_name;		/* Name of this parameter */
  const char	*p_str;			/* String value for this parameter */
  void          *p_val;			/* Location where value is to be stored */
  param_type	*p_type;		/* Type of the parameter, for value conversion */
  int		 p_source;		/* Possible sources of the values */
  const char    *p_gloss;		/* Explanation of this parameter */
  int		 p_dyn;			/* If true, free and replace str on push */
}
  param_t;

#define	PARAM_SRC_ENV	0x1
#define	PARAM_SRC_ARG	0x2
#define	PARAM_SRC_CMD	0x4

extern int set_param_value(param_t *, char *);
extern param_t *find_param_by_name(const char *, int, param_t [], int);
extern int set_param_from_env(char *[], param_t [], int);
extern int set_params_from_string(char *, param_t [], int);
extern int set_opt_params_from_string(char *, param_t [], int);
extern int get_param_str(param_t *, const char **);
// extern void param_brief_usage(char *, int, param_t [], int);
// extern void param_option_usage(FILE *, int, param_t [], int);
// extern const char *pop_param_value(param_t *);
extern void reset_param(param_t *);
extern void setval_param(param_t *, void **);
extern int assign_param(param_t *);
extern int assign_all_params(param_t *, int);
extern int param_value_to_string(param_t *, const char **);
extern int arg_defaults_from_params(void **, int, param_t [], int);
extern int arg_results_to_params(void **, param_t [], int);
extern void debug_params(FILE *, param_t [], int);

#endif /* _PARAM_H */
