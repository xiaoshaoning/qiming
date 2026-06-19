/* Reproduction tests for NPU-reported simulation bugs.
 * Compile: gcc -o test_npu_bugs test_npu_bugs.c -I../include -L.. -lqsim
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "libqsim/session.h"

static int check_val(const char *label, qsim_bit_vector_t v,
                     int bit, qsim_logic_state_t expected) {
    if (!v.bits) { printf("  %s: NULL bits\n", label); return 0; }
    qsim_logic_state_t got = qsim_bit_get(&v, (uint32_t)bit).state;
    if (got != expected) {
        printf("  %s: FAIL: bit %d = %d, expected %d\n", label, bit, got, expected);
        return 0;
    }
    return 1;
}

static void print_val(const char *label, qsim_bit_vector_t v) {
    if (!v.bits) { printf("  %s: NULL\n", label); return; }
    printf("  %s: ", label);
    for (int i = (int)v.width - 1; i >= 0; i--) {
        qsim_logic_state_t s = qsim_bit_get(&v, (uint32_t)i).state;
        printf("%c", s == QSIM_1 ? '1' : s == QSIM_0 ? '0' : s == QSIM_X ? 'X' : 'Z');
    }
    printf("\n");
}

/* Bug 1: NBA with arithmetic expression on RHS */
static void test_bug1_nba_arith(void)
{
    printf("Bug 1: NBA cnt <= cnt + 1'b1 ...\n");
    const char *src =
        "module counter(input clk, input rst_n, output reg [1:0] cnt);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) cnt <= 2'b00;\n"
        "    else        cnt <= cnt + 1'b1;\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }

    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done; }

    /* Assert reset */
    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);

    /* Release reset */
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    /* Clock for 3 posedges (cnt: 0->1->2->3) */
    for (int cyc = 0; cyc < 3; cyc++) {
        qsim_session_force_str(s, "clk", "1'b1");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);
    }

    qsim_bit_vector_t v = qsim_session_eval(s, "cnt");
    print_val("cnt after 3 cycles", v);
    int ok = 1;
    if (!check_val("cnt[0]", v, 0, QSIM_1)) ok = 0;
    if (!check_val("cnt[1]", v, 1, QSIM_1)) ok = 0;
    free(v.bits);

    if (ok) printf("  PASS: cnt = 2'b11 (3)\n");
    else    printf("  FAIL\n");
done:
    qsim_session_free(s);
}

/* Bug 2: NBA with bitwise NOT on RHS */
static void test_bug2_nba_not(void)
{
    printf("Bug 2: NBA a <= ~a ...\n");
    const char *src =
        "module toggle(input clk, input rst_n, output reg a);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) a <= 1'b0;\n"
        "    else        a <= ~a;\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }
    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done2; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done2; }

    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    /* Cycle 1: a should toggle from 0 -> 1 */
    qsim_session_force_str(s, "clk", "1'b1");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_bit_vector_t v = qsim_session_eval(s, "a");
    print_val("a after cycle 1", v);
    int ok = 1;
    if (!check_val("a", v, 0, QSIM_1)) ok = 0;
    free(v.bits);

    /* Cycle 2: a should toggle from 1 -> 0 */
    qsim_session_force_str(s, "clk", "1'b1");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    v = qsim_session_eval(s, "a");
    print_val("a after cycle 2", v);
    if (!check_val("a", v, 0, QSIM_0)) ok = 0;
    free(v.bits);

    if (ok) printf("  PASS: toggles 0->1->0\n");
    else    printf("  FAIL\n");
done2:
    qsim_session_free(s);
}

