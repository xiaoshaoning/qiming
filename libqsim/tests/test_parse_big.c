#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Test 1: reg without initializer */
    const char *src1 = "module m; reg clk; wire x; assign x = clk; endmodule";
    qsim_session_t *s1 = qsim_session_create();
    printf("No init: %s\n", qsim_session_compile_string(s1, "t.v", src1) ? "OK" : "FAIL");
    qsim_session_free(s1);

    /* Test 2: reg with initializer via initial block */
    const char *src2 = "module m; reg clk; initial clk = 0; wire x; assign x = clk; endmodule";
    qsim_session_t *s2 = qsim_session_create();
    printf("Init block: %s\n", qsim_session_compile_string(s2, "t.v", src2) ? "OK" : "FAIL");
    qsim_session_free(s2);

    /* Test 3: The exact perf test source, fixed to use initial block */
    const char *src3 =
        "module bench;\n"
        "  reg clk;\n"
        "  initial clk = 0;\n"
        "  always #5 clk = ~clk;\n"
        "  wire [7:0] sig_0;\n"
        "  assign sig_0 = clk;\n"
        "endmodule\n";
    qsim_session_t *s3 = qsim_session_create();
    printf("Fixed perf: %s\n", qsim_session_compile_string(s3, "t.v", src3) ? "OK" : "FAIL");
    qsim_session_free(s3);

    /* Test 4: Minimal counter design */
    {
        const char *src =
            "module perf_sweep(input clk, input rst);\n"
            "  reg [7:0] c0;\n"
            "  always @(posedge clk or posedge rst)\n"
            "    if (rst) c0 <= 8'd0; else c0 <= c0 + 8'd1;\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        printf("minimal counter: %s\n", qsim_session_compile_string(s, "sweep.v", src) ? "OK" : "FAIL");
        if (qsim_session_compile_string(s, "sweep.v", src)) {
            printf("  elab: %s\n", qsim_session_elaborate(s) ? "OK" : "FAIL");
        }
        qsim_session_free(s);
    }

    /* Test 5: counter with many declarations */
    {
        char src[65536];
        int pos = snprintf(src, sizeof(src),
            "module perf_sweep(input clk, input rst);\n");
        for (int i = 0; i < 128; i++)
            pos += snprintf(src + pos, sizeof(src) - pos,
                "  reg [7:0] c%d;\n", i);
        for (int i = 0; i < 128; i++)
            pos += snprintf(src + pos, sizeof(src) - pos,
                "  always @(posedge clk or posedge rst)\n"
                "    if (rst) c%d <= 8'd0; else c%d <= c%d + 8'd1;\n", i, i, i);
        pos += snprintf(src + pos, sizeof(src) - pos, "endmodule\n");
        qsim_session_t *s5 = qsim_session_create();
        printf("128 counters compile: %s\n", qsim_session_compile_string(s5, "sweep.v", src) ? "OK" : "FAIL");
        if (qsim_session_compile_string(s5, "sweep.v", src)) {
            printf("128 counters elab: %s\n", qsim_session_elaborate(s5) ? "OK" : "FAIL");
        }
        qsim_session_free(s5);
    }

    return 0;
}
