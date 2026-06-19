#include "libqsim/vhdl_pkg_registry.h"
#include <string.h>

/* Table of built-in VHDL functions from standard IEEE packages.
 * These are recognized by name at parse time (for call-vs-index disambiguation)
 * and dispatched at simulation time via eval_vhdl_builtin_func() in uir_sim.c. */
static const vhdl_builtin_func_t s_builtin_funcs[] = {
    /* ieee.std_logic_1164 */
    {"rising_edge",   "ieee.std_logic_1164", VHDL_FUNC_RISING_EDGE,   1, 1},
    {"falling_edge",  "ieee.std_logic_1164", VHDL_FUNC_FALLING_EDGE,  1, 1},

    /* ieee.numeric_std */
    {"to_integer",    "ieee.numeric_std",    VHDL_FUNC_TO_INTEGER,    1, 1},
    {"to_signed",     "ieee.numeric_std",    VHDL_FUNC_TO_SIGNED,     2, 2},
    {"to_unsigned",   "ieee.numeric_std",    VHDL_FUNC_TO_UNSIGNED,   2, 2},
    {"resize",        "ieee.numeric_std",    VHDL_FUNC_RESIZE,        2, 2},
};

static const size_t s_builtin_func_count =
    sizeof(s_builtin_funcs) / sizeof(s_builtin_funcs[0]);

const vhdl_builtin_func_t *vhdl_lookup_builtin_func(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < s_builtin_func_count; i++) {
        if (strcmp(s_builtin_funcs[i].name, name) == 0)
            return &s_builtin_funcs[i];
    }
    return NULL;
}

int vhdl_is_builtin_func(const char *name)
{
    return vhdl_lookup_builtin_func(name) != NULL;
}
