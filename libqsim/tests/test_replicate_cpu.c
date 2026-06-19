/* Reproduction test that exactly mimics the CPU decoder pattern */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/session.h"

static int val_from_str(const char *v) {
    if (!v) return -1;
    uint32_t val = 0;
    int len = (int)strlen(v);
    for (int b = 0; b < 32 && b < len; b++)
        if (v[b] == '1') val |= (1u << b);
    return (int)val;
}

/* Test CPU decoder pattern EXACTLY as in riscv_srv.v */
static int test_cpu_decoder(void) {
    printf("--- CPU decoder pattern: imem[addr] -> mux -> bit-select -> {7{}} ---\n");
    const char *src =
        "module top(input clk, output reg [31:0] tmp_instr);\n"
        "  reg [31:0] imem [0:1023];\n"
        "  reg [63:0] pc_reg;\n"
        "  reg [31:0] tmp_imem_word;\n"
        "  reg [15:0] tmp_hword;\n"
        "  reg tmp_hw12;\n"
        "  reg [4:0] tmp_hword_6_2;\n"
        "  reg tmp_is_compressed;\n"
        "  reg [4:0] rd_p;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    for (i = 0; i < 1024; i = i + 1) imem[i] = 32'h00000013;\n"
        /* imem[0] has a compressed instruction: C.ADDI x10, -5 */
        /* C.ADDI encoding: CI-type {op[1:0]=01, funct3=000, imm[5], rd, imm[4:0]} */
        /* op=01, funct3=000, imm[5]=1, rd=x10=01010, imm[4:0]=11011 */
        /* = 000_1_01010_11011_01 = 0x156D */
        /* bits[1:0]=01 (C-extension), bit12=1 (neg imm) */
        "    imem[0] = 32'h0000156D;\n"
        "    pc_reg = 0;\n"
        "  end\n"
        /* C-extension decoder as in riscv_srv.v */
        "  always @(posedge clk) begin\n"
        "    tmp_imem_word = imem[(pc_reg >> 2) & 10'h3FF];\n"
        "    tmp_hword = pc_reg[1] ? tmp_imem_word[31:16] : tmp_imem_word[15:0];\n"
        "    tmp_is_compressed = (tmp_hword[1:0] != 2'b11);\n"
        "    if (tmp_is_compressed) begin\n"
        "      rd_p  = {3'b001, tmp_hword[4:2]};\n"
        "      tmp_hw12 = tmp_hword[12];\n"
        "      tmp_hword_6_2 = tmp_hword[6:2];\n"
        "      tmp_instr = 32'h00000013;\n"
        "      if ((tmp_hword[1:0]==2'b01) && (tmp_hword[15:13]==3'b000)) begin\n"
        /* C.ADDI */
        "        tmp_instr[6:0] = 7'b0010011;\n"
        "        tmp_instr[11:7] = tmp_hword[11:7];\n"
        "        tmp_instr[14:12] = 3'b000;\n"
        "        tmp_instr[19:15] = tmp_hword[11:7];\n"
        "        tmp_instr[31:25] = {7{tmp_hw12}};\n"  /* THE BUG: replication */
        "        tmp_instr[24:20] = tmp_hword_6_2;\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: session\n"); return 1; }
    int ok = qsim_session_compile_string(s, "cpu_dec.v", src);
    if (!ok) { printf("  FAIL: compile\n"); return 1; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate\n"); return 1; }

    /* Run initial block (sets imem, pc_reg) */
    qsim_session_step_delta(s);

    /* Check: imem[0] should be 0x455B (C.ADDI x10, -5) */
    char *imem0 = qsim_session_eval_str(s, "imem");
    if (imem0) {
        /* imem is 1024*32 bits, too large. Check pc_reg */
        free(imem0);
    }
    char *pc_v = qsim_session_eval_str(s, "pc_reg");
    printf("  pc_reg initial: 0x%08X\n", val_from_str(pc_v));
    free(pc_v);

    /* Cycle 1: posedge clk */
    qsim_session_force_str(s, "clk", "1");
    qsim_session_step_delta(s);

    /* Check intermediate signals */
    char *v;

    v = qsim_session_eval_str(s, "tmp_imem_word");
    printf("  tmp_imem_word: 0x%08X\n", val_from_str(v));
    free(v);

    v = qsim_session_eval_str(s, "tmp_hword");
    printf("  tmp_hword: 0x%04X\n", val_from_str(v));
    free(v);

    v = qsim_session_eval_str(s, "tmp_hw12");
    printf("  tmp_hw12: %d\n", val_from_str(v));
    free(v);

    v = qsim_session_eval_str(s, "tmp_hword_6_2");
    printf("  tmp_hword_6_2: 0x%02X\n", val_from_str(v));
    free(v);

    v = qsim_session_eval_str(s, "rd_p");
    printf("  rd_p: 0x%02X\n", val_from_str(v));
    free(v);

    v = qsim_session_eval_str(s, "tmp_instr");
    int tv = val_from_str(v);
    printf("  tmp_instr: 0x%08X", tv);
    /* For C.ADDI x10, -5 with imm[5]=1, expected:
       bits[31:25] = {7{1}} = 0x7F
       bits[24:20] = 11011 = 0x1B
       bits[19:15] = x10 = 01010
       bits[14:12] = 000
       bits[11:7] = x10 = 01010
       bits[6:0] = 0010011
       = 1111111_11011_01010_000_01010_0010011
       = 0xFB50513 */
    printf(" (expect 0xFFB50513)\n");
    free(v);

    int pass = (tv == 0xFFB50513);
    if (!pass) {
        int bits_31_25 = (tv >> 25) & 0x7F;
        int bits_24_20 = (tv >> 20) & 0x1F;
        int bits_19_15 = (tv >> 15) & 0x1F;
        int bits_11_7 = (tv >> 7) & 0x1F;
        printf("  *** FAIL: bits[31:25]=0x%02X bits[24:20]=0x%02X\n",
               bits_31_25, bits_24_20);
    }

    qsim_session_free(s);
    return pass ? 0 : 1;
}

