/* Test output port propagation through hierarchy (Bug 14/15 investigation)
 *
 * Findings:
 * - Port wires are unidirectional: INPUT creates parent→child, OUTPUT creates child→parent
 * - Forcing a parent wire connected to child OUTPUT does NOT propagate to the child port
 *   (port wire direction is OUTPUT-only, no reverse path)
 * - Forcing a parent wire connected to child INPUT propagates correctly after step_delta
 * - Forcing child input port propagates through always @(*) body to child output immediately
 *   (blocking assign in process body writes directly, no event queue needed)
 * - For OUTPUT ports: changes always flow child→parent, never parent→child
 */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int check_str(qsim_session_t *sess, const char *name, const char *expected) {
    char *val = qsim_session_eval_str(sess, name);
    int ok = val && strcmp(val, expected) == 0;
    printf("  %s = %s (expect %s) %s\n", name, val ? val : "?", expected, ok ? "OK" : "FAIL");
    free(val);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int all_pass = 1;

    const char *child_src =
    "module child(input wire [7:0] in, output reg [7:0] out);\n"
    "  always @(*) out = in;\n"
    "endmodule\n";

    const char *top_src =
    "module top;\n"
    "  reg [7:0] stimulus;\n"
    "  wire [7:0] observed;\n"
    "  child u_child(.in(stimulus), .out(observed));\n"
    "endmodule\n";

    qsim_session_t *sess = qsim_session_create();
    if (!sess) { printf("FAIL: create\n"); return 1; }

    int ok = qsim_session_compile_string(sess, "child.v", child_src);
    if (!ok) { printf("FAIL: compile child\n"); return 1; }
    ok = qsim_session_compile_string(sess, "top.v", top_src);
    if (!ok) { printf("FAIL: compile top\n"); return 1; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("FAIL: elaborate\n"); return 1; }

    /* Initialize: set stimulus, step delta to propagate through hierarchy */
    qsim_session_set_str(sess, "stimulus", "10101010");
    qsim_session_step_delta(sess);

    /* Test 1: Parent reg → child input port → child output → parent wire */
    printf("Test 1: set stimulus, propagate through hierarchy\n");
    all_pass &= check_str(sess, "stimulus", "01010101");
    all_pass &= check_str(sess, "u_child.in", "01010101");
    all_pass &= check_str(sess, "u_child.out", "01010101");
    all_pass &= check_str(sess, "observed", "01010101");

    /* Test 2: Force parent wire connected to child OUTPUT — does NOT propagate to child */
    printf("\nTest 2: force observed (parent wire, connected to child OUTPUT)\n");
    ok = qsim_session_force_str(sess, "observed", "11101110");
    printf("  force_str(observed): %s\n", ok ? "OK" : "FAIL");
    all_pass &= check_str(sess, "observed", "01110111");
    /* u_child.out should NOT change — port wire is OUTPUT-only (child→parent) */
    all_pass &= check_str(sess, "u_child.out", "01010101");
    qsim_session_step_delta(sess);
    all_pass &= check_str(sess, "observed", "01110111");
    all_pass &= check_str(sess, "u_child.out", "01010101");
    printf("  => OUTPUT port wires are unidirectional (child→parent only). Expected.\n");

    /* Test 3: Release observed, set stimulus to a new value to trigger propagation */
    qsim_session_release(sess, "observed");
    qsim_session_set_str(sess, "stimulus", "11001100");
    qsim_session_step_delta(sess);
    all_pass &= check_str(sess, "stimulus", "00110011");
    all_pass &= check_str(sess, "u_child.in", "00110011");
    all_pass &= check_str(sess, "u_child.out", "00110011");
    all_pass &= check_str(sess, "observed", "00110011");

    /* Test 4: Force parent reg connected to child INPUT — propagates with delta */
    printf("\nTest 3: force stimulus (parent reg, connected to child INPUT)\n");
    ok = qsim_session_force_str(sess, "stimulus", "11110000");
    printf("  force_str(stimulus): %s\n", ok ? "OK" : "FAIL");
    all_pass &= check_str(sess, "stimulus", "00001111");
    /* after force: stimulus changed, but port wire events not yet processed */
    all_pass &= check_str(sess, "u_child.in", "00110011");
    qsim_session_step_delta(sess);
    /* after delta: port wire events processed, always @(*) re-evaluated */
    all_pass &= check_str(sess, "u_child.in", "00001111");
    all_pass &= check_str(sess, "u_child.out", "00001111");
    all_pass &= check_str(sess, "observed", "00001111");

    /* Test 5: Force child input port directly — propagates immediately */
    printf("\nTest 4: force u_child.in (child input port, triggers always @(*))\n");
    ok = qsim_session_force_str(sess, "u_child.in", "11001100");
    printf("  force_str(u_child.in): %s\n", ok ? "OK" : "FAIL");
    all_pass &= check_str(sess, "u_child.in", "00110011");
    /* always @(*) out = in runs immediately — blocking assign writes directly */
    all_pass &= check_str(sess, "u_child.out", "00110011");

    printf("\n%s\n", all_pass ? "ALL PASS" : "SOME FAILED");
    qsim_session_free(sess);
    return all_pass ? 0 : 1;
}
