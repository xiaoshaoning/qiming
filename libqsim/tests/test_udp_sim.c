/* UDP instance simulation tests */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, passed = 0;

static int check_str(qsim_session_t *sess, const char *name, const char *expected) {
    char *actual = qsim_session_eval_str(sess, name);
    if (!actual) return 0;
    int ok = strcmp(actual, expected) == 0;
    qsim_session_free_str(actual);
    return ok;
}

#define TEST(name, body) do { \
    tests++; \
    printf("  %s ... ", name); \
    int _ok = 1; \
    do { body } while(0); \
    if (_ok) { printf("OK\n"); passed++; } \
    else { printf("FAIL\n"); } \
} while(0)

int main(void) {
    setbuf(stdout, NULL);

    /* Test 1: Combinational AND UDP */
    printf("--- Test 1: Combinational AND UDP ---\n");
    {
        qsim_session_t *sess = qsim_session_create();
        /* Compile primitive and module separately (grammar design_file only accepts one unit) */
        TEST("compile", {
            if (!qsim_session_compile_string(sess, "prim.v",
                    "primitive my_and(y, a, b);\n"
                    "  output y;\n"
                    "  input a, b;\n"
                    "  table\n"
                    "    0 0 : 0;\n"
                    "    0 1 : 0;\n"
                    "    1 0 : 0;\n"
                    "    1 1 : 1;\n"
                    "  endtable\n"
                    "endprimitive\n")) { _ok = 0; break; }
            if (!qsim_session_compile_string(sess, "top.v",
                    "module top(y, a, b);\n"
                    "  output y;\n"
                    "  input a, b;\n"
                    "  my_and u1(.y(y), .a(a), .b(b));\n"
                    "endmodule\n")) { _ok = 0; break; }
        });
        TEST("elaborate", {
            if (!qsim_session_elaborate(sess)) { _ok = 0; break; }
        });

        /* Check initial state */
        TEST("y=X before any inputs", {
            if (!check_str(sess, "y", "X")) _ok = 0;
        });

        /* Set a=1, b=1 → y should become 1 */
        qsim_session_force_str(sess, "a", "1");
        qsim_session_force_str(sess, "b", "1");
        qsim_session_step_delta(sess);
        TEST("y=1 after a=1,b=1", {
            if (!check_str(sess, "y", "1")) _ok = 0;
        });

        /* Set a=0 → y should become 0 */
        qsim_session_force_str(sess, "a", "0");
        qsim_session_step_delta(sess);
        TEST("y=0 after a=0,b=1", {
            if (!check_str(sess, "y", "0")) _ok = 0;
        });

        /* Set a=1,b=0 → y=0 */
        qsim_session_force_str(sess, "a", "1");
        qsim_session_force_str(sess, "b", "0");
        qsim_session_step_delta(sess);
        TEST("y=0 after a=1,b=0", {
            if (!check_str(sess, "y", "0")) _ok = 0;
        });

        /* a=1,b=1 → y=1 */
        qsim_session_force_str(sess, "b", "1");
        qsim_session_step_delta(sess);
        TEST("y=1 after a=1,b=1", {
            if (!check_str(sess, "y", "1")) _ok = 0;
        });

        qsim_session_free(sess);
    }

    /* Test 2: Combinational OR UDP with X handling */
    printf("\n--- Test 2: Combinational OR UDP with X ---\n");
    {
        qsim_session_t *sess = qsim_session_create();
        TEST("compile+elab", {
            if (!qsim_session_compile_string(sess, "prim.v",
                    "primitive my_or(y, a, b);\n"
                    "  output y;\n"
                    "  input a, b;\n"
                    "  table\n"
                    "    0 0 : 0;\n"
                    "    0 1 : 1;\n"
                    "    1 0 : 1;\n"
                    "    1 1 : 1;\n"
                    "    x 0 : x;\n"
                    "    x 1 : 1;\n"
                    "  endtable\n"
                    "endprimitive\n")) { _ok = 0; break; }
            if (!qsim_session_compile_string(sess, "top.v",
                    "module top(y, a, b);\n"
                    "  output y;\n"
                    "  input a, b;\n"
                    "  my_or u1(.y(y), .a(a), .b(b));\n"
                    "endmodule\n")) { _ok = 0; break; }
            if (!qsim_session_elaborate(sess)) { _ok = 0; break; }
        });

        /* a=0,b=0 → y=0 */
        qsim_session_force_str(sess, "a", "0");
        qsim_session_force_str(sess, "b", "0");
        qsim_session_step_delta(sess);
        TEST("y=0 after a=0,b=0", {
            if (!check_str(sess, "y", "0")) _ok = 0;
        });

        /* a=1,b=0 → y=1 */
        qsim_session_force_str(sess, "a", "1");
        qsim_session_step_delta(sess);
        TEST("y=1 after a=1,b=0", {
            if (!check_str(sess, "y", "1")) _ok = 0;
        });

        /* a=x,b=0 → y=x */
        qsim_session_force_str(sess, "a", "x");
        qsim_session_step_delta(sess);
        TEST("y=X after a=X,b=0", {
            if (!check_str(sess, "y", "X")) _ok = 0;
        });

        /* a=x,b=1 → y=1 */
        qsim_session_force_str(sess, "b", "1");
        qsim_session_step_delta(sess);
        TEST("y=1 after a=x,b=1", {
            if (!check_str(sess, "y", "1")) _ok = 0;
        });

        qsim_session_free(sess);
    }

    /* Test 3: Sequential D flip-flop UDP */
    printf("\n--- Test 3: Sequential DFF UDP ---\n");
    {
        qsim_session_t *sess = qsim_session_create();
        TEST("compile+elab", {
            if (!qsim_session_compile_string(sess, "prim.v",
                    "primitive dff(q, clk, d);\n"
                    "  output reg q;\n"
                    "  input clk, d;\n"
                    "  table\n"
                    "    (01) 0 : ? : 0;\n"
                    "    (01) 1 : ? : 1;\n"
                    "    (0x) 0 : ? : -;\n"
                    "    (0x) 1 : ? : -;\n"
                    "  endtable\n"
                    "endprimitive\n")) { _ok = 0; break; }
            if (!qsim_session_compile_string(sess, "top.v",
                    "module top(q, clk, d);\n"
                    "  output q;\n"
                    "  input clk, d;\n"
                    "  dff u1(.q(q), .clk(clk), .d(d));\n"
                    "endmodule\n")) { _ok = 0; break; }
            if (!qsim_session_elaborate(sess)) { _ok = 0; break; }
        });

        /* Initial state: q=X */
        TEST("q=X initially", {
            if (!check_str(sess, "q", "X")) _ok = 0;
        });

        /* Set clk=0, d=1, step: no posedge yet, q stays X */
        qsim_session_force_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_force_str(sess, "d", "1");
        qsim_session_step_delta(sess);
        TEST("q=X after clk=0,d=1 (no edge)", {
            if (!check_str(sess, "q", "X")) _ok = 0;
        });

        /* Posedge clk=1 with d=1 → q=1 */
        qsim_session_force_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        TEST("q=1 after posedge clk,d=1", {
            if (!check_str(sess, "q", "1")) _ok = 0;
        });

        /* Set d=0, no clock edge → q stays 1 */
        qsim_session_force_str(sess, "d", "0");
        qsim_session_step_delta(sess);
        TEST("q=1 after d=0 (no edge)", {
            if (!check_str(sess, "q", "1")) _ok = 0;
        });

        /* Posedge clk=1 with d=0 → q=0 */
        /* clk was already 1, toggle low first then high */
        qsim_session_force_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_force_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        TEST("q=0 after posedge clk,d=0", {
            if (!check_str(sess, "q", "0")) _ok = 0;
        });

        qsim_session_free(sess);
    }

    /* Test 4: Multi-definition file — primitive and module in one source */
    printf("\n--- Test 4: Multi-definition file ---\n");
    {
        qsim_session_t *sess = qsim_session_create();
        const char *src =
            "primitive my_and(y, a, b);\n"
            "  output y;\n"
            "  input a, b;\n"
            "  table\n"
            "    0 0 : 0;\n"
            "    0 1 : 0;\n"
            "    1 0 : 0;\n"
            "    1 1 : 1;\n"
            "  endtable\n"
            "endprimitive\n"
            "module top(y, a, b);\n"
            "  output y;\n"
            "  input a, b;\n"
            "  my_and u1(.y(y), .a(a), .b(b));\n"
            "endmodule\n";
        TEST("single-file compile", {
            if (!qsim_session_compile_string(sess, "combined.v", src)) { _ok = 0; break; }
            if (!qsim_session_elaborate(sess)) { _ok = 0; break; }
        });

        TEST("y=X initially", {
            if (!check_str(sess, "y", "X")) _ok = 0;
        });

        qsim_session_force_str(sess, "a", "1");
        qsim_session_force_str(sess, "b", "1");
        qsim_session_step_delta(sess);
        TEST("y=1 after a=1,b=1", {
            if (!check_str(sess, "y", "1")) _ok = 0;
        });

        qsim_session_free(sess);
    }

    printf("\n%d/%d passed\n", passed, tests);
    return (passed == tests) ? 0 : 1;
}