/* Bug 3: NBA with concatenation on RHS */
static void test_bug3_nba_concat(void)
{
    printf("Bug 3: NBA out <= {big[15:0], big[31:16]} ...\n");
    const char *src =
        "module byte_swap(input clk, input rst_n,\n"
        "  input [31:0] big, output reg [31:0] out);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) out <= 32'h00000000;\n"
        "    else        out <= {big[15:0], big[31:16]};\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }
    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done3; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done3; }

    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_force_str(s, "big", "32'hAABBCCDD");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    /* Cycle 1 */
    qsim_session_force_str(s, "clk", "1'b1");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_bit_vector_t v = qsim_session_eval(s, "out");
    print_val("out", v);
    int ok = 1;
    /* After byte-swap: out = {big[15:0]=0xCCDD, big[31:16]=0xAABB} = 0xCCDDAABB
       LSB-first: bit0 = BB's bit0 = 1, bit31 = CC's bit7 = 1 */
    if (!check_val("out[0]", v, 0, QSIM_1)) ok = 0;
    if (!check_val("out[31]", v, 31, QSIM_1)) ok = 0;
    free(v.bits);

    if (ok) printf("  PASS: out = 0xCCDDAABB\n");
    else    printf("  FAIL\n");
done3:
    qsim_session_free(s);
}

/* Bug 4: NBA with part-select on LHS (low bits) */
static void test_bug4_nba_part_select_lhs(void)
{
    printf("Bug 4: NBA out[15:0] <= 42, out[31:16] <= 99 ...\n");
    const char *src =
        "module ps_lhs(input clk, input rst_n, output reg [31:0] out);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) out <= 32'h00000000;\n"
        "    else begin\n"
        "      out[15:0]  <= 16'd42;\n"
        "      out[31:16] <= 16'd99;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }
    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done4; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done4; }

    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    qsim_session_force_str(s, "clk", "1'b1");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_bit_vector_t v = qsim_session_eval(s, "out");
    print_val("out", v);
    int ok = 1;
    if (!check_val("out[0]", v, 0, QSIM_0)) ok = 0;   /* 42 bit0 = 0 */
    if (!check_val("out[1]", v, 1, QSIM_1)) ok = 0;   /* 42 bit1 = 1 */
    if (!check_val("out[3]", v, 3, QSIM_1)) ok = 0;   /* 42 bit3 = 1 */
    if (!check_val("out[16]", v, 16, QSIM_1)) ok = 0;  /* 99 bit0 = 1 */
    free(v.bits);

    if (ok) printf("  PASS: out[15:0]=42, out[31:16]=99\n");
    else    printf("  FAIL\n");
done4:
    qsim_session_free(s);
}

/* Bug 5: Continuous assign with part-select on RHS */
static void test_bug5_cont_assign_part_select(void)
{
    printf("Bug 5: assign out = big[15:8] ...\n");
    const char *src =
        "module ca_ps(input [31:0] big, output [7:0] out);\n"
        "  assign out = big[15:8];\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }
    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done5; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done5; }

    qsim_session_force_str(s, "big", "32'hAABBCCDD");
    qsim_session_step_delta(s);
    qsim_bit_vector_t v = qsim_session_eval(s, "out");
    print_val("out", v);
    int ok = 1;
    /* big[15:8] of 0xAABBCCDD. LSB-first: bytes = DD,CC,BB,AA. bits[15:8] = CC = 0b1100_1100 */
    if (!check_val("out[2]", v, 2, QSIM_1)) ok = 0;
    if (!check_val("out[7]", v, 7, QSIM_1)) ok = 0;
    free(v.bits);

    /* Change big and verify re-evaluation */
    qsim_session_force_str(s, "big", "32'h00000000");
    qsim_session_step_delta(s);
    v = qsim_session_eval(s, "out");
    print_val("out after big=0", v);
    if (!check_val("out[0]", v, 0, QSIM_0)) ok = 0;
    if (!check_val("out[7]", v, 7, QSIM_0)) ok = 0;
    free(v.bits);

    if (ok) printf("  PASS\n");
    else    printf("  FAIL\n");
done5:
    qsim_session_free(s);
}

