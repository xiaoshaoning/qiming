/* Regression test for part-select on reg/wire typed signals (Bug #5).
 * Verifies that [hi:lo] and [lo:hi] work in RHS, LHS, continuous assign,
 * blocking assign, nonblocking assign, and equality comparisons.
 *
 * Note: qsim_compile_result must stay alive while uir_sim_context exists,
 * since uir_sim_create stores pointer to units without deep-copying. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/simulator.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"

static int failed = 0;
#define TEST(name) do { printf("  %s ... ", name); fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); failed = 1; } while(0)

typedef struct {
    uir_sim_context_t *sim;
    qsim_compile_result_t *cr;
} test_env_t;

static int setup_test(const char *src, test_env_t *env) {
    const char *sources[] = { src };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    env->cr = qsim_compile(&input);
    if (!env->cr || !env->cr->success) {
        if (env->cr) {
            for (size_t i = 0; i < env->cr->diag_count; i++)
                printf("  DIAG: %s\n", env->cr->diagnostics[i].message);
        }
        return -1;
    }

    uir_design_unit_t **units = (uir_design_unit_t **)env->cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, env->cr->unit_count);
    if (!elab || !elab->success) {
        printf("  ELAB FAILED\n");
        uir_elab_result_free(elab);
        return -1;
    }
    uir_elab_result_free(elab);

    env->sim = uir_sim_create(units, env->cr->unit_count);
    return env->sim ? 0 : -1;
}

static void teardown_test(test_env_t *env) {
    if (env->sim) uir_sim_destroy(env->sim);
    if (env->cr) qsim_compile_result_free(env->cr);
}

static void test_part_select_rhs_cont_assign(void) {
    TEST("RHS continuous assign [15:8]");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  wire [7:0] extracted;\n"
        "  assign extracted = wide[15:8];\n"
        "  initial wide = 32'hAABBCCDD;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "extracted");
    if (!v) { FAIL("signal not found"); teardown_test(&env); return; }

    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    if (v->width != 8) { FAIL("width != 8"); teardown_test(&env); return; }
    if (val != 0xCC) { FAIL("expected 0xCC"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_rhs_low_bits(void) {
    TEST("RHS [7:0] (low byte)");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  wire [7:0] lo;\n"
        "  assign lo = wide[7:0];\n"
        "  initial wide = 32'hAABBCCDD;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "lo");
    if (!v) { FAIL("lo not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    if (val != 0xDD) { FAIL("expected 0xDD"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_rhs_high_bits(void) {
    TEST("RHS [31:24] (high byte)");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  wire [7:0] hi;\n"
        "  assign hi = wide[31:24];\n"
        "  initial wide = 32'hAABBCCDD;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "hi");
    if (!v) { FAIL("hi not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    if (val != 0xAA) { FAIL("expected 0xAA"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_rhs_reversed(void) {
    TEST("RHS [8:15] (lo-to-hi order)");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  wire [7:0] ext;\n"
        "  assign ext = wide[8:15];\n"
        "  initial wide = 32'hAABBCCDD;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "ext");
    if (!v) { FAIL("ext not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    /* wide[8:15] extracts bits 8-15 regardless of notation */
    if (val != 0xCC) { FAIL("expected 0xCC"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_lhs_blocking(void) {
    TEST("LHS blocking assign upper 16 bits");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  initial begin\n"
        "    wide = 32'h00000000;\n"
        "    wide[31:16] = 16'h1234;\n"
        "  end\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "wide");
    if (!v) { FAIL("wide not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    if (val != 0x12340000ULL) { FAIL("expected 0x12340000"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_lhs_nba(void) {
    TEST("LHS nonblocking assign [15:0]");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg clk;\n"
        "  reg [31:0] wide;\n"
        "  always @(posedge clk)\n"
        "    wide[15:0] <= 16'hABCD;\n"
        "  initial begin\n"
        "    wide = 32'hFFFF0000;\n"
        "    clk = 1;\n"
        "  end\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 5);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "wide");
    if (!v) { FAIL("wide not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    if (val != 0xFFFFABCDULL) { FAIL("expected 0xFFFFABCD"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

static void test_part_select_equality(void) {
    TEST("equality comparison (match)");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] addr;\n"
        "  wire match;\n"
        "  assign match = (addr[31:24] == 8'hA5);\n"
        "  initial addr = 32'hA5B0C0D0;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "match");
    if (!v) { FAIL("match not found"); teardown_test(&env); return; }
    if (v->width != 1 || qsim_bit_get(v, 0).state != QSIM_1) {
        FAIL("expected 1"); teardown_test(&env); return;
    }
    PASS();
    teardown_test(&env);
}

static void test_part_select_equality_mismatch(void) {
    TEST("equality comparison (mismatch)");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] addr;\n"
        "  wire match;\n"
        "  assign match = (addr[31:24] == 8'h5A);\n"
        "  initial addr = 32'hA5B0C0D0;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "match");
    if (!v) { FAIL("match not found"); teardown_test(&env); return; }
    if (v->width != 1 || qsim_bit_get(v, 0).state != QSIM_0) {
        FAIL("expected 0"); teardown_test(&env); return;
    }
    PASS();
    teardown_test(&env);
}

static void test_part_select_mid_bits(void) {
    TEST("RHS [20:12] middle bits");
    test_env_t env = {0};
    if (setup_test(
        "module test;\n"
        "  reg [31:0] wide;\n"
        "  wire [8:0] mid;\n"
        "  assign mid = wide[20:12];\n"
        "  initial wide = 32'h00FF7F00;\n"
        "endmodule\n", &env) != 0) { FAIL("setup"); return; }

    uir_sim_run(env.sim, 0);
    qsim_bit_vector_t *v = uir_sim_get_signal(env.sim, "mid");
    if (!v) { FAIL("mid not found"); teardown_test(&env); return; }
    uint64_t val = 0;
    for (uint32_t i = 0; i < v->width && i < 64; i++)
        if (qsim_bit_get(v, i).state == QSIM_1) val |= (1ULL << i);
    /* 0x00FF7F00 = 0b00000000111111110111111100000000.
     * Bits 12-20 (LSB 0): 12=1 13=1 14=1 15=0 16=1 17=1 18=1 19=1 20=1
     * Result = 0b111101111 (LSB-first) = 0x1F7 */
    if (val != 0x1F7) { FAIL("expected 0x1F7"); teardown_test(&env); return; }
    PASS();
    teardown_test(&env);
}

int main(void) {
    printf("Part-select regression tests (Bug #5)\n");
    printf("======================================\n\n");

    test_part_select_rhs_cont_assign();
    test_part_select_rhs_low_bits();
    test_part_select_rhs_high_bits();
    test_part_select_rhs_reversed();
    test_part_select_lhs_blocking();
    test_part_select_lhs_nba();
    test_part_select_equality();
    test_part_select_equality_mismatch();
    test_part_select_mid_bits();

    printf("\n");
    if (failed) { printf("SOME TESTS FAILED\n"); return 1; }
    printf("All tests passed\n");
    return 0;
}
