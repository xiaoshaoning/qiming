/* Qiming bug reproduction tests for bugs 6-10.
   Build: gcc -I D:/codes/qiming/libqsim/include repro.c D:/codes/qiming/build/libqsim.a -lssp
   Run:   ./repro.exe
   Each test returns 0 on success (bug NOT reproduced) or crashes.
   Bugs that STILL reproduce will segfault or print "FAIL".
*/
#include "libqsim/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_6_ifdef_offset(void) {
    /* Bug 6: ifdef blocks inside module cause parse errors at wrong lines.
       The parser miscomputes source-position tracking after ifdef.
       Source: a module with ifdef blocks and valid Verilog between them. */
    printf("=== Bug 6: ifdef line-offset parse errors ===\n");
    const char *src =
        "module t(input clk, rst, output reg [63:0] out);\n"
        "`ifdef FLAG\n"
        "  wire cache_hit = 1'b0;\n"
        "  wire cache_stall = 1'b0;\n"
        "`endif\n"
        "  reg [63:0] r1, r2, r3;\n"
        "  wire [6:0] opcode = r1 & 7'h7F;\n"
        "  wire [4:0] rd = (r1 >> 7) & 5'h1F;\n"
        "  reg [63:0] csr_mtvec, csr_mepc, csr_mcause;\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin r1<=0; r2<=0; r3<=0; out<=0; end\n"
        "    else begin r1<=r1+1; out<=r1; end\n"
        "  end\n"
        "endmodule\n";
    qsim_session_t *s = qsim_session_create();
    int ok = qsim_session_compile_string(s, "t.v", src);
    if (!ok) {
        const char *log = qsim_session_get_log(s);
        printf("  Compile FAIL. Log:\n%s\n", log ? log : "(null)");
        // Check if error line is misreported
        if (log && strstr(log, "div_dividend")) {
            printf("  CONFIRMED: error reported at wrong line (div_dividend)\n");
        } else {
            printf("  Error at reported line (check if it makes sense)\n");
        }
        qsim_session_free(s); return 1;
    }
    ok = qsim_session_elaborate(s);
    printf("  Elaborate: %s\n", ok ? "OK" : "FAIL");
    qsim_session_free(s);
    return ok ? 0 : 1;
}

static int test_7_eval_str(void) {
    /* Bug 7: eval_str on large signals segfaults.
       Creates a 32768-bit array and tries to read it as a string. */
    printf("=== Bug 7: eval_str on large signal ===\n");
    const char *src = "module t; reg [31:0] mem [0:1023]; endmodule";
    qsim_session_t *s = qsim_session_create();
    int ok = qsim_session_compile_string(s, "t.v", src);
    ok = ok && qsim_session_elaborate(s);
    if (!ok) { printf("  SKIP: setup failed\n"); qsim_session_free(s); return 0; }
    printf("  Signal width: 32768 bits. Calling eval_str...\n"); fflush(stdout);
    char *val = qsim_session_eval_str(s, "mem");
    if (val) {
        printf("  OK: got string, len=%zu\n", strlen(val));
        qsim_session_free_str(val);
    } else {
        printf("  FAIL: returned NULL or crashed\n");
    }
    qsim_session_free(s);
    return (val != NULL) ? 0 : 1;
}

static int test_8_step_delta_large(void) {
    /* Bug 8: step_delta segfaults above ~440 signals.
       Generates a module with ~500 signals to push past the limit. */
    printf("=== Bug 8: step_delta above ~440 signals ===\n");
    qsim_session_t *s = qsim_session_create();
    /* Generate ~500 wires + registers */
    char buf[16384];
    int pos = 0;
    pos += sprintf(buf + pos, "module t(input clk, rst, output reg done);\n");
    for (int i = 0; i < 100; i++)
        pos += sprintf(buf + pos, "  reg [63:0] r%d;\n", i);
    for (int i = 0; i < 350; i++)
        pos += sprintf(buf + pos, "  wire [3:0] w%d = r%d[3:0];\n", i, (i%100));
    pos += sprintf(buf + pos,
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin done<=0; ");
    for (int i = 0; i < 100; i++) pos += sprintf(buf + pos, "r%d<=0;", i);
    pos += sprintf(buf + pos,
        " end else begin\n"
        "      if (r0 > 10) done<=1;\n");
    for (int i = 0; i < 100; i++) pos += sprintf(buf + pos, "r%d<=r%d+1;", i, (i+1)%100);
    pos += sprintf(buf + pos, "    end\n  end\nendmodule\n");
    
    int ok = qsim_session_compile_string(s, "t.v", buf);
    if (!ok) { printf("  SKIP: compile failed\n"); qsim_session_free(s); return 0; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate failed\n"); qsim_session_free(s); return 1; }
    
    int sigs = qsim_session_get_signal_count(s);
    printf("  Signal count: %d\n", sigs); fflush(stdout);
    
    ok = qsim_session_step_delta(s);
    printf("  step_delta returned: %d\n", ok);
    qsim_session_free(s);
    return (ok >= 0) ? 0 : 1;
}

static int test_10_large_array(void) {
    /* Bug 10: Large unpacked arrays + non-const index in assign crashes.
       Tests at various array sizes to find the threshold. */
    printf("=== Bug 10: Large array + var-index in assign ===\n");
    struct { int size; const char *desc; } tests[] = {
        {64,  "64 entries"}, {128, "128 entries"}, {256, "256 entries"},
        {512, "512 entries"}, {1024, "1024 entries"}, {2048, "2048 entries"}
    };
    for (int t = 0; t < 6; t++) {
        int sz = tests[t].size;
        printf("  Test: %s... ", tests[t].desc); fflush(stdout);
        qsim_session_t *s = qsim_session_create();
        char src[512];
        int idx_bits = 0; for (int x = sz-1; x > 0; x >>= 1) idx_bits++;
        int addr_bits = idx_bits + 3;
        sprintf(src,
            "module t(input [63:0] a); reg [63:0] d[0:%d]; wire [63:0] r=d[{a[%d:7],3'd0}]; endmodule",
            sz-1, addr_bits-1);
        int ok = qsim_session_compile_string(s, "t.v", src);
        if (!ok) { printf("compile FAIL\n"); qsim_session_free(s); continue; }
        ok = qsim_session_elaborate(s);
        printf("%s\n", ok ? "elaborate OK" : "elaborate FAIL");
        qsim_session_free(s);
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Qiming Bug Reproduction Suite ===\n\n");
    test_6_ifdef_offset();  printf("\n");
    test_7_eval_str();      printf("\n");
    test_8_step_delta_large(); printf("\n");
    test_10_large_array();  printf("\n");
    printf("Done.\n");
    return 0;
}
