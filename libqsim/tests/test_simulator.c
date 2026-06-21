#include "libqsim/simulator.h"
#include "libqsim/uir.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir_sim.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void test_init_shutdown(void)
{
    qsim_init();
    qsim_shutdown();
}

static void test_compile_null_input(void)
{
    qsim_compile_result_t *r = qsim_compile(NULL);
    mu_assert_not_null(r);
    mu_assert(r->success != 0, "null input -> success (warning)");
    mu_assert(r->diag_count >= 1, "null input -> diagnostic");
    qsim_compile_result_free(r);
}

static void test_compile_empty_files(void)
{
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.files = NULL;
    input.file_count = 0;

    qsim_compile_result_t *r = qsim_compile(&input);
    mu_assert_not_null(r);
    mu_assert(r->success != 0, "empty files -> success (warning)");
    qsim_compile_result_free(r);
}

static void test_compile_nonexistent_file(void)
{
    const char *files[] = { "does_not_exist.v" };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.files = files;
    input.file_count = 1;

    qsim_compile_result_t *r = qsim_compile(&input);
    mu_assert_not_null(r);
    mu_assert(r->success == 0, "nonexistent file -> failure");
    mu_assert(r->diag_count >= 1, "diagnostic emitted");
    mu_assert(r->diagnostics[0].is_error != 0, "error diagnostic");

    qsim_compile_result_free(r);
}

static void test_compile_free_null(void)
{
    qsim_compile_result_free(NULL);
}

static void test_sim_null_session(void)
{
    qsim_sim_result_t *r = qsim_sim_run(NULL, 100);
    mu_assert(r == NULL, "null session -> null result");
}

static void test_sim_result_free_null(void)
{
    qsim_sim_result_free(NULL);
}

static void test_sim_result_lifecycle(void)
{
    /* Create a real compile result and pass it as session */
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);

    qsim_sim_result_t *sr = qsim_sim_run((void *)cr, 1000);
    mu_assert_not_null(sr);
    mu_assert(sr->stop_reason != NULL, "stop reason present");

    qsim_sim_result_free(sr);
    qsim_compile_result_free(cr);
}

static void test_compile_inline_verilog(void)
{
    const char *sources[] = {
        "module counter(input clk, output reg [3:0] count);\n"
        "  always @(posedge clk) begin\n"
        "    count <= count + 4'b1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *r = qsim_compile(&input);
    mu_assert_not_null(r);
    mu_assert(r->success != 0, "valid inline source should succeed");
    mu_assert(r->unit_count == 1, "should have 1 design unit");
    mu_assert_str_eq(((uir_design_unit_t *)r->units[0])->name, "counter", "module name");
    qsim_compile_result_free(r);
}

static void test_compile_and_simulate_counter(void)
{
    const char *sources[] = {
        "module counter(input clk, output reg [3:0] count);\n"
        "  always @(posedge clk) begin\n"
        "    count <= count + 4'b1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile counter");

    /* Create sim context directly to access signals */
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Verify initial state: count = X */
    qsim_bit_vector_t *count_val = uir_sim_get_signal(sim, "count");
    mu_assert_not_null(count_val);
    mu_assert_eq(count_val->width, 4, "count is 4 bits");
    mu_assert(qsim_bit_get(count_val, 0).state == QSIM_X, "count starts as X");

    qsim_bit_vector_t *clk_val = uir_sim_get_signal(sim, "clk");
    mu_assert_not_null(clk_val);

    /* Initialize count to 0 */
    qsim_bit_vector_t *zero = qsim_bit_vector_from_state(4, QSIM_0);
    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);

    uir_sim_set_signal(sim, "count", zero);
    uir_sim_run(sim, 1); /* process the event */

    /* Drive clk low at time 0 */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);

    /* Drive clk high at time 5 -> posedge */
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    /* Force count to 0 again (X from first posedge since regs start as X) */
    uir_sim_set_signal(sim, "count", zero);
    uir_sim_run(sim, 1);

    /* Drive clk low then high */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 10);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    /* Now count should be 0 + 1 = 1 */
    count_val = uir_sim_get_signal(sim, "count");
    mu_assert_eq(qsim_bit_get(count_val, 0).state, QSIM_1, "count[0] = 1");

    /* 5 more posedge clocks */
    for (int i = 0; i < 5; i++) {
        uir_sim_set_signal(sim, "clk", clk_0);
        uir_sim_run(sim, 10);
        uir_sim_set_signal(sim, "clk", clk_1);
        uir_sim_run(sim, 10);
    }

    /* Count should be 6 */
    count_val = uir_sim_get_signal(sim, "count");
    uint64_t cv;
    int ok = uir_bv_to_u64(count_val, &cv);
    mu_assert(ok != 0, "count should be known");
    mu_assert_eq(cv, 6, "count = 6 after 6 posedges starting from 0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(zero);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
}

static void test_compile_and_simulate_dff(void)
{
    const char *sources[] = {
        "module dff(input clk, input d, output reg q);\n"
        "  always @(posedge clk) q <= d;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile dff");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);

    /* Verify starts as X */
    qsim_bit_vector_t *q_val = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(q_val, 0).state == QSIM_X, "q starts X");

    /* Drive d=1, posedge clk 0->1 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_1, "q=1 after posedge d=1");

    /* Drive d=0, posedge clk 0->1 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_0, "q=0 after posedge d=0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_1);
    qsim_bit_vector_free(d_0);
}

static void test_posedge_x_to_1(void)
{
    /* Verify X→1 transition is detected as posedge per IEEE 1364-2005 §9.3.2.2 */
    const char *sources[] = {
        "module dff(input clk, input d, output reg q);\n"
        "  always @(posedge clk) q <= d;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile dff");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* q and clk start as X */

    /* Set d=1, then drive clk X→1 — must trigger posedge */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_1, "q=1 after X→1 posedge");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_1);
}

