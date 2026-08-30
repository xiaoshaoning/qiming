/* Regression: unpacked-array module ports (parse, connect, indexed read/write).
 * Covers the U74 pattern: input wire [63:0] pmpaddr [0:7] etc. */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, fails = 0;

static void run(const char *label, const char *src, const char *sig,
                const char *expect) {
    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "ap.v", src);
    if (!ok) { printf("  %-12s PARSE FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("  %-12s ELAB FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    qsim_session_step_time(sess, 5);
    char *val = qsim_session_eval_str(sess, sig);
    int good = val && strcmp(val, expect) == 0;
    printf("  %-12s %s = %.40s (expect %.40s) %s\n", label, sig,
           val ? val : "?", expect, good ? "OK" : "FAIL");
    if (!good) fails++;
    tests++;
    free(val);
    qsim_session_free(sess);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Input array port: indexed read + element part-select read.
     * pmpaddr_w[7] = 1 -> pmpaddr[7][0] = 1 -> grant = 1. */
    run("in_indexed",
        "module sub(input wire [63:0] pmpaddr [0:7], output reg grant);\n"
        "  assign grant = pmpaddr[7][0];\n"
        "endmodule\n"
        "module top;\n"
        "  reg [63:0] pmpaddr_w [0:7];\n"
        "  wire grant;\n"
        "  sub s(.pmpaddr(pmpaddr_w), .grant(grant));\n"
        "  initial pmpaddr_w[7] = 64'h1;\n"
        "endmodule\n",
        "grant", "1");
    /* Part-select on an indexed element: pmpaddr[0][33:0] of element 0 = 0x55. */
    run("in_partsel",
        "module sub(input wire [63:0] pmpaddr [0:7], output reg [33:0] out);\n"
        "  assign out = pmpaddr[0][33:0];\n"
        "endmodule\n"
        "module top;\n"
        "  reg [63:0] pmpaddr_w [0:7];\n"
        "  wire [33:0] out;\n"
        "  sub s(.pmpaddr(pmpaddr_w), .out(out));\n"
        "  initial pmpaddr_w[0] = 64'h55;\n"
        "endmodule\n",
        "out", "1010101000000000000000000000000000");
    /* Output array port: child writes elements, parent reads them back.
     * paddr[1] = 11 -> peek (LSB-first 64-bit) = "1101" + 60 zeros. */
    run("out_write",
        "module sub(output reg [63:0] paddr [0:7]);\n"
        "  integer i;\n"
        "  initial for (i=0;i<8;i=i+1) paddr[i] = 64'd10 + i;\n"
        "endmodule\n"
        "module top;\n"
        "  wire [63:0] paddr_w [0:7];\n"
        "  wire [63:0] peek = paddr_w[1];\n"
        "  sub s(.paddr(paddr_w));\n"
        "endmodule\n",
        "peek", "1101000000000000000000000000000000000000000000000000000000000000");

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
