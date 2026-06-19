#ifndef LIBQSIM_VHDL_PKG_REGISTRY_H
#define LIBQSIM_VHDL_PKG_REGISTRY_H

#include <stddef.h>

/* Kinds of built-in VHDL functions from standard packages */
typedef enum {
    VHDL_FUNC_RISING_EDGE,    /* rising_edge(signal s: std_logic) return boolean */
    VHDL_FUNC_FALLING_EDGE,   /* falling_edge(signal s: std_logic) return boolean */
    VHDL_FUNC_TO_INTEGER,     /* to_integer(signed/unsigned) return integer */
    VHDL_FUNC_TO_SIGNED,      /* to_signed(int, size) return signed */
    VHDL_FUNC_TO_UNSIGNED,    /* to_unsigned(int, size) return unsigned */
    VHDL_FUNC_RESIZE,         /* resize(signed/unsigned, size) return signed/unsigned */
} vhdl_builtin_func_kind_t;

typedef struct {
    const char *name;          /* function name as it appears in VHDL source */
    const char *package;       /* "ieee.std_logic_1164" or "ieee.numeric_std" */
    vhdl_builtin_func_kind_t kind;
    int min_args;
    int max_args;
} vhdl_builtin_func_t;

/* Look up a function name in the built-in registry. Returns NULL if not found. */
const vhdl_builtin_func_t *vhdl_lookup_builtin_func(const char *name);

/* Check if a name is a known built-in function (returns 1 if yes, 0 if no). */
int vhdl_is_builtin_func(const char *name);

#endif /* LIBQSIM_VHDL_PKG_REGISTRY_H */