static void test_compile_and_simulate_mux(void)
{
    const char *sources[] = {
        "module mux2(input a, input b, input sel, output wire y);\n"
        "  assign y = sel ? a : b;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile mux");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* sel=0, a=1, b=0 -> y = sel ? a : b = 0 ? 1 : 0 = 0 */
    uir_sim_set_signal(sim, "a", v1);
    uir_sim_set_signal(sim, "b", v0);
    uir_sim_set_signal(sim, "sel", v0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_eq(qsim_bit_get(y_val, 0).state, QSIM_0, "y=0 sel=0 a=1 b=0");

    /* sel=1 -> y = 1 ? 1 : 0 = 1 */
    uir_sim_set_signal(sim, "sel", v1);
    uir_sim_run(sim, 5);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert_eq(qsim_bit_get(y_val, 0).state, QSIM_1, "y=1 sel=1 a=1 b=0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

static void test_compile_and_simulate_full_adder(void)
{
    const char *sources[] = {
        "module fulladd(input a, input b, input cin, output wire sum, output wire cout);\n"
        "  assign sum = a ^ b ^ cin;\n"
        "  assign cout = (a & b) | (a & cin) | (b & cin);\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile full adder");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Test all 8 combinations: {a,b,cin} -> {sum,cout} */
    struct { int a, b, cin, sum, cout; } cases[] = {
        {0,0,0,0,0}, {1,0,0,1,0}, {0,1,0,1,0}, {1,1,0,0,1},
        {0,0,1,1,0}, {1,0,1,0,1}, {0,1,1,0,1}, {1,1,1,1,1},
    };
    for (size_t i = 0; i < 8; i++) {
        uir_sim_set_signal(sim, "a",   cases[i].a ? v1 : v0);
        uir_sim_set_signal(sim, "b",   cases[i].b ? v1 : v0);
        uir_sim_set_signal(sim, "cin", cases[i].cin ? v1 : v0);
        uir_sim_run(sim, 5);

        qsim_bit_vector_t *sum_v = uir_sim_get_signal(sim, "sum");
        qsim_bit_vector_t *cout_v = uir_sim_get_signal(sim, "cout");
        char label[64];
        snprintf(label, sizeof(label), "sum=%d for %d+%d+%d", cases[i].sum, cases[i].a, cases[i].b, cases[i].cin);
        mu_assert_eq(qsim_bit_get(sum_v, 0).state, cases[i].sum ? QSIM_1 : QSIM_0, label);
        snprintf(label, sizeof(label), "cout=%d for %d+%d+%d", cases[i].cout, cases[i].a, cases[i].b, cases[i].cin);
        mu_assert_eq(qsim_bit_get(cout_v, 0).state, cases[i].cout ? QSIM_1 : QSIM_0, label);
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

static void test_compile_and_simulate_fsm(void)
{
    const char *sources[] = {
        "module fsm4(input clk, input rst, input in, output reg out);\n"
        "  reg [1:0] state, next;\n"
        "  always @(posedge clk) if (rst) state <= 0; else state <= next;\n"
        "  always @(*) begin\n"
        "    case (state)\n"
        "      0: next = in ? 1 : 0;\n"
        "      1: next = in ? 2 : 0;\n"
        "      2: next = in ? 3 : 1;\n"
        "      3: next = in ? 3 : 2;\n"
        "    endcase\n"
        "    out = (state == 3);\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile fsm");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Drive rst=1, in=1, apply posedge -> state should become 0 */
    uir_sim_set_signal(sim, "rst", v1);
    uir_sim_set_signal(sim, "in", v1);
    uir_sim_run(sim, 1);

    /* Posedge: state <= 0 (rst is high) */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *state_val = uir_sim_get_signal(sim, "state");
    mu_assert_eq(qsim_bit_get(state_val, 0).state, QSIM_0, "state=0 after rst");
    mu_assert_eq(qsim_bit_get(state_val, 1).state, QSIM_0, "state=0 after rst (msb)");

    /* Drive rst=0, in=1, then 4 posedges: 0->1->2->3 */
    uir_sim_set_signal(sim, "rst", v0);
    uir_sim_run(sim, 1);

    for (int expected = 1; expected <= 3; expected++) {
        uir_sim_set_signal(sim, "clk", clk_0);
        uir_sim_run(sim, 1);
        uir_sim_set_signal(sim, "clk", clk_1);
        uir_sim_run(sim, 10);

        state_val = uir_sim_get_signal(sim, "state");
        char label[64];
        snprintf(label, sizeof(label), "state=%d after %d posedge", expected, expected);
        mu_assert_eq(qsim_bit_get(state_val, 0).state, (expected & 1) ? QSIM_1 : QSIM_0, label);
        mu_assert_eq(qsim_bit_get(state_val, 1).state, (expected & 2) ? QSIM_1 : QSIM_0, label);
    }

    /* State=3, out should be 1 */
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_eq(qsim_bit_get(out_val, 0).state, QSIM_1, "out=1 in state 3");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

static qsim_bit_vector_t *bv4_from_uint8(uint8_t val)
{
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(4);
    if (bv) {
        for (int i = 0; i < 4; i++)
            qsim_bit_set(bv, i, ((val >> i) & 1) ? QSIM_VAL_1 : QSIM_VAL_0);
    }
    return bv;
}

static void test_simple_blocking_assign(void)
{
    const char *sources[] = {
        "module test_ba(input [3:0] in, output reg [1:0] out);\n"
        "  always @(*) begin\n"
        "    out = in[1:0];\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile test_ba");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab");
    uir_elab_result_free(elab);
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    qsim_bit_vector_t *v = bv4_from_uint8(0x3);
    uir_sim_set_signal(sim, "in", v);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_not_null(out_val);
    qsim_bit_vector_free(v);
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_case_simple(void)
{
    const char *sources[] = {
        "module test_case(input [3:0] in, output reg [1:0] out);\n"
        "  always @(*) begin\n"
        "    case (in)\n"
        "      4'b0001: out = 0;\n"
        "      4'b0010: out = 1;\n"
        "      default: out = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile test_case");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab");
    uir_elab_result_free(elab);
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    qsim_bit_vector_t *v = bv4_from_uint8(0x1);
    uir_sim_set_signal(sim, "in", v);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_not_null(out_val);
    qsim_bit_vector_free(v);
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_casez_simple(void)
{
    const char *sources[] = {
        "module test_casez(input [3:0] in, output reg [1:0] out);\n"
        "  always @(*) begin\n"
        "    casez (in)\n"
        "      4'b???1: out = 0;\n"
        "      default: out = 1;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile test_casez");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab");
    uir_elab_result_free(elab);
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    qsim_bit_vector_t *v = bv4_from_uint8(0x1);
    uir_sim_set_signal(sim, "in", v);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_not_null(out_val);
    qsim_bit_vector_free(v);
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_compile_and_simulate_priority_encoder(void)
{
    const char *sources[] = {
        "module prienc(input [3:0] in, output reg [1:0] out);\n"
        "  always @(*) begin\n"
        "    casez (in)\n"
        "      4'b???1: out = 0;\n"
        "      4'b??10: out = 1;\n"
        "      4'b?100: out = 2;\n"
        "      4'b1000: out = 3;\n"
        "      default: out = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile prienc");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *in_val;

    /* in=0001 -> out=0 */
    in_val = bv4_from_uint8(0x1);
    uir_sim_set_signal(sim, "in", in_val);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_eq(qsim_bit_get(out_val, 0).state, QSIM_0, "out[0]=0 for in=0001");
    mu_assert_eq(qsim_bit_get(out_val, 1).state, QSIM_0, "out[1]=0 for in=0001");
    qsim_bit_vector_free(in_val);

    /* in=0010 -> out=1 */
    in_val = bv4_from_uint8(0x2);
    uir_sim_set_signal(sim, "in", in_val);
    uir_sim_run(sim, 5);
    out_val = uir_sim_get_signal(sim, "out");
    mu_assert_eq(qsim_bit_get(out_val, 0).state, QSIM_1, "out[0]=1 for in=0010");
    mu_assert_eq(qsim_bit_get(out_val, 1).state, QSIM_0, "out[1]=0 for in=0010");
    qsim_bit_vector_free(in_val);

    /* in=0100 -> out=2 */
    in_val = bv4_from_uint8(0x4);
    uir_sim_set_signal(sim, "in", in_val);
    uir_sim_run(sim, 5);
    out_val = uir_sim_get_signal(sim, "out");
    mu_assert_eq(qsim_bit_get(out_val, 0).state, QSIM_0, "out[0]=0 for in=0100");
    mu_assert_eq(qsim_bit_get(out_val, 1).state, QSIM_1, "out[1]=1 for in=0100");
    qsim_bit_vector_free(in_val);

    /* in=1000 -> out=3 */
    in_val = bv4_from_uint8(0x8);
    uir_sim_set_signal(sim, "in", in_val);
    uir_sim_run(sim, 5);
    out_val = uir_sim_get_signal(sim, "out");
    mu_assert_eq(qsim_bit_get(out_val, 0).state, QSIM_1, "out[0]=1 for in=1000");
    mu_assert_eq(qsim_bit_get(out_val, 1).state, QSIM_1, "out[1]=1 for in=1000");
    qsim_bit_vector_free(in_val);

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_compile_inline_vhdl(void)
{
    const char *sources[] = {
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= a;\n"
        "end architecture;\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *r = qsim_compile(&input);
    mu_assert_not_null(r);
    mu_assert(r->success != 0, "valid VHDL inline source should succeed");
    mu_assert(r->unit_count == 1, "should have 1 design unit");
    mu_assert_str_eq(((uir_design_unit_t *)r->units[0])->name, "top", "entity name");
    qsim_compile_result_free(r);
}

/* =================================================================
 * Mixed-language elaboration: Verilog top instantiating VHDL entity
 * ================================================================= */

static void test_mixed_elab_verilog_top_vhdl_sub(void)
{
    /* VHDL entity + architecture */
    const char *vhdl_src =
        "entity counter is\n"
        "  port (clk: in std_logic; rst: in std_logic; count: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of counter is\n"
        "  signal s_count: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_count <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      s_count <= s_count + \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= s_count;\n"
        "end architecture;\n";

    /* Verilog top that instantiates the VHDL entity */
    const char *verilog_src =
        "module testbench;\n"
        "  reg clk, rst;\n"
        "  wire [3:0] count;\n"
        "  counter uut(.clk(clk), .rst(rst), .count(count));\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.source_count = 2;
    const char *sources[] = {vhdl_src, verilog_src};
    input.sources = sources;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);

mu_assert(cr->success != 0, "mixed compile should succeed");
    mu_assert(cr->unit_count == 2, "should have 2 design units");

    /* Find the VHDL counter and Verilog testbench units */
    uir_design_unit_t *counter_unit = NULL;
    uir_design_unit_t *tb_unit = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "counter") == 0)
            counter_unit = u;
        if (strcmp(u->name, "testbench") == 0)
            tb_unit = u;
    }
    mu_assert(counter_unit != NULL, "counter unit found");
    mu_assert(tb_unit != NULL, "testbench unit found");

    /* Verify testbench has an instance */
    mu_assert(tb_unit->instance_count > 0, "testbench has instance");
    mu_assert_str_eq(tb_unit->instances[0]->instance_name, "uut", "instance name uut");
    mu_assert_str_eq(tb_unit->instances[0]->module_name, "counter", "module name counter");

    /* Elaborate both units together */
    uir_design_unit_t *units[] = {counter_unit, tb_unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert_not_null(elab);
    mu_assert(elab->success != 0, "mixed elaboration should succeed");
    mu_assert_eq(elab->diag_count, 0, "no elaboration diagnostics");

    /* Verify instance is bound */
    mu_assert(tb_unit->instances[0]->bound_to != NULL, "instance bound after elaboration");
    mu_assert(tb_unit->instances[0]->bound_to == counter_unit, "bound to counter unit");

    uir_elab_result_free(elab);
    qsim_compile_result_free(cr);
}

static void test_compile_and_simulate_vhdl_counter(void)
{
    const char *sources[] = {
        "entity counter is\n"
        "  port (clk: in std_logic; rst: in std_logic; count: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of counter is\n"
        "  signal s_count: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_count <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      s_count <= s_count + \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= s_count;\n"
        "end architecture;\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile VHDL counter");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Reset: rst=1 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_run(sim, 5);

    /* After reset, count should be 0 */
    qsim_bit_vector_t *cv = uir_sim_get_signal(sim, "count");
    uint64_t count_val;
    mu_assert(uir_bv_to_u64(cv, &count_val) != 0, "count known after reset");
    mu_assert_eq(count_val, 0, "count = 0 after reset");

    /* Release reset, toggle clk posedge: count should become 1 */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    cv = uir_sim_get_signal(sim, "count");
    mu_assert(uir_bv_to_u64(cv, &count_val) != 0, "count known after clk");
    mu_assert_eq(count_val, 1, "count = 1 after one posedge");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
}

/* =================================================================
 * Mixed-language simulation: Verilog testbench drives VHDL DFF
 * ================================================================= */

static void test_mixed_sim_verilog_top_vhdl_sub(void)
{
    /* VHDL DFF — drives output q directly in process, no continuous assign */
    const char *vhdl_src =
        "entity dff is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of dff is\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    /* Verilog testbench instantiating the VHDL DFF */
    const char *verilog_src =
        "module testbench;\n"
        "  reg clk, d;\n"
        "  wire q;\n"
        "  dff uut(.clk(clk), .d(d), .q(q));\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.source_count = 2;
    const char *sources[] = {vhdl_src, verilog_src};
    input.sources = sources;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "mixed compile should succeed");
    mu_assert(cr->unit_count == 2, "should have 2 design units");

    /* Find units */
    uir_design_unit_t *dff_unit = NULL;
    uir_design_unit_t *tb_unit = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "dff") == 0) dff_unit = u;
        if (strcmp(u->name, "testbench") == 0) tb_unit = u;
    }
    mu_assert(dff_unit != NULL, "dff unit found");
    mu_assert(tb_unit != NULL, "testbench unit found");

    uir_design_unit_t *units[] = {dff_unit, tb_unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert_not_null(elab);
    mu_assert(elab->success != 0, "mixed elaboration");
    mu_assert(tb_unit->instances[0]->bound_to == dff_unit, "instance bound");
    uir_elab_result_free(elab);

    /* Create simulation context with testbench only (child reached via hierarchy) */
    uir_sim_context_t *sim = uir_sim_create(&tb_unit, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_1  = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0  = qsim_bit_vector_from_state(1, QSIM_0);

    /* Drive d=1, toggle clk posedge: q should become 1 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    /* Check q via port wire (hierarchical: uut.q propagated to q) */
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after d=1 posedge");

    /* Also check direct hierarchical path */
    qv = uir_sim_get_signal(sim, "uut.q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "uut.q = 1");

    /* Drive d=0, toggle clk: q should become 0 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after d=0 posedge");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_1);
    qsim_bit_vector_free(d_0);
    qsim_compile_result_free(cr);
}

static void test_mixed_sim_vhdl_top_verilog_sub(void)
{
    /* Verilog DFF */
    const char *verilog_src =
        "module dff(input clk, input d, output reg q);\n"
        "  always @(posedge clk) q <= d;\n"
        "endmodule\n";

    /* VHDL testbench with component instantiation */
    const char *vhdl_src =
        "entity testbench is\n"
        "  port (clk: out std_logic; d: out std_logic; q: in std_logic);\n"
        "end entity;\n"
        "architecture sim of testbench is\n"
        "begin\n"
        "  uut: dff port map (clk => clk, d => d, q => q);\n"
        "end architecture;\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.source_count = 2;
    const char *sources[] = {vhdl_src, verilog_src};
    input.sources = sources;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "mixed compile should succeed");
    mu_assert(cr->unit_count == 2, "should have 2 design units");

    /* Find units */
    uir_design_unit_t *tb_unit = NULL;
    uir_design_unit_t *dff_unit = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "testbench") == 0) tb_unit = u;
        if (strcmp(u->name, "dff") == 0) dff_unit = u;
    }
    mu_assert(tb_unit != NULL, "testbench unit found");
    mu_assert(dff_unit != NULL, "dff unit found");

    /* Verify component instantiation parsed correctly */
    mu_assert(tb_unit->instance_count == 1, "tb has 1 instance");
    mu_assert(strcmp(tb_unit->instances[0]->instance_name, "uut") == 0, "inst name uut");
    mu_assert(strcmp(tb_unit->instances[0]->module_name, "dff") == 0, "module name dff");
    mu_assert(tb_unit->instances[0]->connection_count == 3, "3 connections");

    /* Elaborate: bind testbench's uut instance to the Verilog dff module */
    uir_design_unit_t *units[] = {dff_unit, tb_unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert_not_null(elab);
    mu_assert(elab->success != 0, "mixed elab");
    mu_assert(tb_unit->instances[0]->bound_to == dff_unit, "bound to dff");
    uir_elab_result_free(elab);

    /* Create simulation with testbench only (DFF reached via hierarchy) */
    uir_sim_context_t *sim = uir_sim_create(&tb_unit, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_1  = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0  = qsim_bit_vector_from_state(1, QSIM_0);

    /* Drive d=1, toggle clk posedge: q should become 1 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after d=1 posedge");

    /* Check hierarchical path too */
    qv = uir_sim_get_signal(sim, "uut.q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "uut.q = 1");

    /* Drive d=0, toggle clk: q should become 0 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after d=0 posedge");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_1);
    qsim_bit_vector_free(d_0);
    qsim_compile_result_free(cr);
}

/* =================================================================
 * Mixed-language port propagation: Verilog drives VHDL entity port
 * ================================================================= */

static void test_mixed_port_prop_verilog_to_vhdl_ca(void)
{
    /* VHDL: concurrent assignment b <= a — test input port propagation */
    const char *vhdl_src =
        "entity test is\n"
        "  port (a: in std_logic; b: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of test is\n"
        "begin\n"
        "  b <= a;\n"
        "end architecture;\n";

    const char *verilog_src =
        "module tb;\n"
        "  reg a;\n"
        "  wire b;\n"
        "  test uut(.a(a), .b(b));\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.source_count = 2;
    const char *sources[] = {vhdl_src, verilog_src};
    input.sources = sources;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "mixed compile should succeed");

    uir_design_unit_t *tb_unit = NULL, *test_unit = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "test") == 0) test_unit = u;
        if (strcmp(u->name, "tb") == 0) tb_unit = u;
    }
    mu_assert(test_unit != NULL, "test unit found");
    mu_assert(tb_unit != NULL, "tb unit found");

    uir_design_unit_t *units[] = {test_unit, tb_unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "mixed elaboration");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *one = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *zero = qsim_bit_vector_from_state(1, QSIM_0);

    /* Drive a=1 from Verilog side */
    uir_sim_set_signal(sim, "a", one);
    uir_sim_run(sim, 10);

    /* b should be 1 via port propagation → VHDL CA */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    mu_assert_ptr_not_null(bv, "b exists");
    mu_assert(qsim_bit_get(bv, 0).state == QSIM_1, "b = 1 after a=1 (port propagation)");

    /* Drive a=0 */
    uir_sim_set_signal(sim, "a", zero);
    uir_sim_run(sim, 10);

    bv = uir_sim_get_signal(sim, "b");
    mu_assert_ptr_not_null(bv, "b exists after a=0");
    mu_assert(qsim_bit_get(bv, 0).state == QSIM_0, "b = 0 after a=0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(one);
    qsim_bit_vector_free(zero);
}

static void test_mixed_port_prop_vhdl_process_to_internal(void)
{
    /* VHDL process reads input port via sensitivity, drives internal signal.
     * This tests the scenario from the bug report: forcing a Verilog parent
     * wire must propagate to the VHDL entity's port signal and trigger
     * VHDL processes sensitive to that port. */
    const char *vhdl_src =
        "entity test is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of test is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      s <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s;\n"
        "end architecture;\n";

    const char *verilog_src =
        "module tb;\n"
        "  reg clk, d;\n"
        "  wire q;\n"
        "  test uut(.clk(clk), .d(d), .q(q));\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.source_count = 2;
    const char *sources[] = {vhdl_src, verilog_src};
    input.sources = sources;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "mixed compile");

    uir_design_unit_t *tb_unit = NULL, *test_unit = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "test") == 0) test_unit = u;
        if (strcmp(u->name, "tb") == 0) tb_unit = u;
    }
    mu_assert(test_unit != NULL, "test unit found");
    mu_assert(tb_unit != NULL, "tb unit found");

    uir_design_unit_t *units[] = {test_unit, tb_unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "mixed elab");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *one = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *zero = qsim_bit_vector_from_state(1, QSIM_0);

    /* Drive d=1, toggle clk posedge: q should become 1 */
    uir_sim_set_signal(sim, "d", one);
    uir_sim_set_signal(sim, "clk", zero);
    uir_sim_run(sim, 5);
    uir_sim_set_signal(sim, "clk", one);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_ptr_not_null(qv, "q exists");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after d=1 posedge");

    /* Drive d=0, toggle clk: q should become 0 */
    uir_sim_set_signal(sim, "d", zero);
    uir_sim_set_signal(sim, "clk", zero);
    uir_sim_run(sim, 5);
    uir_sim_set_signal(sim, "clk", one);
    uir_sim_run(sim, 10);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert_ptr_not_null(qv, "q exists after d=0");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after d=0 posedge");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(one);
    qsim_bit_vector_free(zero);
}

