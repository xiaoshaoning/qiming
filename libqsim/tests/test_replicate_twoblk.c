/* Two-always-block test: C-extension decoder (blocking) + pipeline (NBA).
 * Tests whether cross-process interaction causes replication to fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libqsim/session.h"

static uint32_t val_from_str(const char *v) {
    if (!v) return 0xFFFFFFFF;
    uint32_t val = 0;
    int len = (int)strlen(v);
    for (int b = 0; b < 32 && b < len; b++)
        if (v[b] == '1') val |= (1u << b);
    return val;
}

static void set_p(qsim_session_t *s, const char *sig, const char *val) {
    qsim_session_set_str(s, sig, val);
}

static void deltas(qsim_session_t *s) {
    for (int i = 0; i < 10; i++) if (!qsim_session_step_delta(s)) break;
}

static void cycle(qsim_session_t *s) {
    set_p(s, "clk", "0"); deltas(s);
    set_p(s, "clk", "1"); deltas(s);
}

static void reset(qsim_session_t *s) {
    set_p(s, "clk", "0"); deltas(s);
    set_p(s, "rst", "0"); deltas(s);
    set_p(s, "rst", "1"); deltas(s);
    set_p(s, "rst", "0"); deltas(s);
}

static int test_two_blocks(void) {
    printf("--- Two always blocks: decoder (blocking) + pipeline (NBA) ---\n");
    /* Two always blocks at posedge clk:
     * Block 1: Decoder using blocking assigns, includes {7{tmp_hw12}} replication.
     * Block 2: Pipeline NBA, captures Block 1 output, increments PC.
     *
     * Pack two 16-bit C.ADDI instructions per 32-bit word:
     *   imem[0] = 0x15ED156D
     *     bytes 0-1: 0x156D = C.ADDI x10, -5
     *     bytes 2-3: 0x15ED = C.ADDI x11, -5
     *   imem[1..] filled with NOPs (32'h00000013)
     */
    const char *src =
        "module top(input clk, input rst, output reg [31:0] ifid_instr);\n"
        "  reg [31:0] imem [0:1023];\n"
        "  reg [63:0] pc_reg;\n"
        "  reg [31:0] tmp_imem_word;\n"
        "  reg [15:0] tmp_hword;\n"
        "  reg tmp_hw12;\n"
        "  reg tmp_is_compressed;\n"
        "  reg [31:0] tmp_instr;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    for (i = 0; i < 1024; i = i + 1) imem[i] = 32'h00000013;\n"
        /* imem[0] = {C.ADDI x11,-5, C.ADDI x10,-5} */
        "    imem[0] = 32'h15ED156D;\n"
        "    pc_reg = 0;\n"
        "  end\n"
        /* Block 1: C-extension decoder (blocking assigns) */
        "  always @(posedge clk) begin\n"
        "    tmp_imem_word = imem[(pc_reg >> 2) & 10'h3FF];\n"
        "    tmp_hword = pc_reg[1] ? tmp_imem_word[31:16] : tmp_imem_word[15:0];\n"
        "    tmp_is_compressed = (tmp_hword[1:0] != 2'b11);\n"
        "    if (tmp_is_compressed) begin\n"
        "      tmp_hw12 = tmp_hword[12];\n"
        "      tmp_instr = 32'h00000013;\n"
        "      if ((tmp_hword[1:0]==2'b01) && (tmp_hword[15:13]==3'b000)) begin\n"
        "        tmp_instr[6:0] = 7'b0010011;\n"
        "        tmp_instr[11:7] = tmp_hword[11:7];\n"
        "        tmp_instr[14:12] = 3'b000;\n"
        "        tmp_instr[19:15] = tmp_hword[11:7];\n"
        "        tmp_instr[31:25] = {7{tmp_hw12}};  /* REPLICATION */\n"
        "        tmp_instr[24:20] = tmp_hword[6:2];\n"
        "      end\n"
        "    end else begin\n"
        "      tmp_instr = tmp_imem_word;\n"
        "    end\n"
        "  end\n"
        /* Block 2: Pipeline register (NBA) */
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin\n"
        "      ifid_instr <= 32'h00000000;\n"
        "      pc_reg <= 0;\n"
        "    end else begin\n"
        "      ifid_instr <= tmp_instr;\n"
        "      pc_reg <= pc_reg + (tmp_is_compressed ? 64'd2 : 64'd4);\n"
        "    end\n"
        "  end\n"
        "endmodule\n";

    qsim_session_t *s = qsim_session_create();
    if (!s) { printf("  FAIL: session\n"); return 1; }
    int ok = qsim_session_compile_string(s, "twoblk.v", src);
    if (!ok) { printf("  FAIL: compile\n"); return 1; }
    ok = qsim_session_elaborate(s);
    if (!ok) { printf("  FAIL: elaborate\n"); return 1; }

    /* Run initial block */
    qsim_session_step_delta(s);

    /* Reset */
    qsim_session_set_str(s, "rst", "1");
    qsim_session_set_str(s, "clk", "0");
    qsim_session_step_delta(s);
    qsim_session_set_str(s, "rst", "0");
    qsim_session_step_delta(s);

    printf("  After reset:\n");
    char *v = qsim_session_eval_str(s, "pc_reg");
    printf("    pc_reg = 0x%08llX\n", (unsigned long long)val_from_str(v));
    free(v);

    int pass = 1;

    /* Block 1 (blocking) updates tmp_instr BEFORE Block 2 (NBA) reads it,
     * so decoded instruction appears in ifid_instr on the SAME posedge. */
    /* imem[0] lower half: C.ADDI x10, -5 -> ADDI x10, x10, -5 -> 0xFFB50513 */
    /* imem[0] upper half: C.ADDI x11, -5 -> ADDI x11, x11, -5 -> 0xFFB58593 */
    /* imem[1] (NOP): 32'h00000013 -> ADDI x0, x0, 0 -> 0x00000013 */
    uint32_t expected[] = {
        0xFFB50513,  /* cycle 0: C.ADDI x10, -5 decoded from imem[0][15:0] */
        0xFFB58593,  /* cycle 1: C.ADDI x11, -5 decoded from imem[0][31:16] */
        0x00000013,  /* cycle 2: NOP from imem[1] */
        0x00000013,  /* cycle 3: NOP from imem[2] */
    };
    int num_cycles = sizeof(expected) / sizeof(expected[0]);

    for (int cyc = 0; cyc < num_cycles; cyc++) {
        /* Cycle clock: clk 0->1 posedge */
        qsim_session_set_str(s, "clk", "0");
        qsim_session_step_delta(s);
        qsim_session_set_str(s, "clk", "1");
        qsim_session_step_delta(s);

        v = qsim_session_eval_str(s, "ifid_instr");
        uint32_t instr_val = val_from_str(v);
        uint32_t bits_31_25 = (instr_val >> 25) & 0x7F;
        uint32_t exp_bits = (expected[cyc] >> 25) & 0x7F;
        int ok_cyc = (instr_val == expected[cyc]);
        printf("  cycle %d: ifid_instr=0x%08X bits[31:25]=0x%02X (expect 0x%08X/%02X)%s\n",
               cyc, instr_val, bits_31_25, expected[cyc], exp_bits,
               ok_cyc ? "" : " *** FAIL");
        if (!ok_cyc) pass = 0;
        free(v);
    }

    qsim_session_free(s);
    return pass ? 0 : 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fails = 0;
    fails += test_two_blocks();
    printf("\n%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails;
}
