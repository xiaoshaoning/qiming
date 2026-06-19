/* Test replication and concatenation in Verilog assignments */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/session.h"

int main(void) {
    /* Bug 3: Full-register concatenation/replication */
    const char *src =
        "module top3(input clk, output reg [31:0] tmp_instr);\n"
        "  reg tmp_hw12;\n"
        "  reg [2:0] rd_p;\n"
        "  always @(*) begin\n"
        "    tmp_hw12 = 1'b1;\n"
        "    rd_p = 3'b101;\n"
        "    tmp_instr = {{7{tmp_hw12}}, rd_p, 3'b000, rd_p, 7'b0010011};\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    int ok = qsim_session_compile_string(s, "bug3.v", src);
    if (ok && qsim_session_elaborate(s)) {
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "tmp_instr");
        if (v) {
            int len = (int)strlen(v);
            printf("LSB-first string (%d bits):\n", len);
            for (int i = 0; i < len; i++) printf("%c", v[i]);
            printf("\n\n");
            printf("Bit positions:\n");
            printf("  bits 0-6  (0010011):  ");
            for (int i = 6; i >= 0; i--) printf("%c", v[i]);
            printf("\n");
            printf("  bits 7-9  (rd_p 2):   ");
            for (int i = 9; i >= 7; i--) printf("%c", v[i]);
            printf("\n");
            printf("  bits 10-12 (000):     ");
            for (int i = 12; i >= 10; i--) printf("%c", v[i]);
            printf("\n");
            printf("  bits 13-15 (rd_p 1):  ");
            for (int i = 15; i >= 13; i--) printf("%c", v[i]);
            printf("\n");
            printf("  bits 16-22 ({7{1}}):  ");
            for (int i = 22; i >= 16; i--) printf("%c", v[i]);
            printf("\n");

            uint32_t val = 0;
            for (int b = 0; b < 32 && b < len; b++)
                if (v[b] == '1') val |= (1u << b);
            printf("\n  hex: 0x%08X\n", val);
            free(v);
        }
        qsim_session_free(s);
    }

    /* Bug 1: replication in bit-range LHS with posedge clock */
    printf("\n--- Bug 1: Replication in bit-range LHS @(posedge clk) ---\n");
    {
        const char *src =
            "module bug1(input clk, output reg [31:0] tmp_instr);\n"
            "  reg tmp_hw12;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_hw12 = 1'b1;\n"
            "    tmp_instr[31:25] = {7{tmp_hw12}};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        int ok1 = qsim_session_compile_string(s, "bug1.v", src);
        if (ok1 && qsim_session_elaborate(s)) {
            for (int cyc = 0; cyc < 3; cyc++) {
                qsim_session_force_str(s, "clk", "0");
                qsim_session_step_delta(s);
                qsim_session_force_str(s, "clk", "1");
                qsim_session_step_delta(s);
                char *v = qsim_session_eval_str(s, "tmp_instr");
                if (v) {
                    uint32_t val = 0;
                    for (int b = 0; b < 32 && b < (int)strlen(v); b++)
                        if (v[b] == '1') val |= (1u << b);
                    uint32_t bits_31_25 = (val >> 25) & 0x7F;
                    printf("  cycle %d: tmp_instr=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n",
                           cyc, val, bits_31_25);
                    free(v);
                }
            }
            qsim_session_free(s);
        } else {
            printf("  bug1: FAIL compile\n");
        }
    }

    /* Bug 2: concatenation in bit-range LHS with posedge clock */
    printf("\n--- Bug 2: Concat in bit-range LHS @(posedge clk) ---\n");
    {
        const char *src =
            "module bug2(input clk, output reg [31:0] tmp_instr);\n"
            "  reg [4:0] tmp_hword_6_2;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_hword_6_2 = 5'b11011;\n"
            "    tmp_instr[25:20] = {1'b1, tmp_hword_6_2};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        int ok2 = qsim_session_compile_string(s, "bug2.v", src);
        if (ok2 && qsim_session_elaborate(s)) {
            for (int cyc = 0; cyc < 3; cyc++) {
                qsim_session_force_str(s, "clk", "0");
                qsim_session_step_delta(s);
                qsim_session_force_str(s, "clk", "1");
                qsim_session_step_delta(s);
                char *v = qsim_session_eval_str(s, "tmp_instr");
                if (v) {
                    uint32_t val = 0;
                    for (int b = 0; b < 32 && b < (int)strlen(v); b++)
                        if (v[b] == '1') val |= (1u << b);
                    uint32_t bits_25_20 = (val >> 20) & 0x3F;
                    printf("  cycle %d: tmp_instr=0x%08X bits[25:20]=0x%02X (expect 0x3B)\n",
                           cyc, val, bits_25_20);
                    free(v);
                }
            }
            qsim_session_free(s);
        } else {
            printf("  bug2: FAIL compile\n");
        }
    }

    /* Bug 3: full-register complex concat/replication with posedge clock */
    printf("\n--- Bug 3: Full-register complex @(posedge clk) ---\n");
    {
        const char *src =
            "module bug3(input clk, output reg [31:0] tmp_instr);\n"
            "  reg tmp_hw12;\n"
            "  reg [2:0] rd_p;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_hw12 = 1'b1;\n"
            "    rd_p = 3'b101;\n"
            "    tmp_instr = {{7{tmp_hw12}}, rd_p, 3'b000, rd_p, 7'b0010011};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        int ok3 = qsim_session_compile_string(s, "bug3.v", src);
        if (ok3 && qsim_session_elaborate(s)) {
            for (int cyc = 0; cyc < 3; cyc++) {
                qsim_session_force_str(s, "clk", "0");
                qsim_session_step_delta(s);
                qsim_session_force_str(s, "clk", "1");
                qsim_session_step_delta(s);
                char *v = qsim_session_eval_str(s, "tmp_instr");
                if (v) {
                    uint32_t val = 0;
                    for (int b = 0; b < 32 && b < (int)strlen(v); b++)
                        if (v[b] == '1') val |= (1u << b);
                    printf("  cycle %d: tmp_instr = 0x%08X (expect 0x007FA293)\n", cyc, val);
                    free(v);
                }
            }
            qsim_session_free(s);
        } else {
            printf("  bug3: FAIL compile\n");
        }
    }

    printf("DONE\n");
    return 0;
}