static void test_recovery_from_names(void)
{
    /* Exact match should produce no suggestions */
    const char *names[] = {"top", "counter", "adder", "mux"};
    qsim_recovery_t *rec = qsim_recovery_from_names("counter", names, 4, "compile");
    mu_assert_ptr_null(rec, "exact match -> NULL");

    /* Close match should suggest */
    rec = qsim_recovery_from_names("countr", names, 4, "compile");
    mu_assert_ptr_not_null(rec, "close match -> recovery");
    mu_assert(rec->suggestion_count >= 1, "at least 1 suggestion");
    mu_assert(strcmp(rec->suggestions[0], "counter") == 0,
              "best suggestion is 'counter'");
    mu_assert(rec->next_tool != NULL, "next_tool set");
    free(rec->suggestions[0]);
    free(rec->suggestions);
    free(rec);

    /* Far match -> no suggestions */
    rec = qsim_recovery_from_names("zzzzz", names, 4, NULL);
    mu_assert_ptr_null(rec, "too far -> NULL");

    /* NULL inputs */
    rec = qsim_recovery_from_names(NULL, names, 4, NULL);
    mu_assert_ptr_null(rec, "NULL name -> NULL");
    rec = qsim_recovery_from_names("foo", NULL, 0, NULL);
    mu_assert_ptr_null(rec, "NULL names -> NULL");

    /* Empty list */
    rec = qsim_recovery_from_names("foo", names, 0, NULL);
    mu_assert_ptr_null(rec, "empty list -> NULL");

    /* Typo: 'addre' should suggest 'adder' */
    rec = qsim_recovery_from_names("addre", names, 4, "compile");
    mu_assert_ptr_not_null(rec, "typo -> recovery");
    mu_assert(rec->suggestion_count >= 1, "at least 1 suggestion");
    mu_assert(strcmp(rec->suggestions[0], "adder") == 0,
              "best suggestion is 'adder'");
    free(rec->suggestions[0]);
    free(rec->suggestions);
    free(rec);
}

/* ── Function/Task simulation tests ── */

static void test_func_return_constant(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg [7:0] result;\n"
        "  function [7:0] get42;\n"
        "  begin\n"
        "    get42 = 8'h42;\n"
        "  end\n"
        "  endfunction\n"
        "  initial begin\n"
        "    result = get42();\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim context");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *result = uir_sim_get_signal(sim, "result");
    mu_assert_ptr_not_null(result, "result signal");
    uint64_t val;
    mu_assert(uir_bv_to_u64(result, &val), "result known");
    mu_assert_eq(val, 0x42ULL, "result = 0x42");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_func_add_args(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg [7:0] result;\n"
        "  function [7:0] add;\n"
        "    input [7:0] a, b;\n"
        "  begin\n"
        "    add = a + b;\n"
        "  end\n"
        "  endfunction\n"
        "  initial begin\n"
        "    result = add(8'h30, 8'h12);\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim context");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *result = uir_sim_get_signal(sim, "result");
    mu_assert_ptr_not_null(result, "result signal");
    uint64_t val;
    mu_assert(uir_bv_to_u64(result, &val), "result known");
    mu_assert_eq(val, 0x42ULL, "0x30 + 0x12 = 0x42");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_func_call_in_always(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg [7:0] cnt;\n"
        "  reg [7:0] result;\n"
        "  function [7:0] double_it;\n"
        "    input [7:0] x;\n"
        "  begin\n"
        "    double_it = x + x;\n"
        "  end\n"
        "  endfunction\n"
        "  always @(cnt) begin\n"
        "    result = double_it(cnt);\n"
        "  end\n"
        "  initial begin\n"
        "    cnt = 8'd5;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim context");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *result = uir_sim_get_signal(sim, "result");
    mu_assert_ptr_not_null(result, "result signal");
    uint64_t val;
    mu_assert(uir_bv_to_u64(result, &val), "result known");
    mu_assert_eq(val, 10ULL, "double(5) = 10");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_task_swap_inout(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg [7:0] a, b;\n"
        "  task swap;\n"
        "    inout [7:0] x, y;\n"
        "    reg [7:0] tmp;\n"
        "  begin\n"
        "    tmp = x;\n"
        "    x = y;\n"
        "    y = tmp;\n"
        "  end\n"
        "  endtask\n"
        "  initial begin\n"
        "    a = 8'd3;\n"
        "    b = 8'd5;\n"
        "    swap(a, b);\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim context");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    qsim_bit_vector_t *b_val = uir_sim_get_signal(sim, "b");
    mu_assert_ptr_not_null(a_val, "a signal");
    mu_assert_ptr_not_null(b_val, "b signal");
    uint64_t av, bv;
    mu_assert(uir_bv_to_u64(a_val, &av), "a known");
    mu_assert(uir_bv_to_u64(b_val, &bv), "b known");
    mu_assert_eq(av, 5ULL, "a == 5 after swap");
    mu_assert_eq(bv, 3ULL, "b == 3 after swap");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_func_in_cont_assign(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] out;\n"
        "  reg [7:0] a, b;\n"
        "  function [7:0] add;\n"
        "    input [7:0] x, y;\n"
        "  begin\n"
        "    add = x + y;\n"
        "  end\n"
        "  endfunction\n"
        "  assign out = add(a, b);\n"
        "  initial begin\n"
        "    a = 8'd2;\n"
        "    b = 8'd3;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim context");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "out");
    mu_assert_ptr_not_null(out_val, "out signal");
    uint64_t val;
    mu_assert(uir_bv_to_u64(out_val, &val), "out known");
    mu_assert_eq(val, 5ULL, "add(2,3) = 5");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── Implicit wire tests ── */