/* Bug 6: Signal read in non-boolean expressions */
static void test_bug6_non_boolean_read(void)
{
    printf("Bug 6: NBA with arithmetic read of same reg ...\n");
    const char *src =
        "module same_reg(input clk, input rst_n, output reg [1:0] x);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) x <= 2'b00;\n"
        "    else        x <= x + 2'b01;\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }
    if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); goto done6; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done6; }

    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    /* 3 cycles: x should be 0->1->2->3 */
    for (int i = 0; i < 3; i++) {
        qsim_session_force_str(s, "clk", "1'b1");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "x");
        print_val("x", v);
        free(v.bits);
    }

    qsim_bit_vector_t v = qsim_session_eval(s, "x");
    int ok = 1;
    if (!check_val("x[0]", v, 0, QSIM_1)) ok = 0; /* x=3 -> bit0=1 */
    if (!check_val("x[1]", v, 1, QSIM_1)) ok = 0; /* x=3 -> bit1=1 */
    free(v.bits);
    if (ok) printf("  PASS: x = 2'b11 (3)\n");
    else    printf("  FAIL\n");
done6:
    qsim_session_free(s);
}

/* Bug 7: NBA width truncation on narrower RHS */
static void test_bug7_nba_width(void)
{
    printf("Bug 7: NBA narrower RHS (active_bank <= 1'b1) ...\n");
    const char *src =
        "module test7(input clk, input rst_n, output reg [1:0] active_bank);\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) active_bank <= 0;\n"
        "    else        active_bank <= 1'b1;\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }

    if (!qsim_session_compile_string(s, "test7.v", src)) { printf("  FAIL: compile\n"); goto done7; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done7; }

    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);

    qsim_session_force_str(s, "clk", "1'b1");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);

    qsim_bit_vector_t v = qsim_session_eval(s, "active_bank");
    print_val("active_bank", v);
    int ok = 1;
    if (!v.bits || v.width != 2) { printf("  FAIL: wrong width\n"); ok = 0; goto skip7; }
    if (qsim_bit_get(&v, 0).state != QSIM_1) { printf("  FAIL: bit 0 should be 1\n"); ok = 0; }
    if (qsim_bit_get(&v, 1).state != QSIM_0) { printf("  FAIL: bit 1 should be 0 (not X)\n"); ok = 0; }
    free(v.bits);

skip7:
    if (ok) printf("  PASS: active_bank = 2'b01\n");
    else    printf("  FAIL\n");
done7:
    qsim_session_free(s);
}

