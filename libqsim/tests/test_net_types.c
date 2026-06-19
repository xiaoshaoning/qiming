/* Phase 3: Net type resolution tests — standalone binary.
 * Verifies parser and sim engine handle wand, wor, tri0, tri1, supply0, supply1. */
#include "libqsim/verilog_parser.h"
#include "libqsim/uir.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir_sim.h"
#include "libqsim/value.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

static int has_signal(uir_design_unit_t *unit, const char *name, uir_signal_type_t type)
{
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (strcmp(unit->signals[i]->name, name) == 0 &&
            unit->signals[i]->sig_type == type)
            return 1;
    }
    return 0;
}

/* ── Parser test ── */

static void test_parse_net_types(void)
{
    const char *src =
        "module m(input a, b);\n"
        "  wand [3:0] w;\n"
        "  wor [7:0] x;\n"
        "  tri0 y;\n"
        "  tri1 z;\n"
        "  supply0 vcc;\n"
        "  supply1 gnd;\n"
        "  uwire single;\n"
        "  tri t;\n"
        "  triand ta;\n"
        "  trior tr;\n"
        "  trireg trr;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("net_types.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(has_signal(r.unit, "w", UIR_SIG_WAND), "w is wand");
    mu_assert(has_signal(r.unit, "x", UIR_SIG_WOR), "x is wor");
    mu_assert(has_signal(r.unit, "y", UIR_SIG_TRI0), "y is tri0");
    mu_assert(has_signal(r.unit, "z", UIR_SIG_TRI1), "z is tri1");
    mu_assert(has_signal(r.unit, "vcc", UIR_SIG_SUPPLY0), "vcc is supply0");
    mu_assert(has_signal(r.unit, "gnd", UIR_SIG_SUPPLY1), "gnd is supply1");
    mu_assert(has_signal(r.unit, "single", UIR_SIG_UWIRE), "single is uwire");
    mu_assert(has_signal(r.unit, "t", UIR_SIG_TRI), "t is tri");
    mu_assert(has_signal(r.unit, "ta", UIR_SIG_TRIAND), "ta is triand");
    mu_assert(has_signal(r.unit, "tr", UIR_SIG_TRIOR), "tr is trior");
    mu_assert(has_signal(r.unit, "trr", UIR_SIG_TRIREG), "trr is trireg");
}

/* ── Sim resolution tests ── */

static qsim_compile_result_t *compile_verilog(const char *src)
{
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;
    return qsim_compile(&input);
}

static uir_sim_context_t *create_sim(qsim_compile_result_t *cr)
{
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    if (!elab->success) return NULL;
    uir_elab_result_free(elab);
    return uir_sim_create(units, 1);
}

static void test_wand_resolution(void)
{
    const char *src =
        "module wand_test;\n"
        "  wand [7:0] w;\n"
        "  reg [7:0] a, b;\n"
        "  assign w = a;\n"
        "  assign w = b;\n"
        "endmodule\n";
    qsim_compile_result_t *cr = compile_verilog(src);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");
    uir_sim_context_t *sim = create_sim(cr);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *allX = qsim_bit_vector_from_state(8, QSIM_X);
    qsim_bit_vector_t *wv;

    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wand: 0 dominates");

    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wand: both 1 -> 1");

    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wand: Z transparent");

    uir_sim_set_signal(sim, "a", allX);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allX), "wand: X when no 0");

    uir_sim_set_signal(sim, "a", allZ);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allZ), "wand: both Z -> Z");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all0);
    qsim_bit_vector_free(all1);
    qsim_bit_vector_free(allZ);
    qsim_bit_vector_free(allX);
}

static void test_wor_resolution(void)
{
    const char *src =
        "module wor_test;\n"
        "  wor [7:0] w;\n"
        "  reg [7:0] a, b;\n"
        "  assign w = a;\n"
        "  assign w = b;\n"
        "endmodule\n";
    qsim_compile_result_t *cr = compile_verilog(src);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");
    uir_sim_context_t *sim = create_sim(cr);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *allX = qsim_bit_vector_from_state(8, QSIM_X);
    qsim_bit_vector_t *wv;

    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wor: 1 dominates");

    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wor: both 0 -> 0");

    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wor: Z transparent");

    uir_sim_set_signal(sim, "a", allX);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allX), "wor: X when no 1");

    uir_sim_set_signal(sim, "a", allZ);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allZ), "wor: both Z -> Z");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all0);
    qsim_bit_vector_free(all1);
    qsim_bit_vector_free(allZ);
    qsim_bit_vector_free(allX);
}

static void test_tri0_pull(void)
{
    const char *src =
        "module tri0_test;\n"
        "  tri0 [7:0] t;\n"
        "  reg [7:0] d;\n"
        "  assign t = d;\n"
        "endmodule\n";
    qsim_compile_result_t *cr = compile_verilog(src);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");
    uir_sim_context_t *sim = create_sim(cr);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *tv;

    uir_sim_set_signal(sim, "d", all1);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all1), "tri0: follows non-Z driver");

    uir_sim_set_signal(sim, "d", allZ);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all0), "tri0: pulls to 0 when undriven");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all0);
    qsim_bit_vector_free(all1);
    qsim_bit_vector_free(allZ);
}

static void test_tri1_pull(void)
{
    const char *src =
        "module tri1_test;\n"
        "  tri1 [7:0] t;\n"
        "  reg [7:0] d;\n"
        "  assign t = d;\n"
        "endmodule\n";
    qsim_compile_result_t *cr = compile_verilog(src);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");
    uir_sim_context_t *sim = create_sim(cr);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *tv;

    uir_sim_set_signal(sim, "d", all0);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all0), "tri1: follows non-Z driver");

    uir_sim_set_signal(sim, "d", allZ);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all1), "tri1: pulls to 1 when undriven");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all0);
    qsim_bit_vector_free(all1);
    qsim_bit_vector_free(allZ);
}

static void test_supply0_supply1(void)
{
    const char *src =
        "module supply_test;\n"
        "  supply0 vcc;\n"
        "  supply1 gnd;\n"
        "  reg d;\n"
        "  assign vcc = d;\n"
        "  assign gnd = d;\n"
        "endmodule\n";
    qsim_compile_result_t *cr = compile_verilog(src);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");
    uir_sim_context_t *sim = create_sim(cr);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *v;

    v = uir_sim_get_signal(sim, "vcc");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, all0), "supply0 initial = 0");

    v = uir_sim_get_signal(sim, "gnd");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, all1), "supply1 initial = 1");

    uir_sim_set_signal(sim, "d", all1);
    uir_sim_run(sim, 5);

    v = uir_sim_get_signal(sim, "vcc");
    mu_assert(qsim_bit_vector_eq(v, all0), "supply0 unchanged by assign");
    v = uir_sim_get_signal(sim, "gnd");
    mu_assert(qsim_bit_vector_eq(v, all1), "supply1 unchanged by assign");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all0);
    qsim_bit_vector_free(all1);
}

/* ── Runner ── */

int mu_tests_run = 0;
int mu_tests_passed = 0;
int mu_tests_failed = 0;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Qiming Net Type Tests (Phase 3)\n");
    printf("========================================\n\n");

    printf("[Parser]\n");
    mu_run_test(test_parse_net_types);

    printf("[Simulator - Resolution]\n");
    mu_run_test(test_wand_resolution);
    mu_run_test(test_wor_resolution);
    mu_run_test(test_tri0_pull);
    mu_run_test(test_tri1_pull);
    mu_run_test(test_supply0_supply1);

    printf("\n");
    mu_summary();

    return mu_tests_failed > 0 ? 1 : 0;
}
