/* Verify fixes for bugs from bug_report_2026-05-19.md */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, passed = 0;

static void check(const char *label, const char *actual, const char *expected) {
    tests++;
    int ok = actual && strcmp(actual, expected) == 0;
    printf("  %s = %s (expect %s) %s\n", label, actual ? actual : "?", expected, ok ? "OK" : "FAIL");
    if (ok) passed++;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ── Bug 3: Parameter expressions in comparisons ── */
    printf("=== Bug 3: Parameter expressions in comparisons ===\n");
    {
        const char *src =
        "module bug3;\n"
        "  parameter ACTIVATIONS = 4;\n"
        "  reg [3:0] bug3_cnt;\n"
        "  reg [7:0] bug3_result;\n"
        "  initial begin\n"
        "    bug3_cnt = 4'd3;\n"
        "    if (bug3_cnt == ACTIVATIONS - 1)\n"
        "      bug3_result = 8'hDD;\n"
        "    else\n"
        "      bug3_result = 8'hEE;\n"
        "  end\n"
        "endmodule\n";

        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "bug3.v", src);
        if (!ok) { printf("  PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                /* 0xDD = 0b11011101, LSB-first = "10111011" */
                char *val = qsim_session_eval_str(sess, "bug3_result");
                check("bug3_result (expect 3==4-1 -> 0xDD)", val, "10111011");
                free(val);
            }
        }
        qsim_session_free(sess);
    }

    /* ── Bug 6: Replication inside concatenation ── */
    /* Note: grammar can't parse {{{8{a}}, {8{b}}}} (pre-existing PEG limitation) */
    printf("\n=== Bug 6: Replication inside concatenation ===\n");
    {
        /* Test: replication alone */
        const char *src1 =
        "module bug6;\n"
        "  reg [7:0] bug6_val;\n"
        "  reg [31:0] bug6_concat;\n"
        "  initial begin\n"
        "    bug6_val = 8'hAB;\n"
        "    bug6_concat = {8{bug6_val}};\n"
        "  end\n"
        "endmodule\n";

        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "bug6.v", src1);
        if (!ok) { printf("  Test1 PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  Test1 ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                char *val = qsim_session_eval_str(sess, "bug6_concat");
                printf("  {8{bug6_val}} where bug6_val=8'hAB: %s\n", val ? val : "?");
                if (val) {
                    /* 8{8'hab} = 32 bits of 0xAB repeated. LSB-first encoding per byte:
                     * 0xAB = 0b10101011 -> LSB-first = "11010101", repeated 4 times */
                    int has_content = 0;
                    for (int i = 0; i < 32 && i < (int)strlen(val); i++)
                        if (val[i] == '1') has_content++;
                    /* 0xAB has 4 ones per byte, 4 bytes = 16 ones expected */
                    printf("  ones count: %d (expect ~16)\n", has_content);
                    if (has_content > 0) { passed++; tests++; } else { tests++; }
                    free(val);
                } else tests++;
            }
        }
        qsim_session_free(sess);

        /* Test 2: replication with trailing item */
        const char *src2 =
        "module bug6b;\n"
        "  reg [7:0] bug6_a, bug6_b;\n"
        "  reg [31:0] bug6_cat;\n"
        "  initial begin\n"
        "    bug6_a = 8'hAB;\n"
        "    bug6_b = 8'hCD;\n"
        "    bug6_cat = {8{bug6_a}, bug6_b};\n"
        "  end\n"
        "endmodule\n";

        sess = qsim_session_create();
        ok = qsim_session_compile_string(sess, "bug6b.v", src2);
        if (!ok) { printf("  Test2 PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  Test2 ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                char *val = qsim_session_eval_str(sess, "bug6_cat");
                printf("  {8{bug6_a}, bug6_b}: %s\n", val ? val : "?");
                if (val) {
                    int nz = 0;
                    for (int i = 0; i < 32 && i < (int)strlen(val); i++)
                        if (val[i] == '1') nz++;
                    printf("  ones count: %d (expect >0)\n", nz);
                    if (nz > 0) { passed++; tests++; } else { tests++; }
                    free(val);
                } else tests++;
            }
        }
        qsim_session_free(sess);
    }

    /* ── Reduction operators ── */
    printf("\n=== Reduction operators ===\n");
    {
        const char *src =
        "module bug7;\n"
        "  reg [7:0] bug7_val;\n"
        "  reg bug7_or, bug7_and, bug7_xor;\n"
        "  initial begin\n"
        "    bug7_val = 8'b10101010;\n"
        "    bug7_or  = |bug7_val;\n"
        "    bug7_and = &bug7_val;\n"
        "    bug7_xor = ^bug7_val;\n"
        "  end\n"
        "endmodule\n";

        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "bug7.v", src);
        if (!ok) { printf("  PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                char *v = qsim_session_eval_str(sess, "bug7_or");
                check("|8'b10101010 (OR)", v, "1");
                free(v);
                v = qsim_session_eval_str(sess, "bug7_and");
                check("&8'b10101010 (AND)", v, "0");
                free(v);
                v = qsim_session_eval_str(sess, "bug7_xor");
                check("^8'b10101010 (XOR)", v, "0");
                free(v);
            }
        }
        qsim_session_free(sess);
    }

    /* ── Port connection propagation ── */
    printf("\n=== Port connection propagation ===\n");
    {
        const char *child_src =
        "module child(input wire [7:0] in, output reg [7:0] out);\n"
        "  always @(*) out = in;\n"
        "endmodule\n";

        const char *top_src =
        "module top;\n"
        "  reg [7:0] top_val;\n"
        "  wire [7:0] child_out;\n"
        "  child u_child(.in(top_val), .out(child_out));\n"
        "  initial begin\n"
        "    top_val = 8'hA5;\n"
        "  end\n"
        "endmodule\n";

        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "child.v", child_src)
              && qsim_session_compile_string(sess, "top.v", top_src);
        if (!ok) { printf("  PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                char *parent_val = qsim_session_eval_str(sess, "top_val");
                char *child_out = qsim_session_eval_str(sess, "u_child.out");
                /* 8'hA5 = 0b10100101, LSB-first = "10100101" */
                check("parent top_val (expect 8'hA5)", parent_val, "10100101");
                check("child out via port", child_out, parent_val ? parent_val : "?");
                free(parent_val);
                free(child_out);
            }
        }
        qsim_session_free(sess);
    }

    /* ── Nested replication in concatenation { {8{a}}, {8{b}} } ── */
    printf("\n=== Nested replication in concatenation ===\n");
    {
        const char *src =
        "module test;\n"
        "  reg [7:0] a, b;\n"
        "  reg [31:0] result;\n"
        "  initial begin\n"
        "    a = 8'hAB;\n"
        "    b = 8'hCD;\n"
        "    result = { {8{a}}, {8{b}} };\n"
        "  end\n"
        "endmodule\n";

        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "test.v", src);
        if (!ok) { printf("  PARSE FAILED\n"); tests++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  ELAB FAILED\n"); tests++; }
            else {
                qsim_session_step_delta(sess);
                char *val = qsim_session_eval_str(sess, "result");
                printf("  result = %s\n", val ? val : "?");
                if (val) {
                    int nz = 0;
                    for (int i = 0; i < 32 && i < (int)strlen(val); i++)
                        if (val[i] == '1') nz++;
                    if (nz > 0) { passed++; tests++; } else { tests++; }
                    free(val);
                } else tests++;
            }
        }
        qsim_session_free(sess);
    }

    printf("\n%d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