/* Bug 8: Bit-select/part-select in boolean conditions and equality comparisons */
static void test_bug8_bit_select_condition(void)
{
    printf("Bug 8: Bit-select in condition (!addr[8]) and part-select in equality (addr[7:4] == N) ...\n");

    /* Test both combinatorial (continuous assign) and sequential (NBA) paths */
    const char *src =
        "module bug8(input [8:0] addr, output reg bank_sel, output reg word_match,\n"
        "            output wire comb_bank_sel, output wire comb_word_match);\n"
        "  assign comb_bank_sel = !addr[8];  /* bit-select in NOT */\n"
        "  assign comb_word_match = (addr[7:4] == 4'b0000);  /* part-select == literal */\n"
        "  always @(*) begin\n"
        "    bank_sel = !addr[8];\n"
        "    word_match = (addr[7:4] == 4'b0110);\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); return; }

    if (!qsim_session_compile_string(s, "bug8.v", src)) { printf("  FAIL: compile\n"); goto done8; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done8; }

    int ok = 1;

    /* Test 1: addr=0 -> !addr[8]=1, addr[7:4]==0 -> both true */
    printf("  Test 1: addr=0\n");
    qsim_session_force_str(s, "addr", "9'b000000000");
    qsim_session_step_delta(s);
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "addr");
        print_val("  addr full value", v);
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "comb_bank_sel");
        print_val("  comb_bank_sel", v);
        if (!check_val("comb_bank_sel", v, 0, QSIM_1)) ok = 0;
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "comb_word_match");
        print_val("  comb_word_match", v);
        if (!check_val("comb_word_match", v, 0, QSIM_1)) ok = 0;
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "bank_sel");
        print_val("  bank_sel (blocking)", v);
        if (!check_val("bank_sel", v, 0, QSIM_1)) ok = 0;
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "word_match");
        print_val("  word_match (blocking)", v);
        if (!check_val("word_match", v, 0, QSIM_0)) ok = 0; /* addr[7:4]=0 != 6 */
        free(v.bits);
    }

    /* Test 2: addr=256 (bit 8 set) -> !addr[8]=0, addr[7:4]==0 */
    printf("  Test 2: addr=256 (bit 8=1)\n");
    qsim_session_force_str(s, "addr", "9'b100000000");
    qsim_session_step_delta(s);
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "addr");
        print_val("  addr full value", v);
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "comb_bank_sel");
        print_val("  comb_bank_sel", v);
        if (!check_val("comb_bank_sel", v, 0, QSIM_0)) ok = 0;
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "comb_word_match");
        print_val("  comb_word_match", v);
        if (!check_val("comb_word_match", v, 0, QSIM_1)) ok = 0;
        free(v.bits);
    }

    /* Test 3: addr=96 -> addr[7:4] = 6 */
    printf("  Test 3: addr=96 (0x60)\n");
    qsim_session_force_str(s, "addr", "9'b001100000");
    qsim_session_step_delta(s);
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "addr");
        print_val("  addr full value", v);
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "comb_word_match");
        print_val("  comb_word_match", v);
        if (!check_val("comb_word_match", v, 0, QSIM_0)) ok = 0; /* addr[7:4]=6 != 0 */
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "word_match");
        print_val("  word_match (blocking)", v);
        if (!check_val("word_match", v, 0, QSIM_1)) ok = 0; /* addr[7:4]=6 == 6 */
        free(v.bits);
    }
    {
        qsim_bit_vector_t v = qsim_session_eval(s, "bank_sel");
        print_val("  bank_sel (blocking)", v);
        if (!check_val("bank_sel", v, 0, QSIM_1)) ok = 0; /* addr[8]=0 */
        free(v.bits);
    }

    if (ok) printf("  PASS: all 4 bit-select/part-select tests passed\n");
    else    printf("  FAIL\n");
done8:
    qsim_session_free(s);
}

/* Bug 9: Verilog case statement mis-selection */
static void test_bug9_case_statement(void)
{
    printf("Bug 9: case statement multi-bit selection ...\n");

    /* Test both combinatorial and sequential paths */
    const char *comb_src =
        "module bug9_comb(input [2:0] sel, output reg [31:0] out);\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0: out = 100;\n"
        "      1: out = 101;\n"
        "      2: out = 102;\n"
        "      3: out = 103;\n"
        "      4: out = 104;\n"
        "      5: out = 105;\n"
        "      6: out = 106;\n"
        "      7: out = 107;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";

    const char *seq_src =
        "module bug9_seq(input clk, input rst_n, output reg [31:0] out);\n"
        "  reg [2:0] rd_ptr;\n"
        "  always @(posedge clk or negedge rst_n) begin\n"
        "    if (!rst_n) begin rd_ptr <= 0; out <= 0; end\n"
        "    else begin\n"
        "      case (rd_ptr)\n"
        "        0: out <= 100;\n"
        "        1: out <= 101;\n"
        "        2: out <= 102;\n"
        "        3: out <= 103;\n"
        "        4: out <= 104;\n"
        "        5: out <= 105;\n"
        "        6: out <= 106;\n"
        "        7: out <= 107;\n"
        "      endcase\n"
        "      rd_ptr <= rd_ptr + 1;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    const char *wide_src =
        "module bug9_wide(input [7:0] sel, output reg [31:0] out);\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      10: out = 210;\n"
        "      100: out = 300;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";

    int ok = 1;
    qsim_session_t *s = NULL;

    /* Test 1: combinatorial case */
    printf("  Test 1: combinatorial case (sel 0-7)\n");
    s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); ok = 0; goto skip1; }
    if (!qsim_session_compile_string(s, "comb.v", comb_src)) { printf("  FAIL: compile\n"); ok = 0; goto skip1; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); ok = 0; goto skip1; }
    for (int val = 0; val < 8; val++) {
        char vbits[4];
        vbits[0] = ((val >> 2) & 1) ? '1' : '0';
        vbits[1] = ((val >> 1) & 1) ? '1' : '0';
        vbits[2] = ((val >> 0) & 1) ? '1' : '0';
        vbits[3] = '\0';
        char force[32];
        snprintf(force, sizeof(force), "3'b%s", vbits);
        qsim_session_force_str(s, "sel", force);
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        int expected = 100 + val;
        int got = 0;
        for (uint32_t i = 0; i < 32 && i < v.width; i++) {
            if (qsim_bit_get(&v, i).state == QSIM_1) got |= (1u << i);
        }
        if (got != expected) {
            printf("  FAIL: sel=%d expected=%d got=%d\n", val, expected, got);
            ok = 0;
        }
        free(v.bits);
    }
