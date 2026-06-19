#include "test.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"

int main(void) {
    const char *src =
        "module test;\n"
        "  reg [1:0] grant_ff;\n"
        "  initial begin\n"
        "    grant_ff = 2'b01;\n"
        "    #1;\n"
        "    if (grant_ff == 2'b01)\n"
        "      $display(\"PASS: grant_ff==2'b01\");\n"
        "    else\n"
        "      $display(\"FAIL: grant_ff==2'b01 returned false\");\n"
        "  end\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    if (!cr || !cr->success) { printf("COMPILE FAILED\n"); return 1; }

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    if (!elab || !elab->success) { printf("ELAB FAILED\n"); return 1; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    if (!sim) { printf("SIM CREATE FAILED\n"); return 1; }

    /* Run initial eval + #1 delay */
    uir_sim_run(sim, 1);

    /* Check grant_ff value */
    qsim_bit_vector_t *val = uir_sim_get_signal(sim, "grant_ff");
    if (!val) val = uir_sim_get_signal(sim, "test.grant_ff");
    if (val) {
        printf("grant_ff = %s\n", qsim_bit_vector_to_str(val));
        printf("grant_ff[0]=%d grant_ff[1]=%d\n",
               qsim_bit_get(val, 0).state,
               qsim_bit_get(val, 1).state);
    } else {
        printf("grant_ff NOT FOUND\n");
    }

    /* Directly test the comparison with eval_expr */
    /* Create a simple comparison expression manually */
    {
        /* Signal read via session */
        qsim_bit_vector_t *result = uir_sim_get_signal(sim, "grant_ff");
        printf("Direct: grant_ff == 2'b01: ");
        if (result && result->width >= 2 &&
            qsim_bit_get(result, 0).state == QSIM_1 &&
            qsim_bit_get(result, 1).state == QSIM_0) {
            printf("PASS (01)\n");
        } else {
            printf("FAIL (got ");
            if (result) {
                for (uint32_t i = 0; i < result->width; i++) {
                    qsim_value_t b = qsim_bit_get(result, i);
                    printf("%c", b.state == QSIM_1 ? '1' : b.state == QSIM_0 ? '0' : 'X');
                }
            } else {
                printf("NULL");
            }
            printf(")\n");
        }
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
