#include "libqsim/uir_sim.h"
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void force_p(qsim_session_t *s, const char *sig, const char *val) {
    qsim_session_force_str(s, sig, val);
}

static void set_p(qsim_session_t *s, const char *sig, const char *val) {
    qsim_session_set_str(s, sig, val);
}

static void deltas(qsim_session_t *s) {
    for (int i = 0; i < 10; i++) if (!qsim_session_step_delta(s)) break;
}

static void cycle(qsim_session_t *s) {
    set_p(s, "clk", "1"); deltas(s);
    set_p(s, "clk", "0"); deltas(s);
}

static uint32_t read_uint(qsim_session_t *s, const char *sig) {
    qsim_bit_vector_t v = qsim_session_eval(s, sig);
    if (!v.bits) return 0xFFFFFFFF;
    uint32_t result = 0;
    for (uint32_t i = 0; i < v.width && i < 32; i++) {
        if (qsim_bit_get(&v, i).state == QSIM_1)
            result |= (1U << i);
    }
    free(v.bits);
    return result;
}

static void print_sig(qsim_session_t *s, const char *sig) {
    char *v = qsim_session_eval_str(s, sig);
    printf("  %s = %s", sig, v ? v : "NULL");
    if (v) {
        uint32_t uv = 0;
        size_t len = strlen(v);
        for (size_t i = 0; i < len && i < 32; i++)
            if (v[i] == '1') uv |= (1U << i);
        printf(" (%u)", uv);
        qsim_session_free_str(v);
    }
    printf("\n");
}

/* ── Flat PE test (verifies expression evaluator) ── */
static int test_flat(void) {
    printf("\n=== Flat PE test ===\n");

    const char *bug12_src =
        "module bug12(input clk, input rst_n,\n"
        "  input weight_load_en,\n"
        "  input [15:0] weight_in,\n"
        "  input [15:0] act_in,\n"
        "  input [31:0] acc_in,\n"
        "  output reg [15:0] act_out,\n"
        "  output reg [31:0] acc_out);\n"
        "  reg [15:0] weight_reg;\n"
        "  always @(posedge clk) begin\n"
        "    if (!rst_n) begin\n"
        "      weight_reg <= 0; acc_out <= 0; act_out <= 0;\n"
        "    end else begin\n"
        "      if (weight_load_en) weight_reg <= weight_in;\n"
        "      acc_out <= acc_in + (act_in * weight_reg);\n"
        "      act_out <= act_in;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("FAIL: create\n"); return 1; }
    if (!qsim_session_compile_string(s, "bug12.v", bug12_src)) {
        printf("FAIL: compile: %s\n", qsim_session_get_log(s));
        return 1;
    }
    if (!qsim_session_elaborate(s)) {
        printf("FAIL: elaborate: %s\n", qsim_session_get_log(s));
        return 1;
    }

    /* Reset */
    force_p(s, "rst_n", "0"); deltas(s);
    cycle(s);
    force_p(s, "rst_n", "1"); deltas(s);
    cycle(s);

    /* Load weight = 5 */
    force_p(s, "weight_in", "0000000000000101");
    force_p(s, "weight_load_en", "1");
    cycle(s);

    force_p(s, "weight_load_en", "0");
    cycle(s);

    /* Compute: act=3, acc_in=0 */
    force_p(s, "act_in", "0000000000000011");
    force_p(s, "acc_in", "00000000000000000000000000000000");
    cycle(s);

    uint32_t got = read_uint(s, "acc_out");
    printf("acc_out = %u (expected 15)\n", got);
    qsim_session_free(s);

    if (got == 15) {
        printf("PASS: Flat PE test\n");
        return 0;
    }
    printf("FAIL: Flat PE test\n");
    return 1;
}