skip1:
    if (s) { qsim_session_free(s); s = NULL; }
    if (ok) printf("    comb 0-7: PASS\n");

    /* Test 2: sequential case (rd_ptr cycles 0-7) */
    printf("  Test 2: sequential case (rd_ptr 0-7)\n");
    s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); ok = 0; goto skip2; }
    if (!qsim_session_compile_string(s, "seq.v", seq_src)) { printf("  FAIL: compile\n"); ok = 0; goto skip2; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); ok = 0; goto skip2; }
    qsim_session_force_str(s, "rst_n", "1'b0");
    qsim_session_force_str(s, "clk", "1'b0");
    qsim_session_step_delta(s);
    qsim_session_force_str(s, "rst_n", "1'b1");
    qsim_session_step_delta(s);
    for (int cyc = 0; cyc < 8; cyc++) {
        qsim_session_force_str(s, "clk", "1'b1");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        int expected = 100 + cyc;
        int got = 0;
        for (uint32_t i = 0; i < 32 && i < v.width; i++) {
            if (qsim_bit_get(&v, i).state == QSIM_1) got |= (1u << i);
        }
        if (got != expected) {
            printf("  FAIL: cycle=%d expected=%d got=%d\n", cyc, expected, got);
            ok = 0;
        }
        free(v.bits);
    }
skip2:
    if (s) { qsim_session_free(s); s = NULL; }
    if (ok) printf("    seq 0-7: PASS\n");

    /* Test 3: multi-digit pattern values via set_str */
    printf("  Test 3: 2-digit pattern (sel=10 → out=210)\n");
    s = qsim_session_create();
    if (!s) { printf("  FAIL: create session\n"); ok = 0; goto skip3; }
    if (!qsim_session_compile_string(s, "wide.v", wide_src)) { printf("  FAIL: compile\n"); ok = 0; goto skip3; }
    if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); ok = 0; goto skip3; }
    {
        qsim_session_set_str(s, "sel", "8'h0A");
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        int got = 0;
        for (uint32_t i = 0; i < 32 && i < v.width; i++) {
            if (qsim_bit_get(&v, i).state == QSIM_1) got |= (1u << i);
        }
        if (got != 210) {
            printf("  FAIL: sel=10 expected=210 got=%d\n", got);
            ok = 0;
        }
        free(v.bits);
    }
skip3:
    if (s) { qsim_session_free(s); s = NULL; }

    if (ok) printf("  PASS: all case statement tests passed\n");
    else    printf("  FAIL\n");
}