/* Test: does replication work inside large always blocks with
   many assignments before it? */
static int test_large_block(void) {
    printf("\n--- Large always block with many assignments before repl ---\n");
    /* Build a large always block with many dummy assignments */
    char src[4096];
    int pos = snprintf(src, sizeof(src),
        "module top(input clk, output reg [31:0] out);\n"
        "  reg [31:0] arr [0:127];\n"
        "  reg [31:0] tmp_a;\n"
        "  reg [15:0] tmp_b;\n"
        "  reg tmp_c;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    for (i = 0; i < 128; i = i + 1) arr[i] = 32'h00001000;\n"
        "  end\n"
        "  always @(posedge clk) begin\n"
        "    tmp_a = arr[0];\n");
    /* Add 50 dummy assignments */
    for (int i = 0; i < 50; i++) {
        pos += snprintf(src + pos, sizeof(src) - pos,
            "    tmp_b = tmp_a[%d:0];\n", (i % 16) + 15);
    }
    pos += snprintf(src + pos, sizeof(src) - pos,
        "    tmp_c = tmp_b[12];\n"
        "    out[31:25] = {7{tmp_c}};\n"
        "  end\n"
        "endmodule\n");

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: session\n"); return 1; }
    int ok = qsim_session_compile_string(s, "large.v", src);
    if (!ok) { printf("  FAIL: compile\n"); return 1; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate\n"); return 1; }
    qsim_session_step_delta(s);

    qsim_session_force_str(s, "clk", "1");
    qsim_session_step_delta(s);
    char *v = qsim_session_eval_str(s, "out");
    if (!v) { printf("  FAIL: eval_str NULL\n"); return 1; }
    uint32_t val = 0;
    for (int b = 0; b < 32 && b < (int)strlen(v); b++)
        if (v[b] == '1') val |= (1u << b);
    uint32_t bits_31_25 = (val >> 25) & 0x7F;
    printf("  out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n", val, bits_31_25);
    qsim_session_free(s);
    return (bits_31_25 == 0x7F) ? 0 : 1;
}

/* Test: array read inside always @(posedge clk) */
static int test_array_read(void) {
    printf("\n--- Array read inside @(posedge clk) -> bit-select -> {7{}} ---\n");
    const char *src =
        "module top(input clk, output reg [31:0] out);\n"
        "  reg [31:0] mem [0:15];\n"
        "  reg [3:0] addr;\n"
        "  reg [31:0] tmp_word;\n"
        "  reg tmp_bit;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    for (i = 0; i < 16; i = i + 1) mem[i] = 32'h00001000;\n"
        "    addr = 0;\n"
        "  end\n"
        "  always @(posedge clk) begin\n"
        "    tmp_word = mem[addr];\n"
        "    tmp_bit = tmp_word[12];\n"
        "    out[31:25] = {7{tmp_bit}};\n"
        "    addr = addr + 1;\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    int ok = qsim_session_compile_string(s, "arr.v", src);
    if (!ok) { printf("  FAIL: compile\n"); return 1; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate\n"); return 1; }
    qsim_session_step_delta(s);

    int pass = 1;
    for (int cyc = 0; cyc < 5; cyc++) {
        qsim_session_force_str(s, "clk", "1");
        qsim_session_step_delta(s);
        char *v = qsim_session_eval_str(s, "out");
        if (!v) { printf("  FAIL: eval_str NULL cyc %d\n", cyc); return 1; }
        uint32_t val = 0;
        for (int b = 0; b < 32 && b < (int)strlen(v); b++)
            if (v[b] == '1') val |= (1u << b);
        uint32_t bits_31_25 = (val >> 25) & 0x7F;
        printf("  cycle %d: out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n",
               cyc, val, bits_31_25);
        if (bits_31_25 != 0x7F) {
            printf("  *** FAIL: replication produced wrong value\n");
            pass = 0;
        }
        free(v);
    }
    qsim_session_free(s);
    return pass ? 0 : 1;
}

/* Test: ternary expression chain */
static int test_ternary_chain(void) {
    printf("\n--- Ternary mux -> bit-select -> {7{}} @(posedge clk) ---\n");
    const char *src =
        "module top(input clk, output reg [31:0] out);\n"
        "  reg [31:0] tmp_word;\n"
        "  reg [15:0] tmp_hword;\n"
        "  reg tmp_bit;\n"
        "  always @(posedge clk) begin\n"
        "    tmp_word = 32'h00001000;\n"
        "    tmp_hword = (tmp_word[1]) ? tmp_word[31:16] : tmp_word[15:0];\n"
        "    tmp_bit = tmp_hword[12];\n"
        "    out[31:25] = {7{tmp_bit}};\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    int ok = qsim_session_compile_string(s, "tern.v", src);
    if (!ok) { printf("  FAIL: compile\n"); return 1; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate\n"); return 1; }

    qsim_session_force_str(s, "clk", "1");
    qsim_session_step_delta(s);
    char *v = qsim_session_eval_str(s, "out");
    if (!v) { printf("  FAIL: eval_str NULL\n"); return 1; }
    uint32_t val = 0;
    for (int b = 0; b < 32 && b < (int)strlen(v); b++)
        if (v[b] == '1') val |= (1u << b);
    uint32_t bits_31_25 = (val >> 25) & 0x7F;
    printf("  out=0x%08X bits[31:25]=0x%02X (expect 0x7F)\n", val, bits_31_25);
    qsim_session_free(s);
    return (bits_31_25 == 0x7F) ? 0 : 1;
}

int main(void) {
    int fails = 0;
    fails += test_cpu_decoder();
    fails += test_large_block();
    fails += test_array_read();
    fails += test_ternary_chain();
    printf("\n%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails;
}
