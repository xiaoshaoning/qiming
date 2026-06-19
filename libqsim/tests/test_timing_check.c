/* Phase 4c -- Timing check monitoring integration test. */
#include "libqsim/simulator.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char last_display[1024];

static void test_display_cb(uir_sim_context_t *ctx, const char *msg, void *user_data) {
    (void)ctx;
    (void)user_data;
    if (msg)
        strncpy(last_display, msg, sizeof(last_display) - 1);
    last_display[sizeof(last_display) - 1] = '\0';
}

typedef struct {
    const char *name;
    const char *src;
    int expect_violation;
} test_entry_t;

int main(void) {
    setbuf(stdout, NULL);

    test_entry_t tests[] = {
        {"$setup violation",
         "module test;\n"
         "  reg d, clk;\n"
         "  specify\n"
         "    $setup(d, clk, 10);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0; d = 0;\n"
         "    #5 d = 1;\n"
         "    #3 clk = 1;\n"
         "  end\n"
         "endmodule\n", 1},

        {"$setup ok",
         "module test;\n"
         "  reg d, clk;\n"
         "  specify\n"
         "    $setup(d, clk, 10);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0; d = 0;\n"
         "    #2 d = 1;\n"
         "    #13 clk = 1;\n"
         "  end\n"
         "endmodule\n", 0},

        {"$hold violation",
         "module test;\n"
         "  reg d, clk;\n"
         "  specify\n"
         "    $hold(clk, d, 5);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0; d = 0;\n"
         "    #8 clk = 1;\n"
         "    #2 d = 1;\n"
         "  end\n"
         "endmodule\n", 1},

        {"$hold ok",
         "module test;\n"
         "  reg d, clk;\n"
         "  specify\n"
         "    $hold(clk, d, 5);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0; d = 0;\n"
         "    #8 clk = 1;\n"
         "    #7 d = 1;\n"
         "  end\n"
         "endmodule\n", 0},

        {"$width violation",
         "module test;\n"
         "  reg clk;\n"
         "  specify\n"
         "    $width(clk, 10);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0;\n"
         "    #5 clk = 1;\n"
         "    #3 clk = 0;\n"
         "  end\n"
         "endmodule\n", 1},

        {"$width ok",
         "module test;\n"
         "  reg clk;\n"
         "  specify\n"
         "    $width(clk, 10);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0;\n"
         "    #5 clk = 1;\n"
         "    #15 clk = 0;\n"
         "  end\n"
         "endmodule\n", 0},

        {"$period violation",
         "module test;\n"
         "  reg clk;\n"
         "  specify\n"
         "    $period(clk, 20);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0;\n"
         "    #5 clk = 1;\n"
         "    #1 clk = 0;\n"
         "    #7 clk = 1;\n"
         "  end\n"
         "endmodule\n", 1},

        {"$period ok",
         "module test;\n"
         "  reg clk;\n"
         "  specify\n"
         "    $period(clk, 20);\n"
         "  endspecify\n"
         "  initial begin\n"
         "    clk = 0;\n"
         "    #5 clk = 1;\n"
         "    #1 clk = 0;\n"
         "    #24 clk = 1;\n"
         "  end\n"
         "endmodule\n", 0},
    };
    int ntests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;

    for (int i = 0; i < ntests; i++) {
        /* Build sim: compile, elaborate, create (keep cr alive until after sim_destroy) */
        qsim_compile_input_t input;
        memset(&input, 0, sizeof(input));
        input.sources = &tests[i].src;
        input.source_count = 1;
        qsim_compile_result_t *cr = qsim_compile(&input);
        if (!cr || !cr->success) { printf("FAIL: %s (compile)\n", tests[i].name); continue; }

        uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
        uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
        if (!elab || !elab->success) { printf("FAIL: %s (elab)\n", tests[i].name); qsim_compile_result_free(cr); continue; }
        uir_elab_result_free(elab);

        uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
        if (!sim) { printf("FAIL: %s (sim_create)\n", tests[i].name); qsim_compile_result_free(cr); continue; }

        uir_sim_set_sys_display_callback(sim, test_display_cb, NULL);
        last_display[0] = '\0';

        uir_sim_run(sim, 200);

        int has_violation = (strstr(last_display, "violation") != NULL);
        int ok = (has_violation == tests[i].expect_violation);
        if (ok) {
            printf("  PASS: %s\n", tests[i].name);
            passed++;
        } else if (has_violation) {
            printf("  FAIL: %s -- unexpected violation: %s\n", tests[i].name, last_display);
        } else {
            printf("  FAIL: %s -- expected violation, got: [%s]\n", tests[i].name, last_display);
        }

        uir_sim_destroy(sim);
        qsim_compile_result_free(cr);
    }

    printf("\nTiming check: %d/%d passed\n", passed, ntests);
    return passed == ntests ? 0 : 1;
}
