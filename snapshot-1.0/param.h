#

#define	MAX_PARAM_VALS	5

typedef enum
{ PARAM_INT16,			/* Parameter is a short integer */
  PARAM_INT32,			/* Parameter is a standard integer */
  PARAM_INT64,			/* Parameter is a long integer */
  PARAM_BOOL,			/* Parameter is a boolean */
  PARAM_STRING,			/* Parameter is a string */
  PARAM_FLOAT,			/* Parameter is a real number (float) */
  PARAM_DOUBLE			/* Parameter is a real number (double) */
}
  param_type;

typedef struct
{ char		*p_name;			/* Name of this parameter */
  int		 p_nv;				/* Number of values recorded -- we take the last */
  char		*p_vals[MAX_PARAM_VALS];	/* Stack of values for this parameter;  top is the one chosen */
  param_type	 p_type;			/* Type of the parameter, for value conversion */
  int		 p_source;			/* Possible sources of the values */
  char          *p_gloss;			/* Explanation of this parameter */
  int		 p_ftop;			/* If true, free and replace top on push */
}
  param_t;

#define	PARAM_SRC_ENV	0x1
#define	PARAM_SRC_ARG	0x2
#define	PARAM_SRC_CMD	0x4

extern int push_param_value(param_t *, char *);
extern param_t *find_param_by_name(char *, int, param_t [], int);
extern int push_param_from_env(char *[], param_t [], int);
extern int get_param_value(param_t *, char **);
extern int assign_value(param_type, char *, void *);
extern int getopt_long_params(int, char *[], const char *, param_t [], int, int (*)(int, char *));
extern void param_option_usage(FILE *, int, param_t [], int);
extern char *pop_param_value(param_t *);
