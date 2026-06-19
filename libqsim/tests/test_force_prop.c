/* Debug: list all signals and test force propagation through hierarchy */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Separate modules — grammar only parses one module per compile_string call */
    const char *child_src =
    "module child(input wire [7:0] in, output reg [7:0] out);\n"
    "  always @(*) out = in;\n"
    "endmodule\n";

    const char *top_src =
    "module top;\n"
    "  reg [7:0] parent_sig;\n"
    "  wire [7:0] child_out;\n"
    "  child u_child(.in(parent_sig), .out(child_out));\n"
    "endmodule\n";

    qsim_session_t *sess = qsim_session_create();
    if (!sess) { printf("FAIL: create\n"); return 1; }

    int ok = qsim_session_compile_string(sess, "child.v", child_src);
    if (!ok) { printf("FAIL: compile child\n"); qsim_session_free(sess); return 1; }

    ok = qsim_session_compile_string(sess, "top.v", top_src);
    if (!ok) { printf("FAIL: compile top\n"); qsim_session_free(sess); return 1; }

    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("FAIL: elaborate\n"); qsim_session_free(sess); return 1; }

    /* List all signals */
    int sig_count = qsim_session_get_signal_count(sess);
    printf("signal_count=%d\n", sig_count);
    for (int si = 0; si < sig_count; si++) {
        const char *sn = qsim_session_get_signal_name(sess, si);
        if (sn) printf("  [%d] %s\n", si, sn);
    }

    if (sig_count == 0) {
        printf("FAIL: no signals found\n");
        qsim_session_free(sess);
        return 1;
    }

    /* Test: force parent_sig, step delta, check if child_out propagates */
    printf("\n--- Test 1: Force parent_sig, step delta, check child_out ---\n");
    ok = qsim_session_force_str(sess, "parent_sig", "10101010");
    printf("force_str(parent_sig, 10101010): %s\n", ok ? "OK" : "FAIL");

    /* Step one delta to process port wire events and continuous assigns */
    qsim_session_step_delta(sess);

    char *pval = qsim_session_eval_str(sess, "parent_sig");
    printf("  parent_sig   = %s\n", pval ? pval : "?");
    free(pval);

    char *cval = qsim_session_eval_str(sess, "child_out");
    printf("  child_out    = %s\n", cval ? cval : "?");
    free(cval);

    cval = qsim_session_eval_str(sess, "u_child.in");
    printf("  u_child.in   = %s\n", cval ? cval : "?");
    free(cval);

    cval = qsim_session_eval_str(sess, "u_child.out");
    printf("  u_child.out  = %s\n", cval ? cval : "?");
    free(cval);

    /* Now force a different value and check again */
    printf("\n--- Test 2: Force parent_sig to 11110000, step delta ---\n");
    ok = qsim_session_force_str(sess, "parent_sig", "11110000");
    printf("force_str(parent_sig, 11110000): %s\n", ok ? "OK" : "FAIL");
    qsim_session_step_delta(sess);

    pval = qsim_session_eval_str(sess, "parent_sig");
    printf("  parent_sig   = %s (expect 00001111 LSB-first = 0xF0)\n", pval ? pval : "?");
    free(pval);

    cval = qsim_session_eval_str(sess, "child_out");
    printf("  child_out    = %s (expect same as parent_sig)\n", cval ? cval : "?");
    free(cval);

    cval = qsim_session_eval_str(sess, "u_child.out");
    printf("  u_child.out  = %s (expect same as parent_sig)\n", cval ? cval : "?");
    free(cval);

    /* Test: force at child port directly */
    printf("\n--- Test 3: Force u_child.in directly ---\n");
    ok = qsim_session_force_str(sess, "u_child.in", "11001100");
    printf("force_str(u_child.in, 11001100): %s\n", ok ? "OK" : "FAIL");
    qsim_session_step_delta(sess);

    cval = qsim_session_eval_str(sess, "u_child.out");
    printf("  u_child.out  = %s (expect 00110011 LSB-first)\n", cval ? cval : "?");
    free(cval);

    cval = qsim_session_eval_str(sess, "child_out");
    printf("  child_out    = %s (expect 00110011 LSB-first)\n", cval ? cval : "?");
    free(cval);

    qsim_session_free(sess);
    return 0;
}
