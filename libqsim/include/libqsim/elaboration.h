#ifndef LIBDSIM_ELABORATION_H
#define LIBDSIM_ELABORATION_H

#include "libqsim/uir.h"
#include "libqsim/simulator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of elaboration: list of elaborated design units + diagnostics */
typedef struct {
    uir_design_unit_t **units;
    size_t unit_count;
    char **diagnostics;
    size_t diag_count;
    qsim_recovery_t **recoveries;  /* parallel to diagnostics, NULL if no recovery */
    int success;
} uir_elab_result_t;

/* Elaborate a set of design units:
 *   - Bind instances to referenced module definitions
 *   - Validate port connections
 *   - Build flat hierarchical signal table
 *   - Resolve sensitivity lists
 */
uir_elab_result_t *uir_elaborate(uir_design_unit_t **units, size_t count);

/* Free elaboration result (does NOT free the design units) */
void uir_elab_result_free(uir_elab_result_t *result);

/* Hierarchical signal lookup: parse "top.inst.signal" and walk instance tree */
uir_node_t *uir_find_signal_hier(uir_design_unit_t *top, const char *hier_path);

/* Expand generate constructs in all design units.
 * Called after elaboration, before simulation context creation.
 * Walks each unit's generate list and expands for/if/case into
 * actual instances, signals, processes, and assigns. */
void uir_expand_generates(uir_design_unit_t **units, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_ELABORATION_H */
