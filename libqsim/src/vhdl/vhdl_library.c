#include "libqsim/vhdl_library.h"
#include <stdlib.h>
#include <string.h>

void vhdl_library_registry_init(vhdl_library_registry_t *reg) {
    if (!reg) return;
    reg->entries = NULL;
    reg->count = 0;
    reg->cap = 0;
}

void vhdl_library_registry_destroy(vhdl_library_registry_t *reg) {
    if (!reg) return;
    free(reg->entries);
    reg->entries = NULL;
    reg->count = 0;
    reg->cap = 0;
}

int vhdl_library_register(vhdl_library_registry_t *reg,
    const char *lib, const char *pkg, uir_design_unit_t *unit, int is_builtin)
{
    if (!reg || !lib || !pkg) return -1;
    if (reg->count >= reg->cap) {
        size_t new_cap = reg->cap ? reg->cap * 2 : 16;
        vhdl_package_entry_t *new_e = realloc(reg->entries, new_cap * sizeof(vhdl_package_entry_t));
        if (!new_e) return -1;
        reg->entries = new_e;
        reg->cap = new_cap;
    }
    vhdl_package_entry_t *e = &reg->entries[reg->count];
    strncpy(e->library, lib, sizeof(e->library) - 1);
    e->library[sizeof(e->library) - 1] = '\0';
    strncpy(e->package_name, pkg, sizeof(e->package_name) - 1);
    e->package_name[sizeof(e->package_name) - 1] = '\0';
    e->unit = unit;
    e->is_builtin = is_builtin;
    reg->count++;
    return 0;
}

vhdl_package_entry_t *vhdl_library_find(vhdl_library_registry_t *reg,
    const char *lib, const char *pkg)
{
    if (!reg || !lib || !pkg) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].library, lib) == 0 &&
            strcmp(reg->entries[i].package_name, pkg) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

void vhdl_library_register_builtins(vhdl_library_registry_t *reg) {
    if (!reg) return;
    /* ieee.std_logic_1164 — provides rising_edge, falling_edge, std_logic type */
    vhdl_library_register(reg, "ieee", "std_logic_1164", NULL, 1);
    /* ieee.numeric_std — provides unsigned, signed, to_integer, etc. */
    vhdl_library_register(reg, "ieee", "numeric_std", NULL, 1);
    /* ieee.std_logic_textio */
    vhdl_library_register(reg, "ieee", "std_logic_textio", NULL, 1);
    /* std.standard — the VHDL standard package (always visible) */
    vhdl_library_register(reg, "std", "standard", NULL, 1);
}
