/* Phase 4c — Sim engine path delay integration test */
#include "libqsim/simulator.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, passed = 0;
#define CHECK(cond, msg) do { tests++; if (cond) { passed++; } else { printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while(0)

/* Test 1: Basic rise delay */
static void test_rise_delay(void) {
    const char *src =
        "module test;\n"
        "  input a;\n"
        "  output b;\n"
        "  specify\n"
        "    (a => b) = (5, 7);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "compile");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "elaborate");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "sim create");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Drive a=1 (X→1 transition → rise delay=5) */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    /* Process time 0 events (a changes, path delay schedules b at t=5) */
    uir_sim_run(sim, 0);

    /* b should still be X */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "get b at t=0");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "b still X at t=0");
    }

    /* Advance to t=10 (processes event at t=5) */
    uir_sim_run(sim, 10);

    bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "get b at t=10");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "b is 1 after rise delay");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 2: Fall delay */
static void test_fall_rise(void) {
    const char *src =
        "module test;\n"
        "  input a;\n"
        "  output b;\n"
        "  specify\n"
        "    (a => b) = (5, 7);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "compile t2");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "elab t2");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "sim t2");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Drive a=0 (X→0 transition → fall delay=7) */
    qsim_bit_vector_t *val_0 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_0, 0, QSIM_VAL_0);
    uir_sim_set_signal(sim, "a", val_0);
    qsim_bit_vector_free(val_0);

    /* Let sim process a=0 and the path delay */
    uir_sim_run(sim, 20);

    /* b should now be 0 (after fall delay) */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "get b after fall");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_0, "b is 0 after fall delay");
    }

    /* Now drive a=1 (0→1 → rise delay=5) */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    /* Process the event but don't go past the rise delay */
    uir_sim_run(sim, 3);

    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_0, "b still 0 before rise");
    }

    /* Advance past the rise delay */
    uir_sim_run(sim, 10);

    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "b is 1 after second rise");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 3: Path delay in submodule instance */
static void test_submodule_path(void) {
    const char *src_buf =
        "module buf(input a, output b);\n"
        "  specify\n"
        "    (a => b) = (3, 4);\n"
        "  endspecify\n"
        "endmodule\n";
    const char *src_top =
        "module top;\n"
        "  wire x, y;\n"
        "  buf u1(.a(x), .b(y));\n"
        "  initial begin\n"
        "    x = 0;\n"
        "    #1 x = 1;\n"
        "  end\n"
        "endmodule\n";
    const char *sources[2] = { src_buf, src_top };

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "compile sub");
    if (!cr || !cr->success) return;
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "elab sub");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "sim sub");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Run initial eval: x=0 at t=0, then #1 schedules x=1 at t=1.
     * Path delay on buf: (x => y) = (3,4).
     * x=0 at t=0 → u1.a=0 via port wire → path delay: y=0 at t=4.
     * But #1 also drives x=1 at t=1 → u1.a=1 at t=1 → path delay: y=1 at t=4.
     * Final at t=5: y=1 (1 arrived at t=4, no more changes). */
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    CHECK(y_val != NULL, "get y");
    if (y_val) {
        qsim_value_t y0 = qsim_bit_get(y_val, 0);
        CHECK(y0.state == QSIM_1, "y is 1 at t=10");
    }

    qsim_bit_vector_t *u1_b = uir_sim_get_signal(sim, "u1.b");
    CHECK(u1_b != NULL, "get u1.b");
    if (u1_b) {
        qsim_value_t u1b0 = qsim_bit_get(u1_b, 0);
        CHECK(u1b0.state == QSIM_1, "u1.b is 1");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 4: Conditional path — condition false → path does not propagate */
static void test_conditional_path_false(void) {
    const char *src =
        "module test;\n"
        "  input a, en;\n"
        "  output b;\n"
        "  specify\n"
        "    if (en) (a => b) = (5, 7);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "cond_false compile");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "cond_false elab");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "cond_false sim");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Drive en=0 (condition false) */
    qsim_bit_vector_t *val_0 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_0, 0, QSIM_VAL_0);
    uir_sim_set_signal(sim, "en", val_0);
    qsim_bit_vector_free(val_0);

    /* Drive a=1 */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    uir_sim_run(sim, 20);

    /* b should still be X because condition (en) is false */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "cond_false get b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "b still X when condition false");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 5: Conditional path — condition true → path propagates */
static void test_conditional_path_true(void) {
    const char *src =
        "module test;\n"
        "  input a, en;\n"
        "  output b;\n"
        "  specify\n"
        "    if (en) (a => b) = (5, 7);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "cond_true compile");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "cond_true elab");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "cond_true sim");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Drive en=1 (condition true) */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "en", val_1);
    qsim_bit_vector_free(val_1);

    /* Drive a=1 */
    val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    uir_sim_run(sim, 0);

    /* b should still be X at t=0 (before rise delay=5) */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "cond_true get b t=0");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "b still X at t=0 before delay");
    }

    uir_sim_run(sim, 10);

    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "b is 1 after rise delay (condition true)");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 6: Arithmetic specparam — specparam T = 5 + 3 → delay = 8 */
static void test_arithmetic_specparam(void) {
    const char *src =
        "module test;\n"
        "  input a;\n"
        "  output b;\n"
        "  specify\n"
        "    specparam T = 5 + 3;\n"
        "    (a => b) = (T, T);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "arith compile");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "arith elab");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "arith sim");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Drive a=1 */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    uir_sim_run(sim, 0);

    /* b still X at t=0 */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "arith get b t=0");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "b still X at t=0");
    }

    /* Advance only to t=4 — before delay=8 */
    uir_sim_run(sim, 4);
    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "b still X at t=4 (< delay=8)");
    }

    /* Advance past delay=8 */
    uir_sim_run(sim, 10);
    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "b is 1 after arithmetic delay=8");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Test 7: Full path with z/x delays */
static void test_full_path_zx(void) {
    const char *src =
        "module test;\n"
        "  input a;\n"
        "  output b;\n"
        "  specify\n"
        "    (a *> b) = (5, 7, 9, 11);\n"
        "  endspecify\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "zx compile");
    if (!cr || !cr->success) return;

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "zx elab");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "zx sim");
    if (!sim) { qsim_compile_result_free(cr); return; }

    /* Basic: drive a=1 → rise delay=5 */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "zx get b after rise");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "b is 1 after rise delay=5");
    }

    /* Drive a=0 → fall delay=7 */
    qsim_bit_vector_t *val_0 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_0, 0, QSIM_VAL_0);
    uir_sim_set_signal(sim, "a", val_0);
    qsim_bit_vector_free(val_0);

    uir_sim_run(sim, 20);

    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_0, "b is 0 after fall delay=7");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

int main(void) {
    setbuf(stdout, NULL);
    test_rise_delay();
    test_fall_rise();
    test_submodule_path();
    test_conditional_path_false();
    test_conditional_path_true();
    test_arithmetic_specparam();
    test_full_path_zx();
    printf("Path delay: %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
