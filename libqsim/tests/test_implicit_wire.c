/* Reproduction test for implicit wire width bug.
 * Undeclared wires should inherit width from assignment context,
 * not default to 1-bit and silently truncate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/simulator.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"

int main(void) {
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] a;\n"
        "  assign a = 8'hAB;\n"
        "  /* implicit_wire is undeclared — should be 8 bits wide from RHS */\n"
        "  assign implicit_wire = a;\n"
        "endmodule\n"
    };

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    if (!cr || !cr->success) {
        printf("COMPILE FAILED\n");
        if (cr) {
            for (size_t i = 0; i < cr->diag_count; i++)
                printf("  %s\n", cr->diagnostics[i].message);
        }
        return 1;
    }

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    if (!elab || !elab->success) {
        printf("ELAB FAILED\n");
        return 1;
    }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    if (!sim) { printf("SIM CREATE FAILED\n"); return 1; }

    uir_sim_run(sim, 0);

    /* Check implicit_wire signal */
    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "implicit_wire");
    if (!v) v = uir_sim_get_signal(sim, "test.implicit_wire");

    printf("implicit_wire: ");
    if (v) {
        printf("width=%u ", v->width);
        for (uint32_t i = 0; i < v->width; i++) {
            qsim_value_t b = qsim_bit_get(v, i);
            printf("%c", b.state == QSIM_1 ? '1' : b.state == QSIM_0 ? '0' : 'X');
        }
        if (v->width == 8) {
            printf("  PASS (8-bit)\n");
        } else {
            printf("  FAIL (expected 8-bit, got %u-bit)\n", v->width);
        }
    } else {
        printf("NOT FOUND (implicit wire was not created)\n");
    }

    /* Dump all signal names */
    int sc = uir_sim_get_signal_count(sim);
    printf("Signals (%d):\n", sc);
    for (int i = 0; i < sc; i++)
        printf("  [%d] %s\n", i, uir_sim_get_signal_name(sim, i));

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    return 0;
}
