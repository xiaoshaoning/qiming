/* Regression: hierarchical references — dut.mem[0] = ..., top.dut.mem writes,
 * $readmemh with an instance-array target. */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, fails = 0;

static void check(const char *label, const char *actual, const char *expect) {
    int ok = actual && strcmp(actual, expect) == 0;
    printf("  %-22s %s (expect %s) %s\n", label,
           actual ? actual : "?", expect, ok ? "OK" : "FAIL");
    if (!ok) fails++;
    tests++;
    free((void *)actual);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Hierarchical assignment: dut.mem[0] and top.dut.mem[3].
     * mem = 0xff,0x00,0x00,0x5a -> 32-bit LSB-first dump. */
    {
        const char *src =
        "module top;\n"
        "  reg clk;\n"
        "  sub dut (.clk(clk));\n"
        "endmodule\n"
        "module sub (clk);\n"
        "  input clk;\n"
        "  reg [7:0] mem [0:3];\n"
        "endmodule\n"
        "module tb;\n"
        "  initial begin\n"
        "    dut.mem[0] = 8'hff;\n"
        "    top.dut.mem[3] = 8'h5a;\n"
        "  end\n"
        "endmodule\n";
        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "hier.v", src);
        if (!ok) { printf("  hier_assign PARSE FAILED\n"); tests++; fails++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  hier_assign ELAB FAILED: %s\n", qsim_session_get_log(sess)); tests++; fails++; }
            else {
                qsim_session_step_time(sess, 5);
                check("hier_assign mem",
                      qsim_session_eval_str(sess, "dut.mem"),
                      "11111111XXXXXXXXXXXXXXXX01011010");
            }
        }
        qsim_session_free(sess);
    }

    /* $readmemh into an instance array. */
    {
        FILE *f = fopen("_hier_mem.hex", "w");
        if (f) { fprintf(f, "ff\n5a\n12\n34\n"); fclose(f); }
        char src[512];
        snprintf(src, sizeof(src),
        "module top;\n"
        "  reg clk;\n"
        "  sub dut (.clk(clk));\n"
        "endmodule\n"
        "module sub (clk);\n"
        "  input clk;\n"
        "  reg [7:0] imem [0:3];\n"
        "endmodule\n"
        "module tb;\n"
        "  initial $readmemh(\"_hier_mem.hex\", dut.imem);\n"
        "endmodule\n");
        qsim_session_t *sess = qsim_session_create();
        int ok = qsim_session_compile_string(sess, "hier.v", src);
        if (!ok) { printf("  hier_readmemh PARSE FAILED\n"); tests++; fails++; }
        else {
            ok = qsim_session_elaborate(sess);
            if (!ok) { printf("  hier_readmemh ELAB FAILED\n"); tests++; fails++; }
            else {
                qsim_session_step_time(sess, 5);
                check("hier_readmemh imem",
                      qsim_session_eval_str(sess, "dut.imem"),
                      "11111111010110100100100000101100");
            }
        }
        qsim_session_free(sess);
        remove("_hier_mem.hex");
    }

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
