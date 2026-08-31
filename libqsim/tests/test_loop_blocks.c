/* Regression: procedural loop semantics (for/while/repeat) — blocking
 * completion, entry-condition check, budget fallback on huge loops. */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, fails = 0;

static void run(const char *label, const char *src, const char *sig,
                const char *expect) {
    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "probe.v", src);
    if (!ok) { printf("  %-14s PARSE FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("  %-14s ELAB FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    qsim_session_step_delta(sess);
    char *val = qsim_session_eval_str(sess, sig);
    int good = val && strcmp(val, expect) == 0;
    printf("  %-14s %s = %s (expect %s) %s\n", label, sig,
           val ? val : "?", expect, good ? "OK" : "FAIL");
    if (!good) fails++; tests++;
    free(val);
    qsim_session_free(sess);
}

static void run_time(const char *label, const char *src, const char *sig,
                    const char *expect, uint64_t until) {
    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "probe.v", src);
    if (!ok) { printf("  %-14s PARSE FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("  %-14s ELAB FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    qsim_session_step_time(sess, until);
    char *val = qsim_session_eval_str(sess, sig);
    int good = val && strcmp(val, expect) == 0;
    printf("  %-14s %s = %s (expect %s) %s\n", label, sig,
           val ? val : "?", expect, good ? "OK" : "FAIL");
    if (!good) fails++;
    tests++;
    free(val);
    qsim_session_free(sess);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* sum is 32-bit; LSB-first dump: value 10 = 0b1010 -> "0101 0000..." */
    run("for_only",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<5;i=i+1) sum=sum+i; end endmodule\n",
        "sum", "01010000000000000000000000000000");
    run("for_only_i",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<5;i=i+1) sum=sum+i; end endmodule\n",
        "i", "10100000000000000000000000000000");
    run("while_only",
        "module t; integer i, sum; initial begin i=0; sum=0;\n"
        " while (i<5) begin sum=sum+i; i=i+1; end end endmodule\n",
        "sum", "01010000000000000000000000000000");
    /* Doc's exact combined repro: was sum=15(0xF), i=6 */
    run("combined",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<5;i=i+1) sum=sum+i;\n"
        " i=0; sum=0;\n"
        " while (i<5) begin sum=sum+i; i=i+1; end end endmodule\n",
        "sum", "01010000000000000000000000000000");
    run("combined_i",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<5;i=i+1) sum=sum+i;\n"
        " i=0; sum=0;\n"
        " while (i<5) begin sum=sum+i; i=i+1; end end endmodule\n",
        "i", "10100000000000000000000000000000");
    /* False initial condition: body must NOT run. Was sum=10, i=11. */
    run("for_false",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=10;i<5;i=i+1) sum=sum+i; end endmodule\n",
        "sum", "00000000000000000000000000000000");
    run("for_false_i",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=10;i<5;i=i+1) sum=sum+i; end endmodule\n",
        "i", "01010000000000000000000000000000");
    run("while_false",
        "module t; integer i, sum; initial begin i=10; sum=0;\n"
        " while (i<5) sum=sum+1; end endmodule\n",
        "sum", "00000000000000000000000000000000");
    /* repeat(0): was runs-once */
    run("repeat0",
        "module t; integer k, s; initial begin s=0;\n"
        " repeat (0) s=s+1; end endmodule\n",
        "s", "00000000000000000000000000000000");
    run("repeat3",
        "module t; integer k, s; initial begin s=0;\n"
        " repeat (3) s=s+2; end endmodule\n",
        "s", "01100000000000000000000000000000");
    /* Big loop (10k iterations): budget fallback, sum=0..9999=49995000 */
    run("big_loop",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<10000;i=i+1) sum=sum+i; end endmodule\n",
        "sum", "00011111001110110101111101000000");
    /* $display between loops sees the final value (was mid-loop 0) */
    run("display_mid",
        "module t; integer i, sum; initial begin sum=0;\n"
        " for (i=0;i<5;i=i+1) sum=sum+i;\n"
        " $display(\"sum=%d\", sum); i=0; sum=0; end endmodule\n",
        "sum", "00000000000000000000000000000000");

    /* Time-based control flow: consecutive delays + trailing $stop must fire
     * at their own times (restructure_delays continuation bug: $stop used to
     * execute at the first delay's fire time). x=2 -> LSB-first "0100". */
    run_time("consec_delays",
        "module t; reg [3:0] x; initial begin x=0;\n"
        " #5 x=1; #5 x=2; $stop; end endmodule\n",
        "x", "0100", 20);
    /* Delay inside a loop: each iteration advances one time unit. x=3 -> "1100". */
    run_time("delay_in_loop",
        "module t; integer i; reg [3:0] x; initial begin x=0;\n"
        " for (i=0;i<3;i=i+1) #1 x=x+1; end endmodule\n",
        "x", "1100", 10);
    /* Clock generator: forever #5 toggles, counter on posedge. cnt=4 -> "0010". */
    run_time("clock_gen",
        "module t; reg clk=0; reg [3:0] cnt=0;\n"
        " always #5 clk=~clk;\n"
        " always @(posedge clk) cnt=cnt+1;\n"
        " initial #45 $stop; endmodule\n",
        "cnt", "1010", 50);
    /* Inline loop-variable declaration: for (integer i = 0; ...) — the
     * FPU/CSR style. sum=0+1+2+3+4=10 -> LSB-first 32-bit. */
    run("for_integer",
        "module t; integer sum; initial begin sum=0;\n"
        " for (integer i = 0; i < 5; i = i + 1) sum = sum + i; end endmodule\n",
        "sum", "01010000000000000000000000000000");
    /* longint (64-bit integer) decl, used in a block. */
    run("longint_decl",
        "module t; longint li; integer s; initial begin\n"
        " li = 64'd7; s = li; end endmodule\n",
        "s", "11100000000000000000000000000000");

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