/* Bug 10: Replication operator {N{expr}} followed by additional concat items */
static void test_bug10_repl_concat(void)
{
    printf("Bug 10: replication + concat {8{a[7]}, a[7:0]} ...\n");

    /* Test A: continuous assign with repl+concat */
    printf("  Test A: continuous assign {8{a[7]}, a[7:0]}\n");
    {
        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); return; }

        if (!qsim_session_compile_string(s, "t.v",
            "module t(input [7:0] a, output [15:0] out);\n"
            "  assign out = {8{a[7]}, a[7:0]};\n"
            "endmodule\n")) { printf("  FAIL: compile\n"); goto done10; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done10; }

        qsim_session_force_str(s, "a", "11111111");
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        print_val("  out", v);
        int ok = 1;
        if (!check_val("out[0]", v, 0, QSIM_1)) ok = 0;  /* a[0]=1 */
        if (!check_val("out[7]", v, 7, QSIM_1)) ok = 0;  /* a[7]=1 */
        if (!check_val("out[15]", v, 15, QSIM_1)) ok = 0; /* replicated bit = 1 */
        free(v.bits);
        if (ok) printf("    PASS: out = 0xFFFF\n");
        else    printf("    FAIL\n");
    done10:
        qsim_session_free(s);
    }

    /* Test B: repl+concat with constant */
    printf("  Test B: assign {8{1'b1}, 8'h00}\n");
    {
        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); return; }

        if (!qsim_session_compile_string(s, "t.v",
            "module t(output [15:0] out);\n"
            "  assign out = {8{1'b1}, 8'h00};\n"
            "endmodule\n")) { printf("  FAIL: compile\n"); goto done10b; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done10b; }
        qsim_session_step_delta(s);
        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        print_val("  out", v);
        int ok = 1;
        /* Expected: MSB 8 bits = 0xFF, LSB 8 bits = 0x00 = 0xFF00 */
        if (!check_val("out[0]", v, 0, QSIM_0)) ok = 0;
        if (!check_val("out[7]", v, 7, QSIM_0)) ok = 0;
        if (!check_val("out[8]", v, 8, QSIM_1)) ok = 0;
        if (!check_val("out[15]", v, 15, QSIM_1)) ok = 0;
        free(v.bits);
        if (ok) printf("    PASS: out = 0xFF00\n");
        else    printf("    FAIL\n");
    done10b:
        qsim_session_free(s);
    }

    /* Test C: NBA with repl+concat */
    printf("  Test C: NBA {8{tmp[7]}, tmp[7:0]}\n");
    {
        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); return; }

        if (!qsim_session_compile_string(s, "t.v",
            "module t(input clk, input rst_n, output reg [15:0] out);\n"
            "  reg [7:0] tmp;\n"
            "  always @(posedge clk or negedge rst_n) begin\n"
            "    if (!rst_n) begin tmp <= 0; out <= 0; end\n"
            "    else begin tmp <= 8'hFF; out <= {8{tmp[7]}, tmp[7:0]}; end\n"
            "  end\n"
            "endmodule\n")) { printf("  FAIL: compile\n"); goto done10c; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); goto done10c; }

        qsim_session_force_str(s, "rst_n", "1'b0");
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "rst_n", "1'b1");
        qsim_session_step_delta(s);
        /* Posedge 1: tmp was 0, out was 0, so tmp becomes 0xFF, out stays 0 */
        qsim_session_force_str(s, "clk", "1'b1");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);
        /* Posedge 2: now tmp=0xFF, out <= {8{1}, 0xFF} = 0xFFFF */
        qsim_session_force_str(s, "clk", "1'b1");
        qsim_session_step_delta(s);
        qsim_session_force_str(s, "clk", "1'b0");
        qsim_session_step_delta(s);

        qsim_bit_vector_t v = qsim_session_eval(s, "out");
        print_val("  out", v);
        int ok = 1;
        if (!check_val("out[0]", v, 0, QSIM_1)) ok = 0;
        if (!check_val("out[15]", v, 15, QSIM_1)) ok = 0;
        free(v.bits);
        if (ok) printf("    PASS: out = 0xFFFF\n");
        else    printf("    FAIL\n");
    done10c:
        qsim_session_free(s);
    }
}