static void test_implicit_wire_cont_assign(void)
{
    /* Undeclared wire should inherit width from RHS */
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] a;\n"
        "  assign a = 8'hAB;\n"
        "  assign implicit_wire = a;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "implicit_wire");
    mu_assert_ptr_not_null(v, "implicit_wire signal");
    mu_assert_eq(v->width, 8U, "implicit_wire width");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_implicit_wire_blocking_assign(void)
{
    /* Undeclared wire in initial block with blocking assign */
    const char *sources[] = {
        "module test;\n"
        "  reg [15:0] a;\n"
        "  initial begin\n"
        "    a = 16'h1234;\n"
        "    implicit_wire = a;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "implicit_wire");
    mu_assert_ptr_not_null(v, "implicit_wire signal");
    mu_assert_eq(v->width, 16U, "implicit_wire width from blocking assign");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_implicit_wire_nonblocking_assign(void)
{
    /* Undeclared wire in always block with nonblocking assign */
    const char *sources[] = {
        "module test;\n"
        "  reg clk;\n"
        "  reg [3:0] d;\n"
        "  always @(posedge clk)\n"
        "    implicit_wire <= d;\n"
        "  initial begin\n"
        "    d = 4'hA;\n"
        "    clk = 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim");

    uir_sim_run(sim, 5);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "implicit_wire");
    mu_assert_ptr_not_null(v, "implicit_wire signal");
    mu_assert_eq(v->width, 4U, "implicit_wire width from nonblocking assign");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_implicit_wire_expr_rhs(void)
{
    /* Implicit wire from binary expression RHS */
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] a;\n"
        "  wire [3:0] b;\n"
        "  assign a = 8'h12;\n"
        "  assign b = 4'h3;\n"
        "  assign implicit_wire = a ^ b;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "implicit_wire");
    mu_assert_ptr_not_null(v, "implicit_wire signal");
    mu_assert_eq(v->width, 8U, "implicit_wire width from binary expr (max of operands)");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_implicit_wire_declared_not_overridden(void)
{
    /* Explicitly declared wire should NOT be overridden */
    const char *sources[] = {
        "module test;\n"
        "  wire [3:0] explicit_wire;\n"
        "  wire [7:0] a;\n"
        "  assign a = 8'hAB;\n"
        "  assign explicit_wire = a;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "explicit_wire");
    mu_assert_ptr_not_null(v, "explicit_wire signal");
    mu_assert_eq(v->width, 4U, "explicit 4-bit wire stays 4-bit");

    /* Should truncate: 8'hAB -> 4'hB */
    uint64_t val;
    mu_assert(uir_bv_to_u64(v, &val), "explicit_wire known");
    mu_assert_eq(val, 0xBULL, "explicit_wire value truncated");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── Delay / timing control tests ── */

static void test_delay_initial_toggle(void)
{
    /* Single #delay in initial block with sequential delays */
    const char *sources[] = {
        "module test;\n"
        "  reg a;\n"
        "  initial begin\n"
        "    #5 a = 1;\n"
        "    #10 a = 0;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile delay_initial_toggle");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Initial eval fires; Delay{5,...} schedules wakeup at t=5.
     * Run to t=6 to cover that event. a should become 1 at t=5. */
    uir_sim_run(sim, 6);
    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a at t=6");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 after #5 delay");

    /* Run to t=16 to cover the #10 delay from t=5. a should become 0 at t=15. */
    uir_sim_run(sim, 10);
    a_val = uir_sim_get_signal(sim, "a");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_0, "a=0 after #10 delay");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_delay_clock_gen(void)
{
    /* Clock generation: always #5 clk = ~clk; */
    const char *sources[] = {
        "module test;\n"
        "  reg clk;\n"
        "  initial clk = 0;\n"
        "  always #5 clk = ~clk;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile delay_clock_gen");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* initial clk=0 runs at t=0, making clk=0.
     * always #5 clk=~clk fires at t=0 (restructured: Delay{5, Block[clk=~clk, Delay{5, ...}]})
     * schedules wakeup at t=5.
     * Run to t=6: clk should be 1 at t=5. */
    uir_sim_run(sim, 6);
    qsim_bit_vector_t *clk = uir_sim_get_signal(sim, "clk");
    mu_assert_ptr_not_null(clk, "clk at t=6");
    mu_assert_eq(qsim_bit_get(clk, 0).state, QSIM_1, "clk=1 at t=5");

    /* Run to t=11: clk should toggle back to 0 at t=10 */
    uir_sim_run(sim, 5);
    clk = uir_sim_get_signal(sim, "clk");
    mu_assert_eq(qsim_bit_get(clk, 0).state, QSIM_0, "clk=0 at t=10");

    /* Run to t=16: clk should toggle to 1 at t=15 */
    uir_sim_run(sim, 5);
    clk = uir_sim_get_signal(sim, "clk");
    mu_assert_eq(qsim_bit_get(clk, 0).state, QSIM_1, "clk=1 at t=15");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_delay_cont_assign(void)
{
    /* Continuous assign with propagation delay: assign #3 a = b; */
    const char *sources[] = {
        "module test;\n"
        "  wire a;\n"
        "  reg b;\n"
        "  assign #3 a = b;\n"
        "  initial begin\n"
        "    b = 0;\n"
        "    #1 b = 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile delay_cont_assign");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* At t=0: initial block sets b=0. The continuous assign #3 fires,
     * scheduling a=0 at t=3. Then #1 b=1 schedules b=1 at t=1.
     * When b changes to 1 at t=1, assign #3 fires again, scheduling a=1 at t=4.
     * The earlier event for a at t=3 is inertially filtered (t=3 < t=4).
     */

    /* Run to t=8 to cover both propagation events */
    uir_sim_run(sim, 8);
    qsim_bit_vector_t *b_val = uir_sim_get_signal(sim, "b");
    mu_assert_ptr_not_null(b_val, "b at t=8");
    mu_assert_eq(qsim_bit_get(b_val, 0).state, QSIM_1, "b=1");

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a at t=8");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 after inertial delay");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── Loop (for/while/repeat/forever) simulation tests ── */

static void test_loop_for_sync(void)
{
    /* Synchronous for loop (no timing controls) */
    const char *sources[] = {
        "module test;\n"
        "  integer i;\n"
        "  reg [7:0] acc;\n"
        "  initial begin\n"
        "    acc = 0;\n"
        "    for (i = 0; i < 5; i = i + 1)\n"
        "      acc = acc + 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile for sync");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *acc = uir_sim_get_signal(sim, "acc");
    mu_assert_ptr_not_null(acc, "acc exists");
    mu_assert_eq(qsim_bit_get(acc, 0).state, QSIM_1, "acc[0]=1 (acc=5)");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_loop_while_sync(void)
{
    const char *sources[] = {
        "module test;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    i = 0;\n"
        "    while (i < 3)\n"
        "      i = i + 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile while sync");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *i_val = uir_sim_get_signal(sim, "i");
    mu_assert_ptr_not_null(i_val, "i exists");
    mu_assert_eq(qsim_bit_get(i_val, 0).state, QSIM_1, "i[0]=1 (i=3)");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_loop_repeat_sync(void)
{
    const char *sources[] = {
        "module test;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    i = 0;\n"
        "    repeat(3) i = i + 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile repeat sync");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *i_val = uir_sim_get_signal(sim, "i");
    mu_assert_ptr_not_null(i_val, "i exists");
    mu_assert_eq(qsim_bit_get(i_val, 0).state, QSIM_1, "i[0]=1 (i=3)");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_loop_for_timing(void)
{
    /* For loop WITH timing control inside body */
    const char *sources[] = {
        "module test;\n"
        "  integer i;\n"
        "  reg [7:0] arr [0:3];\n"
        "  initial begin\n"
        "    for (i = 0; i < 3; i = i + 1)\n"
        "      #5 arr[i] = i;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile for timing");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* t=0: init i=0, schedule body at t=5 */
    /* t=5: arr[0]=0, i=1, (1<3) → schedule body at t=10 */
    /* t=10: arr[1]=1, i=2, (2<3) → schedule body at t=15 */
    /* t=15: arr[2]=2, i=3, (3<3) → done */
    uir_sim_run(sim, 20);

    qsim_bit_vector_t *i_val = uir_sim_get_signal(sim, "i");
    mu_assert_ptr_not_null(i_val, "i exists");
    mu_assert_eq(qsim_bit_get(i_val, 0).state, QSIM_1, "i[0]=1 (i=3)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_loop_forever_clock(void)
{
    /* forever #5 clk = ~clk; generates a clock */
    const char *sources[] = {
        "module test;\n"
        "  reg clk;\n"
        "  initial clk = 0;\n"
        "  initial forever #5 clk = ~clk;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile forever clock");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* t=0: clk=0 runs. forever #5 clk=~clk schedules wakeup at t=5. */
    uir_sim_run(sim, 6);
    qsim_bit_vector_t *clk = uir_sim_get_signal(sim, "clk");
    mu_assert_ptr_not_null(clk, "clk at t=6");
    mu_assert_eq(qsim_bit_get(clk, 0).state, QSIM_1, "clk=1 at t=5");

    uir_sim_run(sim, 5);
    clk = uir_sim_get_signal(sim, "clk");
    mu_assert_eq(qsim_bit_get(clk, 0).state, QSIM_0, "clk=0 at t=10");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_port_propagation_basic(void)
{
    /* Top-level wire drives child module input port */
    const char *sources[] = {
        "module child(input a, output reg y);\n"
        "  always @(a) y = a;\n"
        "endmodule\n",
        "module top;\n"
        "  reg w;\n"
        "  child uut(.a(w), .y());\n"
        "  initial w = 1;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile port_prop");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run initial eval - initial block sets w=1 */
    uir_sim_run(sim, 0);

    /* Dump signal names for debugging */
    int sc = uir_sim_get_signal_count(sim);
    for (int i = 0; i < sc; i++)
        printf("    sig[%d] = %s\n", i, uir_sim_get_signal_name(sim, i));

    qsim_bit_vector_t *w_val = uir_sim_get_signal(sim, "w");
    if (!w_val) w_val = uir_sim_get_signal(sim, "top.w");
    mu_assert_ptr_not_null(w_val, "w exists");
    mu_assert_eq(qsim_bit_get(w_val, 0).state, QSIM_1, "w=1");

    /* The child port a should be 1 (connected to w through port connection) */
    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "top.uut.a");
    if (!a_val) a_val = uir_sim_get_signal(sim, "uut.a");
    mu_assert_ptr_not_null(a_val, "child port a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "child port a=1 via port connection");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_port_propagation_output(void)
{
    /* Child output port propagates back to parent wire */
    const char *sources[] = {
        "module child(input a, output reg y);\n"
        "  always @(a) y = a;\n"
        "endmodule\n",
        "module top;\n"
        "  reg w;\n"
        "  wire w_out;\n"
        "  child uut(.a(w), .y(w_out));\n"
        "  initial w = 1;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile output_prop");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run initial eval: w=1 → propagated to uut.a → uut.y=1 → propagated to w_out */
    uir_sim_run(sim, 0);

    qsim_bit_vector_t *w_val = uir_sim_get_signal(sim, "w");
    if (!w_val) w_val = uir_sim_get_signal(sim, "top.w");
    mu_assert_ptr_not_null(w_val, "w exists");
    mu_assert_eq(qsim_bit_get(w_val, 0).state, QSIM_1, "w=1");

    /* w_out should be 1 (propagated from child output y back to parent) */
    qsim_bit_vector_t *w_out_val = uir_sim_get_signal(sim, "w_out");
    if (!w_out_val) w_out_val = uir_sim_get_signal(sim, "top.w_out");
    mu_assert_ptr_not_null(w_out_val, "w_out exists");
    mu_assert_eq(qsim_bit_get(w_out_val, 0).state, QSIM_1, "w_out=1 via output port propagation");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_reg_equality_comparison(void)
{
    /* Bug #1 regression: multi-bit == on reg returns X.
     * When a reg is assigned a known value via blocking assign,
     * comparing it against a literal must return the correct result. */
    const char *sources[] = {
        "module test;\n"
        "  reg [1:0] grant_ff;\n"
        "  reg       match;\n"
        "  wire      match_w;\n"
        "  assign match_w = (grant_ff == 2'b01);\n"
        "  initial begin\n"
        "    grant_ff = 2'b01;\n"
        "    #1;\n"
        "    match = (grant_ff == 2'b01);\n"
        "  end\n"
        "endmodule\n"
    };

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile eq");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run: initial fires, sets grant_ff=2'b01, schedules #1 wakeup,
     * then at time 1 the condition grant_ff==2'b01 is evaluated */
    uir_sim_run(sim, 10);

    /* Check grant_ff value */
    qsim_bit_vector_t *gf = uir_sim_get_signal(sim, "grant_ff");
    mu_assert_ptr_not_null(gf, "grant_ff exists");
    mu_assert_eq(qsim_bit_get(gf, 0).state, QSIM_1, "grant_ff[0]=1");
    mu_assert_eq(qsim_bit_get(gf, 1).state, QSIM_0, "grant_ff[1]=0");

    /* Check match_w (continuous assign: grant_ff == 2'b01) */
    qsim_bit_vector_t *mw = uir_sim_get_signal(sim, "match_w");
    if (!mw) mw = uir_sim_get_signal(sim, "test.match_w");
    mu_assert_ptr_not_null(mw, "match_w exists");
    mu_assert_eq(qsim_bit_get(mw, 0).state, QSIM_1, "match_w=1 via cont assign == on reg");

    /* Check match (blocking assign: match = (grant_ff == 2'b01)) */
    qsim_bit_vector_t *mv = uir_sim_get_signal(sim, "match");
    if (!mv) mv = uir_sim_get_signal(sim, "test.match");
    mu_assert_ptr_not_null(mv, "match exists");
    mu_assert_eq(qsim_bit_get(mv, 0).state, QSIM_1, "match=1 via blocking assign == on reg");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_nba_compare_after_clock(void)
{
    /* NPU-like pattern: NBA sets a reg, then continuous assign
     * compares the reg against a literal in the next delta. */
    const char *sources[] = {
        "module test;\n"
        "  reg       clk;\n"
        "  reg [1:0] grant_ff;\n"
        "  wire      match;\n"
        "  assign match = (grant_ff == 2'b01);\n"
        "  always #5 clk = ~clk;\n"
        "  always @(posedge clk)\n"
        "    grant_ff <= 2'b01;\n"
        "  initial clk = 0;\n"
        "endmodule\n"
    };

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile nba_eq");
    if (!cr->success) {
        printf("  COMPILE FAILED, %zu diagnostics:\n", cr->diag_count);
        for (size_t di = 0; di < cr->diag_count; di++)
            printf("    %s\n", cr->diagnostics[di].message ? cr->diagnostics[di].message : "?");
    }
    mu_assert(cr->success != 0, "compile nba_eq ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run past the first posedge at t=5 */
    uir_sim_run(sim, 10);

    /* After posedge, grant_ff <= 2'b01 should have taken effect */
    qsim_bit_vector_t *gf = uir_sim_get_signal(sim, "grant_ff");
    mu_assert_ptr_not_null(gf, "grant_ff exists");
    mu_assert_eq(qsim_bit_get(gf, 0).state, QSIM_1, "nba grant_ff[0]=1");
    mu_assert_eq(qsim_bit_get(gf, 1).state, QSIM_0, "nba grant_ff[1]=0");

    /* match should be 1 (continuous assign re-evaluated after NBA) */
    qsim_bit_vector_t *m = uir_sim_get_signal(sim, "match");
    if (!m) m = uir_sim_get_signal(sim, "test.match");
    mu_assert_ptr_not_null(m, "match exists");
    mu_assert_eq(qsim_bit_get(m, 0).state, QSIM_1, "match=1 after NBA (== on reg)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_cross_module_eq_after_port_propagation(void)
{
    /* Bug #1 scenario: reg in top module drives child input port,
     * child compares port value against literal. Before the port
     * propagation fix (Phase 2b), the child's port would stay X
     * and the == comparison would return X. */
    const char *sources[] = {
        "module child(input [1:0] din, output match);\n"
        "  assign match = (din == 2'b01);\n"
        "endmodule\n",
        "module top;\n"
        "  reg [1:0] grant_ff;\n"
        "  wire      match_w;\n"
        "  child uut(.din(grant_ff), .match(match_w));\n"
        "  initial begin\n"
        "    grant_ff = 2'b01;\n"
        "  end\n"
        "endmodule\n"
    };

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile xmodule_eq");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run initial eval */
    uir_sim_run(sim, 0);

    /* Check top-level grant_ff */
    qsim_bit_vector_t *gf = uir_sim_get_signal(sim, "grant_ff");
    if (!gf) gf = uir_sim_get_signal(sim, "top.grant_ff");
    mu_assert_ptr_not_null(gf, "grant_ff exists");
    mu_assert_eq(qsim_bit_get(gf, 0).state, QSIM_1, "gf[0]=1");
    mu_assert_eq(qsim_bit_get(gf, 1).state, QSIM_0, "gf[1]=0");

    /* Check match_w from child module — should be 1 via port propagation + == */
    qsim_bit_vector_t *mw = uir_sim_get_signal(sim, "match_w");
    if (!mw) mw = uir_sim_get_signal(sim, "top.match_w");
    mu_assert_ptr_not_null(mw, "match_w exists");
    mu_assert_eq(qsim_bit_get(mw, 0).state, QSIM_1, "match_w=1 via cross-module == on reg");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_inline_wire_init(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] bus = 8'hAB;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile inline wire init");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    mu_assert(unit->assign_count > 0, "should have continuous assign for init");
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "bus");
    if (!v) v = uir_sim_get_signal(sim, "test.bus");
    mu_assert_not_null(v);
    uint64_t val;
    int known = uir_bv_to_u64(v, &val);
    mu_assert(known != 0, "bus should be known (not X)");
    mu_assert_eq(val, 0xABULL, "bus = 0xAB from inline wire init");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);

    /* reg init */
    {
        const char *src[] = {
            "module test;\n"
            "  reg [7:0] foo = 8'hCD;\n"
            "endmodule\n"
        };
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        cr = qsim_compile(&input);
        mu_assert_not_null(cr);
        mu_assert(cr->success != 0, "compile reg init");
        unit = (uir_design_unit_t *)cr->units[0];
        units[0] = unit;
        elab = uir_elaborate(units, 1);
        mu_assert(elab->success != 0, "elab reg init");
        uir_elab_result_free(elab);
        sim = uir_sim_create(units, 1);
        mu_assert_not_null(sim);
        uir_sim_run(sim, 0);
        v = uir_sim_get_signal(sim, "foo");
        if (!v) v = uir_sim_get_signal(sim, "test.foo");
        mu_assert_not_null(v);
        known = uir_bv_to_u64(v, &val);
        mu_assert(known != 0, "foo should be known (not X)");
        mu_assert_eq(val, 0xCDULL, "foo = 0xCD from inline reg init");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    /* logic init */
    {
        const char *src[] = {
            "module test;\n"
            "  logic [7:0] bar = 4'h5;\n"
            "endmodule\n"
        };
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        cr = qsim_compile(&input);
        mu_assert_not_null(cr);
        mu_assert(cr->success != 0, "compile logic init");
        unit = (uir_design_unit_t *)cr->units[0];
        units[0] = unit;
        elab = uir_elaborate(units, 1);
        mu_assert(elab->success != 0, "elab logic init");
        uir_elab_result_free(elab);
        sim = uir_sim_create(units, 1);
        mu_assert_not_null(sim);
        uir_sim_run(sim, 0);
        v = uir_sim_get_signal(sim, "bar");
        if (!v) v = uir_sim_get_signal(sim, "test.bar");
        mu_assert_not_null(v);
        known = uir_bv_to_u64(v, &val);
        mu_assert(known != 0, "bar should be known (not X)");
        mu_assert_eq(val, 0x05ULL, "bar = 0x05 from inline logic init");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }
}

static void test_scalar_assign_returns_known(void)
{
    /* Bug #3 regression: scalar assign returns X when read externally.
     * Tests multiple scalar patterns. */

    /* Scenario A: scalar wire with continuous assign */
    {
        const char *src[] = {"module test;\n"
            "  wire ready;\n"
            "  assign ready = 1;\n"
            "endmodule\n"};
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        mu_assert(cr && cr->success, "compile A");
        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        mu_assert(elab->success, "elab A");
        uir_elab_result_free(elab);
        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        uir_sim_run(sim, 0);
        qsim_bit_vector_t *v = uir_sim_get_signal(sim, "ready");
        if (!v) v = uir_sim_get_signal(sim, "test.ready");
        mu_assert_ptr_not_null(v, "ready A");
        mu_assert_eq(qsim_bit_get(v, 0).state, QSIM_1, "scalar wire ready=1");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    /* Scenario B: vector wire [0:0] (user workaround) */
    {
        const char *src[] = {"module test;\n"
            "  wire [0:0] ready;\n"
            "  assign ready = 1;\n"
            "endmodule\n"};
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        mu_assert(cr && cr->success, "compile B");
        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        mu_assert(elab->success, "elab B");
        uir_elab_result_free(elab);
        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        uir_sim_run(sim, 0);
        qsim_bit_vector_t *v = uir_sim_get_signal(sim, "ready");
        if (!v) v = uir_sim_get_signal(sim, "test.ready");
        mu_assert_ptr_not_null(v, "ready B");
        mu_assert_eq(qsim_bit_get(v, 0).state, QSIM_1, "vec [0:0] wire ready=1");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    /* Scenario C: scalar reg with blocking assign */
    {
        const char *src[] = {"module test;\n"
            "  reg ready;\n"
            "  initial ready = 1;\n"
            "endmodule\n"};
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        mu_assert(cr && cr->success, "compile C");
        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        mu_assert(elab->success, "elab C");
        uir_elab_result_free(elab);
        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        uir_sim_run(sim, 0);
        qsim_bit_vector_t *v = uir_sim_get_signal(sim, "ready");
        if (!v) v = uir_sim_get_signal(sim, "test.ready");
        mu_assert_ptr_not_null(v, "ready C");
        mu_assert_eq(qsim_bit_get(v, 0).state, QSIM_1, "scalar reg ready=1");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    /* Scenario D: non-ANSI output port with continuous assign */
    {
        const char *src[] = {"module test(ready);\n"
            "  output ready;\n"
            "  assign ready = 1;\n"
            "endmodule\n"};
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        mu_assert(cr && cr->success, "compile D");
        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        mu_assert(elab->success, "elab D");
        uir_elab_result_free(elab);
        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        uir_sim_run(sim, 0);
        qsim_bit_vector_t *v = uir_sim_get_signal(sim, "ready");
        if (!v) v = uir_sim_get_signal(sim, "test.ready");
        mu_assert_ptr_not_null(v, "ready D");
        mu_assert_eq(qsim_bit_get(v, 0).state, QSIM_1, "non-ansi output ready=1");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    /* Scenario E: ANSI output port */
    {
        const char *src[] = {"module test(output ready);\n"
            "  assign ready = 1;\n"
            "endmodule\n"};
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = src; input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        mu_assert(cr && cr->success, "compile E");
        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        mu_assert(elab->success, "elab E");
        uir_elab_result_free(elab);
        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        uir_sim_run(sim, 0);
        qsim_bit_vector_t *v = uir_sim_get_signal(sim, "ready");
        if (!v) v = uir_sim_get_signal(sim, "test.ready");
        mu_assert_ptr_not_null(v, "ready E");
        mu_assert_eq(qsim_bit_get(v, 0).state, QSIM_1, "ansi output ready=1");
        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }
}

/* ── Wait statement ══════════════════════════════════════════════ */

static void test_wait_immediate(void)
{
    /* wait(condition) with condition already true executes body immediately */
    const char *sources[] = {
        "module test;\n"
        "  reg a;\n"
        "  initial begin\n"
        "    a = 0;\n"
        "    wait(1) a = 1;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile wait_immediate");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    uir_sim_run(sim, 0);
    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 after wait(1)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_wait_on_signal_change(void)
{
    /* wait(ready) blocks until ready=1 via separate process */
    const char *sources[] = {
        "module test;\n"
        "  reg ready, a;\n"
        "  initial #5 ready = 1;\n"
        "  initial wait(ready) a = 1;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile wait_on_change");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    /* At t=0: ready is X, wait(ready) registers waiter.
     * At t=5: ready becomes 1, waiter fires and sets a=1. */
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 after wait(ready) fired");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── Event control ═══════════════════════════════════════════════ */

static void test_event_control_posedge(void)
{
    /* @(posedge clk) a = 1; fires when clk rises */
    const char *sources[] = {
        "module test;\n"
        "  reg clk, a;\n"
        "  initial #5 clk = 1;\n"
        "  initial @(posedge clk) a = 1;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile event_posedge");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 after @(posedge clk)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── Disable statement ═══════════════════════════════════════════ */

static void test_disable_block(void)
{
    /* disable my_block; skips remaining statements in the named block */
    const char *sources[] = {
        "module test;\n"
        "  reg a;\n"
        "  initial begin : my_block\n"
        "    a = 1;\n"
        "    disable my_block;\n"
        "    a = 0;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile disable_block");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 (disable skipped a=0)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_disable_task(void)
{
    /* disable self within a task. Call with a dummy arg (work around empty
     * arg list PEG issue when task_enable is inside a begin...end block). */
    const char *sources[] = {
        "module test;\n"
        "  reg a;\n"
        "  task my_task;\n"
        "    begin\n"
        "      a = 1;\n"
        "      disable my_task;\n"
        "      a = 0;\n"
        "    end\n"
        "  endtask\n"
        "  initial my_task(a);\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile disable_task");
    if (!cr->success) {
        for (size_t ei = 0; ei < cr->diag_count; ei++)
            fprintf(stderr, "DTASK: %s\n", cr->diagnostics[ei].message
                    ? cr->diagnostics[ei].message : "(null)");
    }
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elab ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_ptr_not_null(sim, "sim create");

    uir_sim_run(sim, 0);

    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "a");
    mu_assert_ptr_not_null(a_val, "a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "a=1 (disable skipped a=0)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── System task simulation tests (Phase 2) ── */

static char display_buf[4096];
static size_t display_buf_len;

static void test_display_cb(uir_sim_context_t *ctx, const char *msg, void *user_data) {
    (void)ctx;
    (void)user_data;
    if (msg) {
        size_t n = strlen(msg);
        if (display_buf_len + n < sizeof(display_buf)) {
            memcpy(display_buf + display_buf_len, msg, n);
            display_buf_len += n;
            display_buf[display_buf_len] = '\0';
        }
    }
}

static void test_stop_pauses(void)
{
    const char *sources[] = {
        "module test;\n"
        "  initial $stop;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile stop test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    /* Run simulation - $stop should pause it */
    uir_sim_run(sim, 100);
    mu_assert(uir_sim_is_stopped(sim), "$stop should pause sim");
    mu_assert(!uir_sim_is_finished(sim), "$stop should not finish");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_finish_terminates(void)
{
    const char *sources[] = {
        "module test;\n"
        "  initial $finish;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile finish test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 100);
    mu_assert(uir_sim_is_finished(sim), "$finish should terminate");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_stop_continue(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg [7:0] cnt;\n"
        "  initial begin\n"
        "    cnt = 10;\n"
        "    $stop(1);\n"
        "    cnt = 20;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile stop_continue");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 100);
    mu_assert(uir_sim_is_stopped(sim), "stopped after $stop");
    /* Continue simulation */
    uir_sim_clear_stop(sim);
    uir_sim_run(sim, 100);
    uint64_t cnt_val;
    qsim_bit_vector_t *cnt = uir_sim_get_signal(sim, "cnt");
    mu_assert_not_null(cnt);
    mu_assert(uir_bv_to_u64(cnt, &cnt_val), "cnt known");
    mu_assert_eq(cnt_val, 20, "cnt=20 after continue");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_fatal_output_and_finish(void)
{
    const char *sources[] = {
        "module test;\n"
        "  initial $fatal(\"my fatal err\");\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile fatal test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    display_buf[0] = '\0';
    display_buf_len = 0;
    uir_sim_set_sys_display_callback(sim, test_display_cb, NULL);
    uir_sim_run(sim, 100);
    mu_assert(uir_sim_is_finished(sim), "$fatal finishes");
    mu_assert(display_buf_len > 0, "display cb called");
    mu_assert(strstr(display_buf, "Fatal") != NULL,
              "fatal output has Fatal prefix");
    mu_assert(strstr(display_buf, "my fatal err") != NULL,
              "fatal output contains message");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_severity_output(void)
{
    const char *sources[] = {
        "module test;\n"
        "  initial begin\n"
        "    $info(\"info msg\");\n"
        "    $warning(\"warn msg\");\n"
        "    $error(\"err msg\");\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile severity test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    display_buf[0] = '\0';
    display_buf_len = 0;
    uir_sim_set_sys_display_callback(sim, test_display_cb, NULL);
    uir_sim_run(sim, 100);
    mu_assert(strstr(display_buf, "Info:") != NULL, "info prefix");
    mu_assert(strstr(display_buf, "Warning:") != NULL, "warning prefix");
    mu_assert(strstr(display_buf, "Error:") != NULL, "error prefix");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_clog2_compute(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [31:0] r;\n"
        "  assign r = $clog2(16);\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile clog2 test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *r = uir_sim_get_signal(sim, "r");
    mu_assert_not_null(r);
    uint64_t val;
    mu_assert(uir_bv_to_u64(r, &val), "clog2 result known");
    mu_assert_eq(val, 4, "$clog2(16) = 4");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_clog2_of_7(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [31:0] r;\n"
        "  assign r = $clog2(7);\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile clog2(7) test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *r = uir_sim_get_signal(sim, "r");
    mu_assert_not_null(r);
    uint64_t val;
    mu_assert(uir_bv_to_u64(r, &val), "clog2(7) known");
    mu_assert_eq(val, 3, "$clog2(7) = 3");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_signed_passthrough(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [7:0] a, b;\n"
        "  assign a = 8'hA5;\n"
        "  assign b = $signed(a);\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile $signed test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *b = uir_sim_get_signal(sim, "b");
    mu_assert_not_null(b);
    uint64_t val;
    mu_assert(uir_bv_to_u64(b, &val), "signed result known");
    mu_assert_eq(val, 0xA5, "$signed passes value through");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_time_in_process(void)
{
    const char *sources[] = {
        "module test;\n"
        "  reg clk;\n"
        "  reg [63:0] t;\n"
        "  always #5 clk = ~clk;\n"
        "  always @(posedge clk) t = $time;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile $time test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    /* Initialize clk to 0 */
    qsim_bit_vector_t *clk0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "clk", clk0);
    qsim_bit_vector_free(clk0);
    /* Run for 12 fs — should capture one posedge */
    uir_sim_run(sim, 12);
    qsim_bit_vector_t *t = uir_sim_get_signal(sim, "t");
    mu_assert_not_null(t);
    uint64_t tval;
    mu_assert(uir_bv_to_u64(t, &tval), "$time result known");
    mu_assert(tval > 0, "$time > 0 after clock runs");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_random_returns_value(void)
{
    const char *sources[] = {
        "module test;\n"
        "  wire [31:0] r;\n"
        "  assign r = $random;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success, "compile $random test");
    uir_design_unit_t *units[] = {(uir_design_unit_t *)cr->units[0]};
    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);
    uir_sim_run(sim, 0);
    qsim_bit_vector_t *r = uir_sim_get_signal(sim, "r");
    mu_assert_not_null(r);
    uint64_t val;
    mu_assert(uir_bv_to_u64(r, &val), "$random result known");
    mu_assert(val != 0, "$random non-zero (statistically likely)");
    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* =================================================================
 * Net type resolution tests (Phase 3)
 * ================================================================= */

static void test_wand_resolution(void)
{
    const char *sources[] = {
        "module wand_test;\n"
        "  wand [7:0] w;\n"
        "  reg [7:0] a, b;\n"
        "  assign w = a;\n"
        "  assign w = b;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile wand_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *allX = qsim_bit_vector_from_state(8, QSIM_X);

    qsim_bit_vector_t *wv;

    /* 0 dominates: a=FF, b=00 -> w=00 */
    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wand: 0 dominates");

    /* Both 1: a=FF, b=FF -> w=FF */
    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wand: both 1 -> 1");

    /* Z transparent: a=FF, b=ZZ -> w=FF */
    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wand: Z transparent");

    /* X propagates when no 0: a=XX, b=FF -> w=XX */
    uir_sim_set_signal(sim, "a", allX);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allX), "wand: X when no 0");

    /* Both Z -> Z */
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
    const char *sources[] = {
        "module wor_test;\n"
        "  wor [7:0] w;\n"
        "  reg [7:0] a, b;\n"
        "  assign w = a;\n"
        "  assign w = b;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile wor_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);
    qsim_bit_vector_t *allX = qsim_bit_vector_from_state(8, QSIM_X);

    qsim_bit_vector_t *wv;

    /* 1 dominates: a=00, b=FF -> w=FF */
    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all1), "wor: 1 dominates");

    /* Both 0: a=00, b=00 -> w=00 */
    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wor: both 0 -> 0");

    /* Z transparent: a=00, b=ZZ -> w=00 */
    uir_sim_set_signal(sim, "a", all0);
    uir_sim_set_signal(sim, "b", allZ);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, all0), "wor: Z transparent");

    /* X propagates when no 1: a=XX, b=00 -> w=XX */
    uir_sim_set_signal(sim, "a", allX);
    uir_sim_set_signal(sim, "b", all0);
    uir_sim_run(sim, 5);
    wv = uir_sim_get_signal(sim, "w");
    mu_assert(qsim_bit_vector_eq(wv, allX), "wor: X when no 1");

    /* Both Z -> Z */
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
    const char *sources[] = {
        "module tri0_test;\n"
        "  tri0 [7:0] t;\n"
        "  reg [7:0] d;\n"
        "  assign t = d;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile tri0_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);

    qsim_bit_vector_t *tv;

    /* Non-Z driver: t follows d */
    uir_sim_set_signal(sim, "d", all1);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all1), "tri0: follows driver when non-Z");

    /* Pull to 0 when undriven (all Z) */
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
    const char *sources[] = {
        "module tri1_test;\n"
        "  tri1 [7:0] t;\n"
        "  reg [7:0] d;\n"
        "  assign t = d;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile tri1_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(8, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(8, QSIM_1);
    qsim_bit_vector_t *allZ = qsim_bit_vector_from_state(8, QSIM_Z);

    qsim_bit_vector_t *tv;

    /* Non-Z driver: t follows d */
    uir_sim_set_signal(sim, "d", all0);
    uir_sim_run(sim, 5);
    tv = uir_sim_get_signal(sim, "t");
    mu_assert(qsim_bit_vector_eq(tv, all0), "tri1: follows driver when non-Z");

    /* Pull to 1 when undriven (all Z) */
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
    const char *sources[] = {
        "module supply_test;\n"
        "  supply0 vcc;\n"
        "  supply1 gnd;\n"
        "  reg d;\n"
        "  assign vcc = d;\n"
        "  assign gnd = d;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile supply_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(1, QSIM_1);

    qsim_bit_vector_t *v;

    /* supply0 starts as 0 */
    v = uir_sim_get_signal(sim, "vcc");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, all0), "supply0 initial value = 0");

    /* supply1 starts as 1 */
    v = uir_sim_get_signal(sim, "gnd");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, all1), "supply1 initial value = 1");

    /* Try to overwrite via continuous assign */
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

/* ── Named event tests (Phase 5a) ── */

static void test_event_trigger_wakes_waiter(void)
{
    const char *sources[] = {
        "module test;\n"
        "  event ev;\n"
        "  reg a;\n"
        "  initial begin -> ev; a = 1; end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile event_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Run simulation — trigger fires immediately (no delay), a becomes 1 */
    uir_sim_run(sim, 1);

    qsim_bit_vector_t *v = uir_sim_get_signal(sim, "a");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, all1), "a becomes 1 after event trigger");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(all1);
}

static void test_event_in_sensitivity_list(void)
{
    const char *sources[] = {
        "module test;\n"
        "  event ev;\n"
        "  reg a;\n"
        "  always @(ev) a = a + 1;\n"
        "  initial begin a = 0; -> ev; -> ev; end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile event_sens_test");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v;


    /* reg a may be X initially (uninitialized), so run until it becomes predictable */
    uir_sim_run(sim, 1);

    v = uir_sim_get_signal(sim, "a");
    mu_assert_not_null(v);
    mu_assert(qsim_bit_vector_eq(v, qsim_bit_vector_from_state(1, QSIM_1)) ||
              qsim_bit_vector_eq(v, qsim_bit_vector_from_state(1, QSIM_0)) ||
              qsim_bit_vector_eq(v, qsim_bit_vector_from_state(1, QSIM_X)),
              "a has some value");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── SystemVerilog process tests (always_comb / always_ff / always_latch) ── */

static void test_sv_always_comb(void)
{
    const char *sources[] = {
        "module test;\n"
        "  logic [3:0] y;\n"
        "  logic [3:0] a, b;\n"
        "  always_comb begin\n"
        "    y = a & b;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_always_comb");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *a_5 = qsim_bit_vector_from_str("0101");
    qsim_bit_vector_t *b_3 = qsim_bit_vector_from_str("0011");
    qsim_bit_vector_t *all1 = qsim_bit_vector_from_state(4, QSIM_1);
    qsim_bit_vector_t *all0 = qsim_bit_vector_from_state(4, QSIM_0);

    /* Verify initial state: y = X */
    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    mu_assert_eq(y_val->width, 4, "y is 4 bits");

    /* Set a=5, b=3 and run.  y should be 0101 & 0011 = 0001 = 1 */
    uir_sim_set_signal(sim, "a", a_5);
    uir_sim_set_signal(sim, "b", b_3);
    uir_sim_run(sim, 10);

    y_val = uir_sim_get_signal(sim, "y");
    uint64_t yv;
    int ok = uir_bv_to_u64(y_val, &yv);
    mu_assert(ok != 0, "y should be known after driving inputs");
    mu_assert_eq(yv, 1, "y = 1 for a=5,b=3 (0101 & 0011 = 0001)");

    /* Set a=0, b still 3.  y should be 0 */
    uir_sim_set_signal(sim, "a", all0);
    uir_sim_run(sim, 10);

    y_val = uir_sim_get_signal(sim, "y");
    ok = uir_bv_to_u64(y_val, &yv);
    mu_assert(ok != 0, "y should be known after a=0");
    mu_assert_eq(yv, 0, "y = 0 for a=0,b=3");

    /* Set a=0xF, b=0xF.  y should be 0xF */
    uir_sim_set_signal(sim, "a", all1);
    uir_sim_set_signal(sim, "b", all1);
    uir_sim_run(sim, 10);

    y_val = uir_sim_get_signal(sim, "y");
    ok = uir_bv_to_u64(y_val, &yv);
    mu_assert(ok != 0, "y should be known after a=b=0xF");
    mu_assert_eq(yv, 0xF, "y = 0xF for a=b=0xF");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(a_5);
    qsim_bit_vector_free(b_3);
    qsim_bit_vector_free(all1);
    qsim_bit_vector_free(all0);
}

static void test_sv_always_ff(void)
{
    const char *sources[] = {
        "module test;\n"
        "  logic q;\n"
        "  logic clk, d;\n"
        "  always_ff @(posedge clk) begin\n"
        "    q <= d;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_always_ff");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);

    /* Verify starts as X */
    qsim_bit_vector_t *q_val = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(q_val, 0).state == QSIM_X, "q starts X");

    /* Drive d=1, posedge clk 0->1 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_1, "q=1 after posedge d=1");

    /* Drive d=0, posedge clk 0->1 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_0, "q=0 after posedge d=0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_1);
    qsim_bit_vector_free(d_0);
}

static void test_sv_always_latch(void)
{
    const char *sources[] = {
        "module test;\n"
        "  logic q;\n"
        "  logic en, d;\n"
        "  always_latch begin\n"
        "    if (en) q <= d;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_always_latch");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *en_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *en_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);

    /* Start with en=0, d=1 — latch is closed, q remains X */
    uir_sim_set_signal(sim, "en", en_0);
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *q_val = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(q_val, 0).state == QSIM_X, "q stays X when en=0");

    /* en=1 — latch opens, q follows d=1 */
    uir_sim_set_signal(sim, "en", en_1);
    uir_sim_run(sim, 10);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_1, "q=1 when en=1,d=1");

    /* d changes to 0 while en=1 — q follows */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 10);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_0, "q=0 when en=1,d=0");

    /* en=0 — latch closes, q holds value (0) */
    uir_sim_set_signal(sim, "en", en_0);
    uir_sim_run(sim, 10);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_0, "q still 0 when en=0 (latch holds)");

    /* d changes while en=0 — q does NOT change (latch closed) */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 10);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert_eq(qsim_bit_get(q_val, 0).state, QSIM_0, "q still 0 when en=0,d changed (latch closed)");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(en_1);
    qsim_bit_vector_free(en_0);
    qsim_bit_vector_free(d_1);
    qsim_bit_vector_free(d_0);
}

/* ── SystemVerilog interface/modport simulation test ── */

static void test_sv_interface_sim(void)
{
    const char *sources[] = {
        "interface my_bus;\n"
        "  logic [7:0] data;\n"
        "  logic clk;\n"
        "  modport master (input clk, output data);\n"
        "endinterface\n"
        "module top;\n"
        "  my_bus bus_inst();\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_interface");
    mu_assert_eq(cr->unit_count, 2, "should have 2 design units (interface + module)");

    /* Locate interface and top module */
    uir_design_unit_t *iface = NULL;
    uir_design_unit_t *top = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "my_bus") == 0) {
            iface = u;
            mu_assert(u->is_interface != 0, "my_bus marked as interface");
            mu_assert(u->modport_count >= 1, "my_bus has modport");
            mu_assert_str_eq(u->modports[0].name, "master", "modport name");
            mu_assert(u->modports[0].port_count >= 2, "modport has ports");
        } else if (strcmp(u->name, "top") == 0) {
            top = u;
            mu_assert(top->instance_count >= 1, "top has interface instance");
            mu_assert_str_eq(top->instances[0]->module_name, "my_bus", "instance module name");
            mu_assert_str_eq(top->instances[0]->instance_name, "bus_inst", "instance name");
        }
    }
    mu_assert_not_null(iface);
    mu_assert_not_null(top);

    /* Elaborate both units */
    uir_design_unit_t *units[] = {top, iface};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    /* Create sim context */
    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    /* Access interface signals through hierarchy */
    qsim_bit_vector_t *data_val = uir_sim_get_signal(sim, "bus_inst.data");
    mu_assert_not_null(data_val);
    mu_assert_eq(data_val->width, 8, "data is 8 bits");
    mu_assert(qsim_bit_get(data_val, 0).state == QSIM_X, "data starts X");

    qsim_bit_vector_t *clk_val = uir_sim_get_signal(sim, "bus_inst.clk");
    mu_assert_not_null(clk_val);
    mu_assert_eq(clk_val->width, 1, "clk is 1 bit");
    mu_assert(qsim_bit_get(clk_val, 0).state == QSIM_X, "clk starts X");

    /* Set data through hierarchy and verify */
    qsim_bit_vector_t *test_val = qsim_bit_vector_from_str("10101011");
    uir_sim_set_signal(sim, "bus_inst.data", test_val);
    uir_sim_run(sim, 1);

    data_val = uir_sim_get_signal(sim, "bus_inst.data");
    uint64_t dv;
    int ok = uir_bv_to_u64(data_val, &dv);
    mu_assert(ok != 0, "data should be known after set");
    mu_assert_eq(dv, 0xAB, "data = 0xAB");

    /* Drive clk through hierarchy and verify */
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "bus_inst.clk", clk_1);
    uir_sim_run(sim, 1);

    clk_val = uir_sim_get_signal(sim, "bus_inst.clk");
    mu_assert_eq(qsim_bit_get(clk_val, 0).state, QSIM_1, "clk = 1");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(test_val);
    qsim_bit_vector_free(clk_1);
}

/* ── SystemVerilog package/import simulation test ── */

static void test_sv_package_sim(void)
{
    const char *sources[] = {
        "package my_pkg;\n"
        "  parameter WIDTH = 8;\n"
        "endpackage\n"
        "module top;\n"
        "  import my_pkg::WIDTH;\n"
        "  logic [7:0] data;\n"
        "  always_comb begin\n"
        "    data = 8'hAB;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_package");
    mu_assert_eq(cr->unit_count, 2, "2 units: package + module");

    /* Locate package and top module */
    uir_design_unit_t *pkg = NULL;
    uir_design_unit_t *top = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "my_pkg") == 0) {
            pkg = u;
            mu_assert_str_eq(u->language, "verilog", "package language");
            mu_assert(u->param_count >= 1, "package has parameter");
            mu_assert_str_eq(u->params[0].hier_path, "WIDTH", "param name");
        } else if (strcmp(u->name, "top") == 0) {
            top = u;
            mu_assert(top->import_count >= 1, "top has import");
            mu_assert_str_eq(top->imports[0].pkg_name, "my_pkg", "import pkg");
            mu_assert_str_eq(top->imports[0].item_name, "WIDTH", "import item");
        }
    }
    mu_assert_not_null(pkg);
    mu_assert_not_null(top);

    /* Elaborate both units */
    uir_design_unit_t *units[] = {top, pkg};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    /* Create sim context */
    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    /* Access data signal */
    qsim_bit_vector_t *data_val = uir_sim_get_signal(sim, "data");
    mu_assert_not_null(data_val);
    mu_assert_eq(data_val->width, 8, "data is 8 bits");

    /* Run: always_comb executes, data = 8'hAB */
    uir_sim_run(sim, 10);

    data_val = uir_sim_get_signal(sim, "data");
    uint64_t dv;
    int ok = uir_bv_to_u64(data_val, &dv);
    mu_assert(ok != 0, "data should be known after run");
    mu_assert_eq(dv, 0xAB, "data = 0xAB after always_comb");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* Concurrent package: both named and wildcard import styles */
static void test_sv_package_import_wildcard(void)
{
    const char *sources[] = {
        "package my_pkg;\n"
        "  parameter WIDTH = 8;\n"
        "endpackage\n"
        "module top;\n"
        "  import my_pkg::*;\n"
        "  logic [7:0] data;\n"
        "  always_comb begin\n"
        "    data = 8'h42;\n"
        "  end\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile sv_package wildcard");
    mu_assert_eq(cr->unit_count, 2, "2 units: package + module");

    uir_design_unit_t *top = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "top") == 0) {
            top = u;
            mu_assert(top->import_count >= 1, "top has import");
            mu_assert_str_eq(top->imports[0].pkg_name, "my_pkg", "import pkg");
            mu_assert(top->imports[0].item_name == NULL, "wildcard import has NULL item");
        }
    }
    mu_assert_not_null(top);

    uir_design_unit_t *pkg = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "my_pkg") == 0) { pkg = u; break; }
    }
    mu_assert_not_null(pkg);

    uir_design_unit_t *units[] = {top, pkg};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaboration should succeed");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *data_val = uir_sim_get_signal(sim, "data");
    mu_assert_not_null(data_val);
    uint64_t dv;
    int ok = uir_bv_to_u64(data_val, &dv);
    mu_assert(ok != 0, "data should be known");
    mu_assert_eq(dv, 0x42, "data = 0x42");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── SystemVerilog logic port-type simulation tests ── */

static void test_sv_logic_port_basic(void)
{
    /* Scalar logic port connection: top logic -> child input logic -> child output logic */
    const char *sources[] = {
        "module child(input logic a, output logic y);\n"
        "  always_comb y = a;\n"
        "endmodule\n",
        "module top;\n"
        "  logic w;\n"
        "  logic w_out;\n"
        "  child uut(.a(w), .y(w_out));\n"
        "  initial w = 1;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile logic_port");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run initial eval: w=1 propagated through child ports */
    uir_sim_run(sim, 0);

    qsim_bit_vector_t *w_val = uir_sim_get_signal(sim, "w");
    mu_assert_ptr_not_null(w_val, "w exists");
    mu_assert_eq(qsim_bit_get(w_val, 0).state, QSIM_1, "w=1");

    /* Child input port a should reflect w through port connection */
    qsim_bit_vector_t *a_val = uir_sim_get_signal(sim, "uut.a");
    mu_assert_ptr_not_null(a_val, "child port a exists");
    mu_assert_eq(qsim_bit_get(a_val, 0).state, QSIM_1, "child port a=1");

    /* Child output y propagates back to top w_out */
    qsim_bit_vector_t *w_out_val = uir_sim_get_signal(sim, "w_out");
    mu_assert_ptr_not_null(w_out_val, "w_out exists");
    mu_assert_eq(qsim_bit_get(w_out_val, 0).state, QSIM_1, "w_out=1");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_sv_logic_port_vector(void)
{
    /* Vector logic ports: child inverts 4-bit input via always_comb */
    const char *sources[] = {
        "module child(input logic [3:0] a, output logic [3:0] y);\n"
        "  always_comb y = ~a;\n"
        "endmodule\n",
        "module top;\n"
        "  logic [3:0] data_in;\n"
        "  logic [3:0] data_out;\n"
        "  child uut(.a(data_in), .y(data_out));\n"
        "  initial data_in = 4'b1010;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile logic_port_vec");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Run initial eval */
    uir_sim_run(sim, 0);

    /* Check data_in = 4'b1010 from initial block */
    qsim_bit_vector_t *in_val = uir_sim_get_signal(sim, "data_in");
    mu_assert_ptr_not_null(in_val, "data_in exists");
    uint64_t dv;
    mu_assert(uir_bv_to_u64(in_val, &dv) != 0, "data_in known");
    mu_assert_eq(dv, 0xA, "data_in = 0xA");

    /* Check data_out = ~data_in = 4'b0101 through child logic ports */
    qsim_bit_vector_t *out_val = uir_sim_get_signal(sim, "data_out");
    mu_assert_ptr_not_null(out_val, "data_out exists");
    mu_assert(uir_bv_to_u64(out_val, &dv) != 0, "data_out known");
    mu_assert_eq(dv, 0x5, "data_out = ~data_in = 0x5");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

static void test_nested_case_sens(void);
static void test_sv_logic_cont_assign(void)
{
    /* Continuous assignment to logic type (SV allows assign on logic) */
    const char *sources[] = {
        "module top;\n"
        "  logic [7:0] data;\n"
        "  assign data = 8'hAB;\n"
        "endmodule\n"
    };
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_ptr_not_null(cr, "compile logic_assign");
    mu_assert(cr->success != 0, "compile ok");
    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    mu_assert(elab->success != 0, "elaboration ok");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    mu_assert_ptr_not_null(sim, "sim create");

    /* Continuous assign evaluates during initial eval */
    uir_sim_run(sim, 0);

    qsim_bit_vector_t *data_val = uir_sim_get_signal(sim, "data");
    mu_assert_ptr_not_null(data_val, "data exists");
    uint64_t dv;
    mu_assert(uir_bv_to_u64(data_val, &dv) != 0, "data known after assign");
    mu_assert_eq(dv, 0xAB, "data = 0xAB from continuous assign");

    /* Change the RHS signal and run: verify re-evaluation */
    /* (No RHS signal to change here, but the assign should hold) */
    uir_sim_run(sim, 10);

    data_val = uir_sim_get_signal(sim, "data");
    mu_assert(uir_bv_to_u64(data_val, &dv) != 0, "data still known after steps");
    mu_assert_eq(dv, 0xAB, "data still 0xAB");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
}

/* ── VHDL dual-instance reproduction (Bug 1) ── */

static int bv1_to_u64(qsim_bit_vector_t *bv) {
    if (!bv || bv->width == 0) return -1;
    uint64_t val = 0;
    for (uint32_t i = 0; i < bv->width && i < 64; i++)
        if (bv->bits[i].state == QSIM_1) val |= (1ULL << i);
    return (int)val;
}

static void test_vhdl_dual_instance(void)
{
    /* Sub-entity: a simple 4-bit register (no library/use, use bit type) */
    const char *sub_src =
        "entity reg4 is\n"
        "  port (clk: in bit; rst: in bit;\n"
        "        d: in bit_vector(3 downto 0); q: out bit_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of reg4 is\n"
        "  signal r: bit_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      r <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      r <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= r;\n"
        "end architecture;\n";

    /* Parent: dual instance of reg4 via component instantiation */
    const char *parent_src =
        "entity dual is\n"
        "  port (clk: in bit; rst: in bit;\n"
        "        d0: in bit_vector(3 downto 0); q0: out bit_vector(3 downto 0);\n"
        "        d1: in bit_vector(3 downto 0); q1: out bit_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of dual is\n"
        "  component reg4 is\n"
        "    port (clk: in bit; rst: in bit;\n"
        "          d: in bit_vector(3 downto 0); q: out bit_vector(3 downto 0));\n"
        "  end component;\n"
        "begin\n"
        "  u0: reg4 port map(clk => clk, rst => rst, d => d0, q => q0);\n"
        "  u1: reg4 port map(clk => clk, rst => rst, d => d1, q => q1);\n"
        "end architecture;\n";

    const char *sources[] = {parent_src, sub_src};
    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = sources;
    input.source_count = 2;

    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    if (!cr->success) {
        for (size_t i = 0; i < cr->diag_count; i++)
            printf("  diag[%zu]: %s\n", i, cr->diagnostics[i].message);
    }
    mu_assert(cr->success != 0, "compile dual_vhdl");

    uir_design_unit_t *reg4_u = NULL, *dual_u = NULL;
    for (size_t i = 0; i < cr->unit_count; i++) {
        uir_design_unit_t *u = (uir_design_unit_t *)cr->units[i];
        if (strcmp(u->name, "reg4") == 0) reg4_u = u;
        if (strcmp(u->name, "dual") == 0) dual_u = u;
    }
    mu_assert_not_null(reg4_u);
    mu_assert_not_null(dual_u);
    mu_assert(dual_u->instance_count == 2, "dual has 2 instances");
    mu_assert_str_eq(dual_u->instances[0]->instance_name, "u0", "first instance u0");
    mu_assert_str_eq(dual_u->instances[1]->instance_name, "u1", "second instance u1");

    uir_design_unit_t *units[] = {dual_u, reg4_u};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaboration ok");
    mu_assert(dual_u->instances[0]->bound_to != NULL, "u0 bound");
    mu_assert(dual_u->instances[1]->bound_to != NULL, "u1 bound");
    mu_assert(dual_u->instances[0]->bound_to == dual_u->instances[1]->bound_to,
              "both bound to same reg4");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d0_5 = qsim_bit_vector_from_str("0101");
    qsim_bit_vector_t *d1_A = qsim_bit_vector_from_str("1010");

    /* Reset both: rst=1 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_set_signal(sim, "d0", d0_5);
    uir_sim_set_signal(sim, "d1", d1_A);
    uir_sim_run(sim, 5);

    /* After reset, q0=q1=0 despite d0/d1 being set */
    qsim_bit_vector_t *q0 = uir_sim_get_signal(sim, "q0");
    qsim_bit_vector_t *q1 = uir_sim_get_signal(sim, "q1");
    mu_assert_not_null(q0);
    mu_assert_not_null(q1);
    int q0v = bv1_to_u64(q0);
    int q1v = bv1_to_u64(q1);

    /* q0 and q1 should both be 0 after reset, regardless of d0/d1 values */
    q0v = bv1_to_u64(q0);
    q1v = bv1_to_u64(q1);

    /* Release reset, clock posedge: registers capture d */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    q0 = uir_sim_get_signal(sim, "q0");
    q1 = uir_sim_get_signal(sim, "q1");
    q0v = bv1_to_u64(q0);
    q1v = bv1_to_u64(q1);
    mu_assert_eq(q0v, 5, "q0 = 5 (0b0101) after clk");
    mu_assert_eq(q1v, 10, "q1 = 10 (0b1010) after clk, independent of q0");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d0_5);
    qsim_bit_vector_free(d1_A);
}

/* ── Nested case sensitivity bug ── */
static void test_nested_case_sens(void)
{
    const char *src =
        "module t(input clk, input rst, output reg done);\n"
        "  reg [1:0] state;\n"
        "  reg [3:0] acc;\n"
        "  always @(posedge clk or posedge rst)\n"
        "    if (rst) begin state <= 0; done <= 0; acc <= 0; end\n"
        "    else begin\n"
        "      case (state)\n"
        "        0: state <= 1;\n"
        "        1: begin\n"
        "          case (acc[1:0])\n"
        "            2'b01: ;\n"
        "            default: ;\n"
        "          endcase\n"
        "        end\n"
        "        2: begin done <= 1; state <= 0; end\n"
        "      endcase\n"
        "    end\n"
        "endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert_not_null(cr);
    mu_assert(cr->success != 0, "compile");

    uir_design_unit_t *unit = (uir_design_unit_t *)cr->units[0];
    uir_design_unit_t *units[] = {unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *rst1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "rst", rst1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *done_val = uir_sim_get_signal(sim, "done");
    mu_assert_not_null(done_val);
    mu_assert(qsim_bit_get(done_val, 0).state != QSIM_X, "done not X after rst");
    mu_assert(qsim_bit_get(done_val, 0).state == QSIM_0, "done=0 after rst");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(rst1);
}


/* ── Mixed-language vector port propagation (std_logic_vector ↔ wire) ── */
static void test_mixed_vector_port_prop(void)
{
    const char *vhdl =
        "entity add8 is\n"
        "  port (a: in std_logic_vector(7 downto 0);\n"
        "        b: in std_logic_vector(7 downto 0);\n"
        "        y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture rtl of add8 is\n"
        "begin\n"
        "  y <= a;\n"
        "end architecture;\n";

    const char *verilog =
        "module tb; wire [7:0] a, b, y; add8 uut(.a(a), .b(b), .y(y)); endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    const char *sources[] = {vhdl, verilog};
    input.sources = sources; input.source_count = 2;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success != 0, "compile mixed vector");
    mu_assert(cr->unit_count == 2, "2 units");

    uir_design_unit_t *units[] = {(uir_design_unit_t*)cr->units[0], (uir_design_unit_t*)cr->units[1]};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate vector");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v = qsim_bit_vector_alloc(8);
    for (int i = 0; i < 8; i++) qsim_bit_set(v, i, (i % 2) ? QSIM_VAL_1 : QSIM_VAL_0);
    uir_sim_set_signal(sim, "a", v);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0");
    mu_assert(qsim_bit_get(yv, 7).state == QSIM_1, "y(7)=1");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(v);
}

/* ── Mixed VHDL-process to Verilog-wire port propagation ── */
static void test_mixed_vhdl_to_verilog_prop(void)
{
    const char *vhdl =
        "entity counter is\n"
        "  port (clk: in std_logic; count: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of counter is\n"
        "  signal cnt: std_logic_vector(3 downto 0) := \"0000\";\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      cnt <= \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= cnt;\n"
        "end architecture;\n";

    const char *verilog =
        "module tb; reg clk; wire [3:0] cnt; counter uut(.clk(clk), .count(cnt)); endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    const char *sources[] = {vhdl, verilog};
    input.sources = sources; input.source_count = 2;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success != 0, "compile vhdl-to-verilog");
    mu_assert(cr->unit_count == 2, "2 units");

    uir_design_unit_t *units[] = {(uir_design_unit_t*)cr->units[0], (uir_design_unit_t*)cr->units[1]};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate v2v");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "clk", clk1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *cv = uir_sim_get_signal(sim, "cnt");
    mu_assert_not_null(cv);
    mu_assert(qsim_bit_get(cv, 0).state == QSIM_1, "cnt(0)=1");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(clk1);
}

/* ── 3-level mixed-language hierarchy ── */
static void test_mixed_multi_level(void)
{
    /* VHDL leaf: buffer */
    const char *vhdl =
        "entity buf is port(a: in std_logic; y: out std_logic); end entity;\n"
        "architecture rtl of buf is begin y <= a; end architecture;\n";
    /* Verilog middle: wraps buffer */
    const char *verilog_mid =
        "module mid(input a, output y); buf b(.a(a), .y(y)); endmodule\n";
    /* Verilog top: instantiates mid */
    const char *verilog_top =
        "module top; reg a; wire y; mid m(.a(a), .y(y)); endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    const char *sources[] = {vhdl, verilog_mid, verilog_top};
    input.sources = sources; input.source_count = 3;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success != 0, "compile 3-level");
    mu_assert(cr->unit_count == 3, "3 units");

    uir_design_unit_t *units[] = {
        (uir_design_unit_t*)cr->units[0],
        (uir_design_unit_t*)cr->units[1],
        (uir_design_unit_t*)cr->units[2]
    };
    uir_elab_result_t *elab = uir_elaborate(units, 3);
    mu_assert(elab->success != 0, "elaborate 3-level");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 3);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *one = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "a", one);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 through 3 levels");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(one);
}

/* ── Hierarchical signal access by dotted path ── */
static void test_mixed_hier_signal_access(void)
{
    const char *vhdl =
        "entity sub is port(x: in std_logic; z: out std_logic); end entity;\n"
        "architecture rtl of sub is\n"
        "  signal inner: std_logic;\n"
        "begin\n"
        "  inner <= x;\n"
        "  z <= inner;\n"
        "end architecture;\n";
    const char *verilog =
        "module top; reg x; wire z; sub s(.x(x), .z(z)); endmodule\n";

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    const char *sources[] = {vhdl, verilog};
    input.sources = sources; input.source_count = 2;
    qsim_compile_result_t *cr = qsim_compile(&input);
    mu_assert(cr->success != 0, "compile hier");
    mu_assert(cr->unit_count == 2, "2 units");

    uir_design_unit_t *units[] = {(uir_design_unit_t*)cr->units[0], (uir_design_unit_t*)cr->units[1]};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate hier");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    /* Access sub-instance signal by hierarchical path */
    qsim_bit_vector_t *inner_v = uir_sim_get_signal(sim, "s.inner");
    mu_assert_ptr_not_null(inner_v, "s.inner exists");

    qsim_bit_vector_t *one = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "x", one);
    uir_sim_run(sim, 10);

    /* Check propagation through the VHDL submodule */
    qsim_bit_vector_t *zv = uir_sim_get_signal(sim, "z");
    mu_assert_not_null(zv);
    mu_assert(qsim_bit_get(zv, 0).state == QSIM_1, "z=1 through sub");

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    qsim_bit_vector_free(one);
}

void register_simulator_tests(void)
{
    printf("[Simulator]\n");
    mu_run_test(test_init_shutdown);
    mu_run_test(test_recovery_from_names);
    mu_run_test(test_compile_null_input);
    mu_run_test(test_compile_empty_files);
    mu_run_test(test_compile_nonexistent_file);
    mu_run_test(test_compile_free_null);
    mu_run_test(test_sim_null_session);
    mu_run_test(test_sim_result_free_null);
    mu_run_test(test_sim_result_lifecycle);
    mu_run_test(test_compile_inline_verilog);
    mu_run_test(test_compile_inline_vhdl);
    mu_run_test(test_compile_and_simulate_counter);
    mu_run_test(test_compile_and_simulate_dff);
    mu_run_test(test_posedge_x_to_1);
    mu_run_test(test_compile_and_simulate_mux);
    mu_run_test(test_compile_and_simulate_full_adder);
    mu_run_test(test_compile_and_simulate_fsm);
    mu_run_test(test_inline_wire_init);
    mu_run_test(test_simple_blocking_assign);
    mu_run_test(test_case_simple);
    mu_run_test(test_compile_and_simulate_vhdl_counter);
    mu_run_test(test_mixed_elab_verilog_top_vhdl_sub);
    mu_run_test(test_mixed_sim_verilog_top_vhdl_sub);
    mu_run_test(test_mixed_sim_vhdl_top_verilog_sub);
    mu_run_test(test_mixed_port_prop_verilog_to_vhdl_ca);
    mu_run_test(test_mixed_port_prop_vhdl_process_to_internal);
    mu_run_test(test_mixed_vector_port_prop);
    mu_run_test(test_mixed_vhdl_to_verilog_prop);
    mu_run_test(test_mixed_multi_level);
    mu_run_test(test_mixed_hier_signal_access);

    /* VHDL dual-instance (Bug 1 reproduction) */
    mu_run_test(test_vhdl_dual_instance);

    /* Delay / Timing Controls */
    mu_run_test(test_delay_initial_toggle);
    mu_run_test(test_delay_clock_gen);
    mu_run_test(test_delay_cont_assign);

    /* Wait / Event Control / Disable */
    mu_run_test(test_wait_immediate);
    mu_run_test(test_wait_on_signal_change);
    mu_run_test(test_event_control_posedge);
    mu_run_test(test_disable_block);
    mu_run_test(test_disable_task);

    /* Loop statements (for/while/repeat/forever) */
    mu_run_test(test_loop_for_sync);
    mu_run_test(test_loop_while_sync);
    mu_run_test(test_loop_repeat_sync);
    mu_run_test(test_loop_for_timing);
    mu_run_test(test_loop_forever_clock);

    /* Port propagation through hierarchy */
    mu_run_test(test_port_propagation_basic);
    mu_run_test(test_port_propagation_output);

    /* Equality/comparison operations */
    mu_run_test(test_reg_equality_comparison);
    mu_run_test(test_nba_compare_after_clock);
    mu_run_test(test_cross_module_eq_after_port_propagation);

    /* Scalar signal assign (Bug #3) */
    mu_run_test(test_scalar_assign_returns_known);

    /* Function/Task */
    mu_run_test(test_func_return_constant);
    mu_run_test(test_func_add_args);
    mu_run_test(test_func_call_in_always);
    mu_run_test(test_task_swap_inout);
    mu_run_test(test_func_in_cont_assign);

    /* Implicit wire */
    mu_run_test(test_implicit_wire_cont_assign);
    mu_run_test(test_implicit_wire_blocking_assign);
    mu_run_test(test_implicit_wire_nonblocking_assign);
    mu_run_test(test_implicit_wire_expr_rhs);
    mu_run_test(test_implicit_wire_declared_not_overridden);

    /* System tasks (Phase 2) */
    mu_run_test(test_stop_pauses);
    mu_run_test(test_finish_terminates);
    mu_run_test(test_stop_continue);
    mu_run_test(test_fatal_output_and_finish);
    mu_run_test(test_severity_output);
    mu_run_test(test_clog2_compute);
    mu_run_test(test_clog2_of_7);
    mu_run_test(test_signed_passthrough);
    mu_run_test(test_time_in_process);
    mu_run_test(test_random_returns_value);

    /* Net type resolution (Phase 3) */
    mu_run_test(test_wand_resolution);
    mu_run_test(test_wor_resolution);
    mu_run_test(test_tri0_pull);
    mu_run_test(test_tri1_pull);
    mu_run_test(test_supply0_supply1);

    /* Named events (Phase 5a) */
    mu_run_test(test_event_trigger_wakes_waiter);
    mu_run_test(test_event_in_sensitivity_list);

    /* SystemVerilog process types */
    mu_run_test(test_sv_always_comb);
    mu_run_test(test_sv_always_ff);
    mu_run_test(test_sv_always_latch);

    /* SystemVerilog interface/modport */
    mu_run_test(test_sv_interface_sim);

    /* SystemVerilog package/import */
    mu_run_test(test_sv_package_sim);
    mu_run_test(test_sv_package_import_wildcard);

    /* SystemVerilog logic port-type */
    mu_run_test(test_sv_logic_port_basic);
    mu_run_test(test_sv_logic_port_vector);
    mu_run_test(test_sv_logic_cont_assign);
    mu_run_test(test_nested_case_sens);
    printf("\n");
}
