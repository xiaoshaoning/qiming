#ifndef LIBQSIM_VHDL_LIBRARY_H
#define LIBQSIM_VHDL_LIBRARY_H

#include "libqsim/uir.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Entry for a registered VHDL package (ieee.std_logic_1164, work.mypkg, etc.) */
typedef struct {
    char library[64];
    char package_name[64];
    uir_design_unit_t *unit;  /* NULL for builtins without source */
    int is_builtin;
} vhdl_package_entry_t;

/* Linear-scan registry of known VHDL packages */
typedef struct {
    vhdl_package_entry_t *entries;
    size_t count, cap;
} vhdl_library_registry_t;

void vhdl_library_registry_init(vhdl_library_registry_t *reg);
void vhdl_library_registry_destroy(vhdl_library_registry_t *reg);

/* Register a package. lib="ieee", pkg="std_logic_1164", unit may be NULL for
 * builtins, is_builtin=1 for pre-defined packages without source. */
int vhdl_library_register(vhdl_library_registry_t *reg,
    const char *lib, const char *pkg, uir_design_unit_t *unit, int is_builtin);

/* Find a package. Returns NULL if not found. */
vhdl_package_entry_t *vhdl_library_find(vhdl_library_registry_t *reg,
    const char *lib, const char *pkg);

/* Register the standard IEEE builtin packages (ieee.std_logic_1164,
 * ieee.numeric_std) as is_builtin entries. */
void vhdl_library_register_builtins(vhdl_library_registry_t *reg);

#ifdef __cplusplus
}
#endif

#endif /* LIBQSIM_VHDL_LIBRARY_H */
