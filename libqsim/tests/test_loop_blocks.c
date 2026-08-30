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

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