/* Bug 11: NBA-set register reads as zero in arithmetic expression.
 * Reproduction of the PE test pattern: weight_reg <= weight_in via NBA,
 * then acc_out <= acc_in + (act_in * weight_reg) reads weight_reg as 0. */
static void set_p(qsim_session_t *s, const char *sig, const char *val) {
    qsim_session_set_str(s, sig, val);
}
static void force_p(qsim_session_t *s, const char *sig, const char *val) {
    qsim_session_force_str(s, sig, val);
}
static void deltas(qsim_session_t *s) {
    for (int i = 0; i < 10; i++) if (!qsim_session_step_delta(s)) break;
}
static void cycle_set_p(qsim_session_t *s) {
    set_p(s, "clk", "1"); deltas(s);
    set_p(s, "clk", "0"); deltas(s);
}

/* Produce MSB-first binary string from uint32 value (qsim string convention) */
static const char* bitstr(uint32_t val, int width) {
    static char buf[128];
    for (int i = 0; i < width; i++)
        buf[width - 1 - i] = (val & (1U << i)) ? '1' : '0';
    buf[width] = '\0';
    return buf;
}

/* Read signal as unsigned int (MSB-first bit vector) */
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

static void test_bug11_nba_reg_zero_in_arith(void)
{
    printf("Bug 11: NBA weight_reg read as zero in arithmetic ...\n");
    int all_ok = 1;

    /* Test A: force_p data_in, read weight_reg in arithmetic expression */
    {
        const char *src =
            "module bug11a(input clk, input rst_n, input [7:0] data_in,"
            "  output reg [15:0] result);\n"
            "  reg [7:0] weight_reg;\n"
            "  always @(posedge clk or negedge rst_n) begin\n"
            "    if (!rst_n) begin\n"
            "      weight_reg <= 0;\n"
            "      result <= 0;\n"
            "    end else begin\n"
            "      weight_reg <= data_in;\n"
            "      result <= result + (8'd20 * weight_reg);\n"
            "    end\n"
            "  end\n"
            "endmodule\n";

        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); all_ok = 0; goto done11a; }
        if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); all_ok = 0; goto done11a; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); all_ok = 0; goto done11a; }

        /* Test multiple values including edge cases */
        int vals[] = {1, 2, 3, 7, 10, 127, 255};
        for (int vi = 0; vi < 7; vi++) {
            int test_val = vals[vi];
            force_p(s, "rst_n", "0"); deltas(s);
            force_p(s, "clk", "0"); deltas(s);
            force_p(s, "rst_n", "1"); deltas(s);

            /* Need the force_p to be a reg assignment to propagate correctly.
             * Use cycle_set_p so that data_in is stable at clock edge. */
            force_p(s, "data_in", bitstr(test_val, 8));
            deltas(s);

            /* Cycle 1: weight_reg <= data_in (NBA scheduled), result stays 0 */
            cycle_set_p(s);
            /* Cycle 2: result = 0 + 20 * weight_reg, where weight_reg = test_val */
            cycle_set_p(s);

            uint32_t got = read_uint(s, "result");
            uint32_t exp = (uint32_t)(20 * test_val);
            if (got != exp) {
                printf("  FAIL[%d]: result=%u expected=%u\n", test_val, got, exp);
                all_ok = 0;
            }
        }
        if (all_ok) printf("  Test A (force_p, multi-value): PASS\n");

    done11a:
        qsim_session_free(s);
    }

    /* Test B: set_p data_in (scheduled event), multi-always blocks */
    {
        const char *src =
            "module bug11b(input clk, input rst_n, input [7:0] data_in,"
            "  output reg [15:0] result);\n"
            "  reg [7:0] weight_reg;\n"
            "  always @(posedge clk or negedge rst_n) begin\n"
            "    if (!rst_n) weight_reg <= 0;\n"
            "    else weight_reg <= data_in;\n"
            "  end\n"
            "  always @(posedge clk or negedge rst_n) begin\n"
            "    if (!rst_n) result <= 0;\n"
            "    else result <= result + (8'd20 * weight_reg);\n"
            "  end\n"
            "endmodule\n";

        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); all_ok = 0; goto done11b; }
        if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); all_ok = 0; goto done11b; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); all_ok = 0; goto done11b; }

        force_p(s, "rst_n", "0"); deltas(s);
        force_p(s, "clk", "0"); deltas(s);
        force_p(s, "rst_n", "1"); deltas(s);

        /* Test multi-always with set_p on data_in */
        set_p(s, "data_in", "00000011"); deltas(s);

        /* Cycle 1: weight_reg <= data_in=3, result stays 0 */
        cycle_set_p(s);
        /* Cycle 2: result = 0 + 20*weight_reg=60 */
        cycle_set_p(s);

        uint32_t got = read_uint(s, "result");
        if (got != 60) {
            printf("  FAIL[multi-always]: result=%u expected=60\n", got);
            all_ok = 0;
        } else {
            printf("  Test B (multi-always, set_p): PASS\n");
        }

    done11b:
        qsim_session_free(s);
    }

    /* Test C: force_p on internal reg (weight_reg directly), read in arithmetic */
    {
        const char *src =
            "module bug11c(input clk, input rst_n, output reg [15:0] out);\n"
            "  reg [7:0] weight_reg;\n"
            "  always @(posedge clk or negedge rst_n) begin\n"
            "    if (!rst_n) out <= 0;\n"
            "    else out <= out + (8'd20 * weight_reg);\n"
            "  end\n"
            "endmodule\n";

        qsim_session_t *s = qsim_session_create();
        if (!s) { printf("  FAIL: create session\n"); all_ok = 0; goto done11c; }
        if (!qsim_session_compile_string(s, "test.v", src)) { printf("  FAIL: compile\n"); all_ok = 0; goto done11c; }
        if (!qsim_session_elaborate(s)) { printf("  FAIL: elaborate\n"); all_ok = 0; goto done11c; }

        int vals_c[] = {1, 2, 3, 5, 10, 127, 255};
        for (int vi = 0; vi < 7; vi++) {
            int test_val = vals_c[vi];
            force_p(s, "rst_n", "0"); deltas(s);
            force_p(s, "clk", "0"); deltas(s);
            force_p(s, "rst_n", "1"); deltas(s);

            /* Force internal weight_reg directly */
            force_p(s, "weight_reg", bitstr(test_val, 8));
            deltas(s);

            cycle_set_p(s);
            uint32_t got = read_uint(s, "out");
            uint32_t exp = (uint32_t)(20 * test_val);
            if (got != exp) {
                printf("  FAIL[C%d]: out=%u expected=%u\n", test_val, got, exp);
                all_ok = 0;
            }
        }
        if (all_ok) printf("  Test C (force_p internal reg): PASS\n");

    done11c:
        qsim_session_free(s);
    }

    if (all_ok) printf("  PASS: All Bug 11 tests passed\n");
    else        printf("  FAIL\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_bug1_nba_arith();
    test_bug2_nba_not();
    test_bug3_nba_concat();
    test_bug4_nba_part_select_lhs();
    test_bug5_cont_assign_part_select();
    test_bug6_non_boolean_read();
    test_bug7_nba_width();
    test_bug8_bit_select_condition();
    test_bug9_case_statement();
    test_bug10_repl_concat();
    test_bug11_nba_reg_zero_in_arith();

    printf("\nDone.\n");
    return 0;
}