/* ── Hierarchical test (verifies literal port connections) ── */
static int test_hier(void) {
    printf("\n=== Hierarchical test (literal port connections) ===\n");

    const char *pe_src =
        "module pe(input clk, input rst_n,\n"
        "  input weight_load_en, input [15:0] weight_in,\n"
        "  input [15:0] act_in, input [31:0] acc_in,\n"
        "  output reg [15:0] act_out, output reg [31:0] acc_out);\n"
        "  reg [15:0] weight_reg;\n"
        "  always @(posedge clk) begin\n"
        "    if (!rst_n) begin\n"
        "      weight_reg <= 0; acc_out <= 0; act_out <= 0;\n"
        "    end else begin\n"
        "      if (weight_load_en) weight_reg <= weight_in;\n"
        "      acc_out <= acc_in + (act_in * weight_reg);\n"
        "      act_out <= act_in;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    const char *arr_src =
        "module arr(input clk, input rst_n, input weight_load_en,\n"
        "  input [15:0] weight_0, input [15:0] weight_1,\n"
        "  input [15:0] act_0, input [31:0] acc_0, input [31:0] acc_1,\n"
        "  output [15:0] act_0_out, output [15:0] act_1_out,\n"
        "  output [31:0] acc_0_out, output [31:0] acc_1_out);\n"
        "  pe pe0(.clk(clk), .rst_n(rst_n), .weight_load_en(weight_load_en),\n"
        "    .weight_in(weight_0), .act_in(act_0), .acc_in(acc_0),\n"
        "    .act_out(act_0_out), .acc_out(acc_0_out));\n"
        "  pe pe1(.clk(clk), .rst_n(rst_n), .weight_load_en(weight_load_en),\n"
        "    .weight_in(weight_1),\n"
        "    .act_in(act_0_out), .acc_in(acc_1),\n"
        "    .act_out(act_1_out), .acc_out(acc_1_out));\n"
        "endmodule\n";

    const char *mid_src =
        "module mid(input clk, input rst_n, input start,\n"
        "  input [15:0] weight_0, input [15:0] weight_1,\n"
        "  input [15:0] act_in, output [31:0] result_0,\n"
        "  output [31:0] result_1, output reg weight_load_en,\n"
        "  output reg done);\n"
        "  wire [15:0] act_0_out, act_1_out;\n"
        "  wire [31:0] acc_0_out, acc_1_out;\n"
        "  reg [2:0] state;\n"
        "  localparam IDLE = 0, LOAD = 1, COMP = 2, DRAIN = 3, DONE = 4;\n"
        "  arr u_arr(.clk(clk), .rst_n(rst_n),\n"
        "    .weight_load_en(weight_load_en),\n"
        "    .weight_0(weight_0), .weight_1(weight_1),\n"
        "    .act_0(act_in), .acc_0(32'b0), .acc_1(32'b0),\n"
        "    .act_0_out(act_0_out), .act_1_out(act_1_out),\n"
        "    .acc_0_out(result_0), .acc_1_out(result_1));\n"
        "  always @(posedge clk) begin\n"
        "    if (!rst_n) begin state <= 0; weight_load_en <= 0; done <= 0; end\n"
        "    else begin\n"
        "      case (state)\n"
        "        0: if (start) begin state <= 1; weight_load_en <= 1; end\n"
        "        1: begin weight_load_en <= 0; state <= 2; end\n"
        "        2: state <= 3;\n"
        "        3: begin state <= 4; done <= 1; end\n"
        "        4: begin done <= 0; state <= 0; end\n"
        "      endcase\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    const char *top_src =
        "module top(input clk, input rst_n, input start,\n"
        "  input [15:0] weight_0, input [15:0] weight_1,\n"
        "  input [15:0] act_in,\n"
        "  output [31:0] result_0, output [31:0] result_1);\n"
        "  mid u_mid(.clk(clk), .rst_n(rst_n), .start(start),\n"
        "    .weight_0(weight_0), .weight_1(weight_1),\n"
        "    .act_in(act_in), .result_0(result_0), .result_1(result_1));\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    qsim_session_compile_string(s, "pe.v", pe_src);
    qsim_session_compile_string(s, "arr.v", arr_src);
    qsim_session_compile_string(s, "mid.v", mid_src);
    qsim_session_compile_string(s, "top.v", top_src);
    if (!qsim_session_elaborate(s)) {
        printf("FAIL elaborate: %s\n", qsim_session_get_log(s));
        return 1;
    }

    /* Reset */
    force_p(s, "rst_n", "0"); deltas(s);
    cycle(s);
    force_p(s, "rst_n", "1"); deltas(s);
    cycle(s);

    printf("=== After reset ===\n");
    print_sig(s, "u_mid.u_arr.pe0.weight_reg");
    print_sig(s, "u_mid.u_arr.pe1.weight_reg");

    /* Set PE weight_in ports BEFORE start */
    printf("\n=== Setting PE weight_in (set_p) ===\n");
    set_p(s, "u_mid.u_arr.pe0.weight_in", "0000000000000001");
    set_p(s, "u_mid.u_arr.pe1.weight_in", "0000000000000010");

    /* Start computation */
    force_p(s, "act_in", "0000000000000011");
    force_p(s, "weight_0", "0000000000000001");
    force_p(s, "weight_1", "0000000000000010");
    force_p(s, "start", "1");
    cycle(s);
    force_p(s, "start", "0");

    printf("=== After start + 1 cycle ===\n");
    print_sig(s, "u_mid.state");
    print_sig(s, "u_mid.weight_load_en");
    print_sig(s, "u_mid.u_arr.pe0.weight_in");
    print_sig(s, "u_mid.u_arr.pe0.weight_reg");
    print_sig(s, "u_mid.u_arr.pe1.weight_in");
    print_sig(s, "u_mid.u_arr.pe1.weight_reg");

    cycle(s);
    printf("=== After 2 cycles ===\n");
    print_sig(s, "u_mid.state");
    print_sig(s, "u_mid.weight_load_en");
    print_sig(s, "u_mid.u_arr.pe0.weight_reg");
    print_sig(s, "u_mid.u_arr.pe1.weight_reg");

    cycle(s);
    printf("=== After 3 cycles ===\n");
    print_sig(s, "u_mid.state");
    print_sig(s, "result_0");
    print_sig(s, "result_1");
    uint32_t r0 = read_uint(s, "result_0");
    uint32_t r1 = read_uint(s, "result_1");
    printf("result_0 = %u (expected 3 = 3*1), result_1 = %u (expected 6 = 3*2)\n", r0, r1);

    int pass = (r0 == 3 && r1 == 6);
    if (pass)
        printf("PASS: port propagation + PE arithmetic works through hierarchy\n");
    else
        printf("FAIL: port propagation or PE arithmetic broken\n");

    qsim_session_free(s);
    return pass ? 0 : 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int f = test_flat();
    int h = test_hier();

    printf("\n========================\n");
    if (f == 0 && h == 0)
        printf("PASS: Bug 12 is FIXED\n");
    else
        printf("FAIL: Bug 12 still present (flat=%s, hier=%s)\n",
               f == 0 ? "PASS" : "FAIL", h == 0 ? "PASS" : "FAIL");
    return (f == 0 && h == 0) ? 0 : 1;
}
