/* Focused: does {7{tmp_hw12}} work when tmp_hw12 = tmp_hword[12] with bit 12 = 1? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/session.h"

int main(void) {
    /* Ultra simple: tmp_hw12 assigned directly = 1 (known working) */
    {
        printf("--- Direct assign: tmp_hw12 = 1 ---\n");
        const char *src =
            "module t(input clk, output reg [31:0] out);\n"
            "  reg tmp_hw12;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_hw12 = 1'b1;\n"
            "    out[31:25] = {7{tmp_hw12}};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        qsim_session_compile_string(s, "t.v", src);
        qsim_session_elaborate(s);
        qsim_session_force_str(s, "clk", "1");
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "out");
        uint32_t val = 0;
        for (int b = 0; b < 32; b++) if (v[b] == '1') val |= (1u << b);
        printf("  out=0x%08X bits[31:25]=0x%02X\n", val, (val>>25)&0x7F);
        qsim_session_free(s);
    }

    /* Via hword part-select: tmp_hw12 = tmp_hword[12] with 0x1000 (=bit12=1) */
    {
        printf("\n--- Via hword: tmp_hw12 = tmp_hword[12], tmp_hword=0x1000 ---\n");
        const char *src =
            "module t2(input clk, output reg [31:0] out);\n"
            "  reg [31:0] tmp_imem_word;\n"
            "  reg [15:0] tmp_hword;\n"
            "  reg tmp_hw12;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_imem_word = 32'h00001000;\n"
            "    tmp_hword = tmp_imem_word[15:0];\n"
            "    tmp_hw12 = tmp_hword[12];\n"
            "    out[31:25] = {7{tmp_hw12}};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        qsim_session_compile_string(s, "t2.v", src);
        qsim_session_elaborate(s);
        qsim_session_force_str(s, "clk", "1");
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "out");
        if (!v) { printf("  FAIL: eval_str NULL\n"); return 1; }
        uint32_t val = 0;
        for (int b = 0; b < 32; b++) if (v[b] == '1') val |= (1u << b);
        uint32_t bits_31_25 = (val >> 25) & 0x7F;
        printf("  out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n", val, bits_31_25);
        qsim_session_free(s);
        if (bits_31_25 != 0x7F) {
            printf("  *** BUG CONFIRMED: replication failed with intermediate assigns\n");
        }
    }

    /* Alternative: bit-select directly from tmp_imem_word */
    {
        printf("\n--- Via direct bit-select from tmp_imem_word ---\n");
        const char *src =
            "module t3(input clk, output reg [31:0] out);\n"
            "  reg [31:0] tmp_imem_word;\n"
            "  reg tmp_hw12;\n"
            "  always @(posedge clk) begin\n"
            "    tmp_imem_word = 32'h00001000;\n"
            "    tmp_hw12 = tmp_imem_word[12];\n"
            "    out[31:25] = {7{tmp_hw12}};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        qsim_session_compile_string(s, "t3.v", src);
        qsim_session_elaborate(s);
        qsim_session_force_str(s, "clk", "1");
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "out");
        if (!v) { printf("  FAIL: eval_str NULL\n"); return 1; }
        uint32_t val = 0;
        for (int b = 0; b < 32; b++) if (v[b] == '1') val |= (1u << b);
        uint32_t bits_31_25 = (val >> 25) & 0x7F;
        printf("  out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n", val, bits_31_25);
        qsim_session_free(s);
        if (bits_31_25 != 0x7F) {
            printf("  *** BUG CONFIRMED\n");
        }
    }

    /* Via combinational chain + posedge */
    {
        printf("\n--- Combinational chain feeding posedge clk ---\n");
        const char *src =
            "module t4(input clk, output reg [31:0] out);\n"
            "  reg [31:0] tmp_imem_word;\n"
            "  reg [15:0] tmp_hword;\n"
            "  reg tmp_hw12;\n"
            "  always @(*) tmp_imem_word = 32'h00001000;\n"
            "  always @(*) tmp_hword = tmp_imem_word[15:0];\n"
            "  always @(*) tmp_hw12 = tmp_hword[12];\n"
            "  always @(posedge clk) begin\n"
            "    out[31:25] = {7{tmp_hw12}};\n"
            "  end\n"
            "endmodule\n";
        qsim_session_t *s = qsim_session_create();
        qsim_session_compile_string(s, "t4.v", src);
        qsim_session_elaborate(s);
        qsim_session_step_delta(s); /* let combinational logic settle */
        qsim_session_force_str(s, "clk", "1");
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "out");
        if (!v) { printf("  FAIL: eval_str NULL\n"); return 1; }
        uint32_t val = 0;
        for (int b = 0; b < 32; b++) if (v[b] == '1') val |= (1u << b);
        uint32_t bits_31_25 = (val >> 25) & 0x7F;
        printf("  out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n", val, bits_31_25);
        qsim_session_free(s);
        if (bits_31_25 != 0x7F) {
            printf("  *** BUG CONFIRMED\n");
        }
    }

    printf("\nDONE\n");
    return 0;
}
