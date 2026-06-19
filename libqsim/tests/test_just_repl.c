#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>

static int tests = 0, passed = 0;

static void test_case(const char *name, const char *module, int width, const char *expected) {
    tests++;
    qsim_session_t *s = qsim_session_create();
    qsim_session_compile_string(s, "t.v", module);
    qsim_session_elaborate(s);
    qsim_session_step_delta(s);
    char result[128];
    qsim_bit_vector_t v = qsim_session_eval(s, "out");
    int pos = 0;
    for (int i = width - 1; i >= 0; i--) {
        qsim_logic_state_t st = qsim_bit_get(&v, i).state;
        result[pos++] = st == QSIM_1 ? '1' : st == QSIM_0 ? '0' : 'X';
    }
    result[pos] = '\0';
    if (strcmp(result, expected) == 0) {
        printf("  PASS: %s = %s\n", name, result);
        passed++;
    } else {
        printf("  FAIL: %s = %s (expected %s)\n", name, result, expected);
    }
    qsim_session_free(s);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Replication+Concat Test Suite\n=============================\n");

    /* Test 1: replication first + trailing literal — this was Bug 10 */
    test_case("repl+trailing (Bug 10)",
        "module t(output [6:0] out); assign out = {4{1'b1}, 3'b000}; endmodule\n",
        7, "1111000");

    /* Test 2: standalone replication (no trailing item) */
    test_case("standalone repl",
        "module t(output [3:0] out); assign out = {4{1'b1}}; endmodule\n",
        4, "1111");

    /* Test 3: replication with wider inner expr */
    test_case("wider repl",
        "module t(output [7:0] out); assign out = {2{4'b1010}}; endmodule\n",
        8, "10101010");

    /* Test 4: three items: repl, lit, lit */
    test_case("repl+lit+lit",
        "module t(output [5:0] out); assign out = {2{1'b1}, 2'b00, 2'b11}; endmodule\n",
        6, "110011");

    /* Test 5: plain concat (no replication, regression check) */
    test_case("plain concat",
        "module t(output [7:0] out); assign out = {4'b1111, 4'b0000}; endmodule\n",
        8, "11110000");

    /* Test 6: three-item plain concat (regression) */
    test_case("three-item concat",
        "module t(output [5:0] out); assign out = {1'b1, 2'b00, 3'b111}; endmodule\n",
        6, "100111");

    /* Known limitation: replication as non-first concat item
     * e.g. {3'b000, 4{1'b1}} or {4{1'b1}, 4{1'b0}} — the grammar
     * intercepts `4` as a plain number in primary_expr before
     * concat_expr gets a chance. This is a separate grammar issue. */

    printf("\nResults: %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
