/* RV32I CPU simulation tests -- single-cycle 32-bit RISC-V via session API. */
#include "minunit.h"
#include "libqsim/session.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ================================================================
 * RV32I instruction encoding helpers
 * ================================================================ */

/* R-type: ADD/SUB/SLL/SLT/SLTU/XOR/SRL/SRA/OR/AND */
#define RV_R_TYPE(funct7, funct3, rd, rs1, rs2) \
    (((funct7) << 25) | ((rs2) << 20) | ((rs1) << 15) | ((funct3) << 12) | ((rd) << 7) | 0x33)
#define RV_ADD(rd, rs1, rs2)  RV_R_TYPE(0x00, 0, rd, rs1, rs2)
#define RV_SUB(rd, rs1, rs2)  RV_R_TYPE(0x20, 0, rd, rs1, rs2)
#define RV_SLL(rd, rs1, rs2)  RV_R_TYPE(0x00, 1, rd, rs1, rs2)
#define RV_SLT(rd, rs1, rs2)  RV_R_TYPE(0x00, 2, rd, rs1, rs2)
#define RV_SLTU(rd, rs1, rs2) RV_R_TYPE(0x00, 3, rd, rs1, rs2)
#define RV_XOR(rd, rs1, rs2)  RV_R_TYPE(0x00, 4, rd, rs1, rs2)
#define RV_SRL(rd, rs1, rs2)  RV_R_TYPE(0x00, 5, rd, rs1, rs2)
#define RV_SRA(rd, rs1, rs2)  RV_R_TYPE(0x20, 5, rd, rs1, rs2)
#define RV_OR(rd, rs1, rs2)   RV_R_TYPE(0x00, 6, rd, rs1, rs2)
#define RV_AND(rd, rs1, rs2)  RV_R_TYPE(0x00, 7, rd, rs1, rs2)

/* I-type: ADDI/SLLI/SLTI/SLTIU/XORI/SRLI/SRAI/ORI/ANDI */
#define RV_I_TYPE(funct3, rd, rs1, imm12) \
    ((((imm12) & 0xFFF) << 20) | ((rs1) << 15) | ((funct3) << 12) | ((rd) << 7) | 0x13)
#define RV_ADDI(rd, rs1, imm12)  RV_I_TYPE(0, rd, rs1, imm12)
#define RV_SLLI(rd, rs1, imm12)  RV_I_TYPE(1, rd, rs1, (imm12) & 0x1F)
#define RV_SLTI(rd, rs1, imm12)  RV_I_TYPE(2, rd, rs1, imm12)
#define RV_SLTIU(rd, rs1, imm12) RV_I_TYPE(3, rd, rs1, imm12)
#define RV_XORI(rd, rs1, imm12)  RV_I_TYPE(4, rd, rs1, imm12)
/* SRLI/SRAI: imm12 bit 10 = 1 for SRAI */
#define RV_SRLI(rd, rs1, shamt)  RV_I_TYPE(5, rd, rs1, (shamt) & 0x1F)
#define RV_SRAI(rd, rs1, shamt)  RV_I_TYPE(5, rd, rs1, 0x400 | ((shamt) & 0x1F))
#define RV_ORI(rd, rs1, imm12)   RV_I_TYPE(6, rd, rs1, imm12)
#define RV_ANDI(rd, rs1, imm12)  RV_I_TYPE(7, rd, rs1, imm12)

/* Load I-type: LB/LH/LW/LBU/LHU */
#define RV_LOAD(funct3, rd, rs1, imm12) \
    ((((imm12) & 0xFFF) << 20) | ((rs1) << 15) | ((funct3) << 12) | ((rd) << 7) | 0x03)
#define RV_LW(rd, rs1, imm12)  RV_LOAD(2, rd, rs1, imm12)

/* Store S-type: SB/SH/SW */
#define RV_STORE(funct3, rs2, rs1, imm12) \
    (((((imm12) >> 5) & 0x7F) << 25) | ((rs2) << 20) | ((rs1) << 15) | ((funct3) << 12) | (((imm12) & 0x1F) << 7) | 0x23)
#define RV_SW(rs2, rs1, imm12) RV_STORE(2, rs2, rs1, imm12)

/* B-type: BEQ/BNE/BLT/BGE/BLTU/BGEU offset is byte offset from PC */
static uint32_t enc_btype(uint32_t funct3, uint32_t rs1, uint32_t rs2, int32_t offset) {
    uint32_t imm13 = offset & 0x1FFF;
    uint32_t bit12 = (imm13 >> 12) & 1;
    uint32_t bit11 = (imm13 >> 11) & 1;
    uint32_t bits_10_5 = (imm13 >> 5) & 0x3F;
    uint32_t bits_4_1 = (imm13 >> 1) & 0xF;
    return (bit12 << 31) | (bits_10_5 << 25) | (rs2 << 20) | (rs1 << 15)
         | (funct3 << 12) | (bits_4_1 << 8) | (bit11 << 7) | 0x63;
}
#define RV_BEQ(rs1, rs2, offset)  enc_btype(0, rs1, rs2, offset)
#define RV_BNE(rs1, rs2, offset)  enc_btype(1, rs1, rs2, offset)
#define RV_BLT(rs1, rs2, offset)  enc_btype(4, rs1, rs2, offset)
#define RV_BGE(rs1, rs2, offset)  enc_btype(5, rs1, rs2, offset)
#define RV_BLTU(rs1, rs2, offset) enc_btype(6, rs1, rs2, offset)
#define RV_BGEU(rs1, rs2, offset) enc_btype(7, rs1, rs2, offset)

/* J-type: JAL offset is byte offset from PC */
static uint32_t enc_jal(uint32_t rd, int32_t offset) {
    uint32_t imm21 = offset & 0x1FFFFF;
    uint32_t bit20 = (imm21 >> 20) & 1;
    uint32_t bits_19_12 = (imm21 >> 12) & 0xFF;
    uint32_t bit11 = (imm21 >> 11) & 1;
    uint32_t bits_10_1 = (imm21 >> 1) & 0x3FF;
    return (bit20 << 31) | (bits_10_1 << 21) | (bit11 << 20) | (bits_19_12 << 12) | ((rd) << 7) | 0x6F;
}
#define RV_JAL(rd, offset) enc_jal(rd, offset)

/* JALR */
#define RV_JALR(rd, rs1, imm12) \
    ((((imm12) & 0xFFF) << 20) | ((rs1) << 15) | (0 << 12) | ((rd) << 7) | 0x67)

/* EBREAK */
#define RV_EBREAK 0x00100073

/* ECALL */
#define RV_ECALL 0x00000073

/* MRET */
#define RV_MRET 0x30200073

/* CSR instructions: CSRRW, CSRRS, CSRRC (register variants) */
#define RV_CSRRW(rd, rs1, csr)    (((csr)&0xFFF)<<20 | ((rs1)&0x1F)<<15 | 0x001<<12 | ((rd)&0x1F)<<7 | 0x73)
#define RV_CSRRS(rd, rs1, csr)    (((csr)&0xFFF)<<20 | ((rs1)&0x1F)<<15 | 0x002<<12 | ((rd)&0x1F)<<7 | 0x73)
#define RV_CSRRC(rd, rs1, csr)    (((csr)&0xFFF)<<20 | ((rs1)&0x1F)<<15 | 0x003<<12 | ((rd)&0x1F)<<7 | 0x73)

/* CSR immediate variants: CSRRWI, CSRRSI, CSRRCI */
#define RV_CSRRWI(rd, uimm, csr)  (((csr)&0xFFF)<<20 | ((uimm)&0x1F)<<15 | 0x005<<12 | ((rd)&0x1F)<<7 | 0x73)
#define RV_CSRRSI(rd, uimm, csr)  (((csr)&0xFFF)<<20 | ((uimm)&0x1F)<<15 | 0x006<<12 | ((rd)&0x1F)<<7 | 0x73)
#define RV_CSRRCI(rd, uimm, csr)  (((csr)&0xFFF)<<20 | ((uimm)&0x1F)<<15 | 0x007<<12 | ((rd)&0x1F)<<7 | 0x73)

/* Timer peripheral address */
#define TIMER_COMPARE_ADDR 0x20000000

/* LUI / AUIPC */
#define RV_LUI(rd, imm20)   ((((imm20) & 0xFFFFF) << 12) | ((rd) << 7) | 0x37)
#define RV_AUIPC(rd, imm20) ((((imm20) & 0xFFFFF) << 12) | ((rd) << 7) | 0x17)

/* ================================================================
 * Shared helpers
 * ================================================================ */

/* Read rv32i_top.v source (search multiple relative paths). Returns NULL on failure. */
static char *read_rv32i_verilog(size_t *out_len) {
    const char *paths[] = {"../../example/rv32i/rv32i_top.v",
                           "../../../example/rv32i/rv32i_top.v",
                           "example/rv32i/rv32i_top.v",
                           "../example/rv32i/rv32i_top.v",
                           "../libqsim/../example/rv32i/rv32i_top.v"};
    FILE *f = NULL;
    for (int i = 0; i < 5; i++) {
        f = fopen(paths[i], "rb");
        if (f) break;
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)len + 1);
    if (!src) { fclose(f); return NULL; }
    size_t nread = fread(src, 1, (size_t)len, f);
    src[nread] = '\0';
    fclose(f);
    *out_len = (size_t)nread;
    return src;
}

#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

/* Build IMEM bit string (LSB-first, 32768 bits = 1024 words x 32 bits).
 * Unused slots filled with EBREAK. */
static void build_imem_str32(char *out, const uint32_t *program, size_t count) {
    for (int i = 0; i < IMEM_WORDS; i++) {
        uint32_t val = ((size_t)i < count) ? program[i] : RV_EBREAK;
        for (int b = 0; b < 32; b++) {
            int pos = IMEM_BITS - 1 - (i * 32 + b);
            out[pos] = ((val >> b) & 1) ? '1' : '0';
        }
    }
    out[IMEM_BITS] = '\0';
}

/* Convert LSB-first 32-char string to uint32_t for debug printing */
static uint32_t lsbfirst_to_u32(const char *s) {
    uint32_t val = 0;
    for (int i = 0; i < 32 && s[i]; i++)
        if (s[i] == '1') val |= (1u << i);
    return val;
}

/* Session-based reset: 0->1 posedge on rst */
static void session_reset(qsim_session_t *sess) {
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "1");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_step_delta(sess);
}

/* Clock cycle: clk=0, delta, clk=1, delta. Returns halted string (caller frees) or NULL. */
static char *session_clock_cycle(qsim_session_t *sess) {
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "1");
    qsim_session_step_delta(sess);
    return qsim_session_eval_str(sess, "halted");
}

/* ================================================================
 * Test: ADDI x10, x0, 42; EBREAK  =>  x10 = 42
 * ================================================================ */
static void test_rv32i_addi(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(10, 0, 42),   // x10 = 42
        RV_EBREAK
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 2);
    /* Print signal list */
    int sig_count = qsim_session_get_signal_count(sess);
    printf("  signal_count=%d\n", sig_count);
    for (int si = 0; si < sig_count && si < 80; si++) {
        const char *sn = qsim_session_get_signal_name(sess, si);
        if (sn) printf("    [%d] %s\n", si, sn);
    }

    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    /* Diagnostic: read imem back */
    char *imem_check = qsim_session_eval_str(sess, "imem");
    if (imem_check) {
        printf("  imem width=%zu\n", strlen(imem_check));
        /* Print all 1024 words as LSB-first strings */
        for (int wi = 0; wi < 1024 && wi < 8; wi++) {
            printf("  imem[%d]: ", wi);
            for (int b = 0; b < 32; b++)
                printf("%c", imem_check[wi*32 + b]);
            printf("\n");
        }
        free(imem_check);
    }
    /* Print the force string first 2 words for reference */
    printf("  force[0]: ");
    for (int i = 0; i < 32; i++) printf("%c", imem_str[IMEM_BITS-32+i]);
    printf("\n  force[1]: ");
    for (int i = 0; i < 32; i++) printf("%c", imem_str[IMEM_BITS-64+i]);
    printf("\n");
    /* Diagnostic: check if_instr */
    char *if_instr_val = qsim_session_eval_str(sess, "if_instr");
    if (if_instr_val) {
        printf("  if_instr after force: %s\n", if_instr_val);
        free(if_instr_val);
    }

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 20; cycle++) {
        char *h = session_clock_cycle(sess);

        /* Read detailed pipeline state */
        char *pc_after   = qsim_session_eval_str(sess, "pc");
        char *x10_after  = qsim_session_eval_str(sess, "x10");
        char *instr_after = qsim_session_eval_str(sess, "instruction");
        char *halted_after = qsim_session_eval_str(sess, "halted");

        /* ID stage (decoded from ifid_instr — current value after NBAs) */
        char *id_rd_s     = qsim_session_eval_str(sess, "id_rd");
        char *id_rs2_s    = qsim_session_eval_str(sess, "id_rs2");

        /* Try reading idex_rd directly (may fail if not a named signal) */
        char *idex_rd_raw = qsim_session_eval_str(sess, "idex_rd");
        char *bubble_raw  = qsim_session_eval_str(sess, "bubble_idex");

        /* ID/EX state via debug outputs */
        char *idex_rd_s   = qsim_session_eval_str(sess, "debug_idex_rd");
        char *idex_rs1_s  = qsim_session_eval_str(sess, "debug_idex_rs1");
        char *idex_rs2_s  = qsim_session_eval_str(sess, "debug_idex_rs2");
        char *idex_pc_s   = qsim_session_eval_str(sess, "debug_idex_pc");
        char *idex_imm_s  = qsim_session_eval_str(sess, "debug_idex_imm");
        char *idex_br     = qsim_session_eval_str(sess, "debug_idex_branch");
        char *idex_f3     = qsim_session_eval_str(sess, "debug_idex_funct3");
        char *idex_fwd1   = qsim_session_eval_str(sess, "debug_forward_rs1_val");
        char *idex_fwd2   = qsim_session_eval_str(sess, "debug_forward_rs2_val");

        /* Control signals */
        char *flush_s     = qsim_session_eval_str(sess, "debug_flush_ifid");
        char *stall_s     = qsim_session_eval_str(sess, "debug_load_use_stall");
        char *br_taken    = qsim_session_eval_str(sess, "debug_branch_taken");
        char *br_target   = qsim_session_eval_str(sess, "debug_ex_branch_target");

        /* IF/ID state */
        char *ifid_instr_s = qsim_session_eval_str(sess, "ifid_instr");
        char *alu_result_s = qsim_session_eval_str(sess, "alu_result");

        /* Print one-line summary */
        printf("  CYC%d:", cycle);
        printf(" pc=%s", pc_after ? pc_after : "?");
        printf(" ifid=%s", instr_after ? instr_after : "?");
        printf(" id_rd=%s id_rs2=%s", id_rd_s ? id_rd_s : "?", id_rs2_s ? id_rs2_s : "?");
        printf(" raw_rd=%s", idex_rd_raw ? idex_rd_raw : "?");
        printf(" bub=%s", bubble_raw ? bubble_raw : "?");
        printf(" fl=%s st=%s", flush_s ? flush_s : "?", stall_s ? stall_s : "?");
        printf(" br=%s tk=%s", idex_br ? idex_br : "?", br_taken ? br_taken : "?");
        printf(" rd=%s rs1=%s rs2=%s", idex_rd_s ? idex_rd_s : "?",
               idex_rs1_s ? idex_rs1_s : "?", idex_rs2_s ? idex_rs2_s : "?");
        printf(" ipc=%s alu=%s", idex_pc_s ? idex_pc_s : "?",
               alu_result_s ? alu_result_s : "?");
        printf(" fw1=%s fw2=%s", idex_fwd1 ? idex_fwd1 : "?", idex_fwd2 ? idex_fwd2 : "?");
        printf(" x10=%s halt=%s", x10_after ? x10_after : "?", halted_after ? halted_after : "?");
        printf("\n");

        /* Free everything */
        free(pc_after); free(x10_after); free(instr_after); free(halted_after);
        free(id_rd_s); free(id_rs2_s);
        free(idex_rd_raw); free(bubble_raw);
        free(idex_rd_s); free(idex_rs1_s); free(idex_rs2_s);
        free(idex_pc_s); free(idex_imm_s); free(idex_br); free(idex_f3);
        free(idex_fwd1); free(idex_fwd2);
        free(flush_s); free(stall_s); free(br_taken);
        free(br_target);
        free(ifid_instr_s); free(alu_result_s);

        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "addi halted");

    /* Diagnostic: check if_instr and signals after execution */
    char *if_instr2 = qsim_session_eval_str(sess, "if_instr");
    if (if_instr2) {
        printf("  if_instr after run: %s\n", if_instr2);
        free(if_instr2);
    }
    char *pc_val = qsim_session_eval_str(sess, "pc");
    if (pc_val) {
        printf("  pc after run: %s\n", pc_val);
        free(pc_val);
    }
    char *instr_val = qsim_session_eval_str(sess, "instruction");
    if (instr_val) {
        printf("  instruction after run: %s\n", instr_val);
        free(instr_val);
    }

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* x10 = 42 = 0x2A = 00101010, LSB-first = "01010100" + 24 zero bits */
    printf("  x10 = %s (expected 01010100000000000000000000000000)\n", x10);
    char *rv = qsim_session_eval_str(sess, "reg_x10");
    if (rv) { printf("  reg_x10 = %s\n", rv); free(rv); }
    rv = qsim_session_eval_str(sess, "ifid_instr");
    if (rv) { printf("  ifid_instr = %s\n", rv); free(rv); }
    rv = qsim_session_eval_str(sess, "id_rd");
    if (rv) { printf("  id_rd = %s\n", rv); free(rv); }
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42");
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Fibonacci(10) = 55
 *   ADDI x10, x0, 0     # a = 0
 *   ADDI x11, x0, 1     # b = 1
 *   ADDI x12, x0, 10    # n = 10
 * loop:
 *   ADD  x13, x10, x11  # t = a + b
 *   ADDI x10, x11, 0    # a = b
 *   ADDI x11, x13, 0    # b = t
 *   ADDI x12, x12, -1   # n--
 *   BNE  x12, x0, loop  # if n != 0, goto loop
 *   EBREAK
 * ================================================================ */
static void test_rv32i_fib10(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    /* Instructions at bytes 0, 4, 8, 12, 16, 20, 24, 28, 32 */
    uint32_t program[] = {
        RV_ADDI(10, 0, 0),    /* 0:  x10 = 0 */
        RV_ADDI(11, 0, 1),    /* 4:  x11 = 1 */
        RV_ADDI(12, 0, 10),   /* 8:  x12 = 10 (count) */
        RV_ADD(13, 10, 11),   /* 12: x13 = x10 + x11 */
        RV_ADDI(10, 11, 0),   /* 16: x10 = x11 */
        RV_ADDI(11, 13, 0),   /* 20: x11 = x13 */
        RV_ADDI(12, 12, -1),  /* 24: x12-- */
        RV_BNE(12, 0, -16),   /* 28: if x12 != 0, goto 12 (loop) */
        RV_EBREAK             /* 32: halt */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 9);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 200; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "fib10 halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* x10 = 55 = 0x37 = 0b00110111, LSB-first = "11101100" + 24 zero bits */
    mu_assert(strcmp(x10, "11101100000000000000000000000000") == 0,
              "x10 = 55");
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Multiply 7 * 6 = 42 via repeated addition
 *   ADDI x11, x0, 7    # x11 = 7
 *   ADDI x12, x0, 6    # x12 = 6
 *   ADDI x10, x0, 0    # x10 = 0 (result)
 * loop:
 *   BEQ  x12, x0, done # if x12 == 0, done
 *   ADD  x10, x10, x11 # x10 += x11
 *   ADDI x12, x12, -1  # x12--
 *   JAL  x0, loop      # goto loop
 * done:
 *   EBREAK
 * ================================================================ */
static void test_rv32i_mul(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    /* Instructions at bytes 0, 4, 8, 12, 16, 20, 24, 28
     * PC=0:  ADDI x11, x0, 7      # x11 = 7
     * PC=4:  ADDI x12, x0, 6      # x12 = 6
     * PC=8:  ADDI x10, x0, 0      # x10 = 0
     * PC=12: BEQ x12, x0, +16     # if x12==0, skip to PC=28
     * PC=16: ADD x10, x10, x11    # x10 += x11
     * PC=20: ADDI x12, x12, -1    # x12--
     * PC=24: JAL x0, -16          # goto PC=12 (JAL offset = 12-28 = -16 from the NEXT instruction at PC=28)
     * PC=28: EBREAK               # halt
     */
    /* Let me recalculate offsets:
     * BEQ at PC=12: target=28. Offset=28-12=16. The B-type offset encoding targets PC + offset, so offset=16.
     * BNE at PC=24: target=12. PC at execution = 24. Offset = 12-24 = -12? Wait, for JAL the return is PC+4.
     *   But for branches too, in a single-cycle CPU the next PC is compared during execution while we're at PC=24.
     *   For BNE, offset = target - PC = 12 - 24 = -12. But wait, in the RISC-V spec, branch offset is relative to the instruction address.
     *
     * Actually, our CPU computes `pc_next = pc_reg + imm_b` for branches, and `pc_next = pc_reg + imm_j` for JAL.
     * At PC=12, if BEQ not taken, pc_next = 16 (pc+4). If taken, pc_next = 12 + imm_b.
     * We want taken to go to PC=28, so imm_b = 28-12 = 16. In B-type encoding, offset=16.
     *
     * At PC=24, JAL x0: return addr = current PC + 4 = 28. Target = 24 + imm_j = 12.
     * So imm_j = 12-24 = -12.
     *
     * Wait, but JAL uses imm_j for the target and writes PC+4 to rd.
     * In our CPU: wb_sel=2'b10 (PC+4 for write-back), pc_sel=2'b10 (pc_reg + imm_j).
     * So the return address = pc_reg + 4 (which is the next instruction after JAL), and target = pc_reg + imm_j.
     * At PC=24: return = 28, target = 24 + (-12) = 12. ✓
     *
     * For BEQ at PC=12: branch_taken ? (pc_reg + imm_b) : pc_plus_4.
     * If taken: target = 12 + 16 = 28. ✓
     */
    uint32_t program[] = {
        RV_ADDI(11, 0, 7),    /* 0 */
        RV_ADDI(12, 0, 6),    /* 4 */
        RV_ADDI(10, 0, 0),    /* 8 */
        RV_BEQ(12, 0, 16),    /* 12: if x12==0 goto 28 */
        RV_ADD(10, 10, 11),   /* 16 */
        RV_ADDI(12, 12, -1),  /* 20 */
        RV_JAL(0, -12),       /* 24: goto 12 */
        RV_EBREAK             /* 28 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 8);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 100; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "mul halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* x10 = 42 = 0x2A = 00101010, LSB-first = "01010100" + 24 zero bits */
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42");
    printf("    mul: x10=%s cycle=%d\n", x10, halted_cycle);
    fflush(stdout);
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Load/store word
 *   ADDI x10, x0, 42    # x10 = 42 (value to store)
 *   ADDI x11, x0, 0     # x11 = 0 (DMEM address offset)
 *   SW   x10, 0(x11)    # mem[x11+0] = x10
 *   ADDI x10, x0, 0     # x10 = 0 (clear)
 *   LW   x12, 0(x11)    # x12 = mem[x11+0]
 *   ADDI x10, x12, 0    # x10 = x12 (result)
 *   EBREAK
 * ================================================================ */
static void test_rv32i_lw_sw(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(10, 0, 42),    /* 0:  x10 = 42 */
        RV_LUI(11, 0x10),      /* 4:  x11 = 0x00010000 (DMEM base) */
        RV_SW(10, 11, 0),      /* 8:  DMEM[0] = x10 */
        RV_ADDI(10, 0, 0),     /* 12: x10 = 0 (clobber) */
        RV_LW(12, 11, 0),      /* 16: x12 = DMEM[0] */
        RV_ADDI(10, 12, 0),    /* 20: x10 = x12 */
        RV_EBREAK              /* 24 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 7);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 30; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "lw_sw halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42 after load-back");
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Load/store at high DMEM offset (0x3FC, last word in DMEM)
 *   ADDI x10, x0, 42     # x10 = 42 (value to store)
 *   ADDI x11, x0, 1020   # x11 = 1020 (DMEM byte offset = 0x3FC)
 *   SW   x10, 0(x11)     # DMEM[1020] = x10
 *   ADDI x10, x0, 0      # x10 = 0 (clobber)
 *   LW   x12, 0(x11)     # x12 = DMEM[1020]
 *   ADDI x10, x12, 0     # x10 = x12 (result)
 *   EBREAK
 * ================================================================ */
static void test_rv32i_lw_sw_high(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(10, 0, 42),    /* 0:  x10 = 42 */
        RV_LUI(11, 0x10),      /* 4:  x11 = 0x00010000 (DMEM base) */
        RV_ADDI(11, 11, 1020), /* 8:  x11 += 1020 (offset 0x3FC) */
        RV_SW(10, 11, 0),      /* 12: DMEM[1020] = x10 */
        RV_ADDI(10, 0, 0),     /* 16: x10 = 0 (clobber) */
        RV_LW(12, 11, 0),      /* 20: x12 = DMEM[1020] */
        RV_ADDI(10, 12, 0),    /* 24: x10 = x12 */
        RV_EBREAK              /* 28 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 8);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 30; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "lw_sw_high halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42 after load-back from high DMEM offset");
    printf("    lw_sw_high: x10=42 cycle=%d\n", halted_cycle);
    fflush(stdout);
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Load/store at offset 0x3F8 (byte 1016).
 * Same as above but at a different high offset to verify
 * DMEM write path across word boundaries.
 * ================================================================ */
static void test_rv32i_lw_sw_high_0x3F8(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(10, 0, 42),    /* 0:  x10 = 42 */
        RV_LUI(11, 0x10),      /* 4:  x11 = 0x00010000 (DMEM base) */
        RV_ADDI(11, 11, 1016), /* 8:  x11 += 1016 (offset 0x3F8) */
        RV_SW(10, 11, 0),      /* 12: DMEM[1016] = x10 */
        RV_ADDI(10, 0, 0),     /* 16: x10 = 0 (clobber) */
        RV_LW(12, 11, 0),      /* 20: x12 = DMEM[1016] */
        RV_ADDI(10, 12, 0),    /* 24: x10 = x12 */
        RV_EBREAK              /* 28 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 8);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 30; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "lw_sw_high_0x3F8 halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42 after load-back from DMEM offset 0x3F8");
    printf("    lw_sw_high_0x3F8: x10=42 cycle=%d\n", halted_cycle);
    fflush(stdout);
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: AUIPC + JALR (position-independent return)
 *   jalr_test: JAL x1, test     # link to test, jump
 *   done:      EBREAK
 *   test:      ADDI x10, x0, 42 # set return value
 *              JALR x0, x1, 0   # return via x1
 * This tests pc_sel=2'b11 (JALR) and wb_sel=2'b10 (PC+4).
 * ================================================================ */
static void test_rv32i_jal_jalr(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    /* PC=0: JAL x1, +8    # link to x1 = PC+4 = 4, jump to PC=8
     * PC=4: LBREAK (fallback, never reached)
     * PC=8: ADDI x10, x0, 42
     * PC=12: JALR x0, x1, 0  # jump to x1 + 0 = 4
     * PC=16: LBREAK (the final halt) -- wait, JALR goes to target.
     *   JAL at PC=0 writes PC+4=4 to x1, jumps to PC=8.
     *   ADDI at PC=8 sets x10=42.
     *   JALR at PC=12 jumps to x1=4 (PC=4), writes PC+4=16 to x0 (discarded).
     *   At PC=4: EBREAK.
     * Wait that's wrong. JALR goes to (rs1 + imm) & ~1. rs1=x1 which was set to 4 (PC+4 from JAL).
     * So JALR target = (4 + 0) & ~1 = 4.
     * Then at PC=4 we'd want EBREAK. But the program has the JAL at PC=0 then EBREAK at PC=4.
     * Let me re-arrange:
     * PC=0: JAL x1, +12   # link to x1 = PC+4 = 4, jump to PC=12. rd=x1, offset=12.
     * PC=4: EBREAK (return target from JALR)
     * PC=8: (unused/EBREAK)
     * PC=12: ADDI x10, x0, 42
     * PC=16: JALR x0, 1, 0  # jump to x1(=4), rd=x0 so no write
     */
    uint32_t program[] = {
        RV_JAL(1, 12),        /* 0:  JAL x1, +12 → PC=12, x1=4 */
        RV_EBREAK,             /* 4:  return target from JALR, halt */
        0,                     /* 8:  padding (unused) */
        RV_ADDI(10, 0, 42),   /* 12: x10 = 42 */
        RV_JALR(0, 1, 0),     /* 16: jump to x1+0 = 4 */
        RV_EBREAK              /* 20: (shouldn't reach) */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 6);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 20; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "jal_jalr halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    mu_assert(strcmp(x10, "01010100000000000000000000000000") == 0,
              "x10 = 42 after JAL/JALR");
    printf("    jal_jalr: x10=%s cycle=%d\n", x10, halted_cycle);
    fflush(stdout);
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: LUI + AUIPC
 *   LUI  x10, 42        # x10 = 42 << 12 = 0x2A000
 *   AUIPC x11, 0         # x11 = PC + 0 = PC at this instruction
 *   EBREAK
 * Expected: x10 = 0x2A000, x11 = 8 (PC of AUIPC instr = 4)
 * Wait: LUI at PC=0: x10 = 42 << 12 = 0x2A000
 * AUIPC at PC=4: x11 = 4 + 0 = 4
 * EBREAK at PC=8
 * ================================================================ */
static void test_rv32i_lui_auipc(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_LUI(10, 42),       /* 0:  x10 = 42 << 12 = 0x2A000 */
        RV_AUIPC(11, 0),       /* 4:  x11 = PC + 0 = 4 */
        RV_EBREAK              /* 8 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 3);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 20; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "lui_auipc halted");

    /* x10 = 0x2A000 = 172032, LSB-first representation */
    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* 172032 = 0x2A000, binary bit31..0:
     * bit16=1 (0x10000), bit18=1 (0x20000), bit19=1 (0x40000), bit17=1 (0x20000)
     * Actually 0x2A000 = 2*16^3 + 10*16^2 + 0 + 0 = 2*4096 + 10*256 = 8192 + 2560 = 10752 + ... wait
     * 0x2A000:
     *   2*65536 = 131072
     *   A*4096 = 10*4096 = 40960
     *   0*256 = 0
     *   0*16 = 0
     *   0*1 = 0
     * Total = 131072 + 40960 = 172032
     * Binary: 10101000000000000000 (bits 17-20 set + others):
     * bit17 = 131072, bit19 = 524288...
     * 172032 = 131072 + 40960
     * 40960 = 32768 + 8192 = 2^15 + 2^13
     * So: 2^17 + 2^15 + 2^13 = 131072 + 32768 + 8192 = 172032
     * LSB-first: bit0=0,1=0,...,12=0,13=1,14=0,15=1,16=0,17=1,18=0,19=0,...,31=0
     * So 32-char string: 0s, then 1 at pos 13, 1 at pos 15, 1 at pos 17
     * = "00000000000001010000000000000000" ... wait let me be more careful
     * bits 0-31 LSB first: 0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,0,...
     * = "00000000000001010100000000000000"
     * Let me verify: pos 13=1, pos 15=1, pos 17=1
     * That gives bits 0-31: 00000000000001010100000000000000
     * String index 13='1', 15='1', 17='1'. ✓ */
    mu_assert(strcmp(x10, "00000000000001010100000000000000") == 0,
              "x10 = 0x2A000");
    printf("    lui: x10=%s (expect 00000000000001010100000000000000)\n", x10);

    /* x11 = 4, LSB-first = "00100000000000000000000000000000" */
    char *x11 = qsim_session_eval_str(sess, "x11");
    mu_assert_not_null(x11);
    mu_assert(strcmp(x11, "00100000000000000000000000000000") == 0,
              "x11 = 4 (AUIPC)");
    printf("    auipc: x11=%s (expect 00100000000000000000000000000000)\n", x11);
    fflush(stdout);
    free(x10); free(x11);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: ALU operations (R-type)
 *   ADDI x10, x0, 7     # x10 = 7
 *   ADDI x11, x0, 3     # x11 = 3
 *   ADD  x12, x10, x11  # x12 = 10
 *   SUB  x13, x10, x11  # x13 = 4
 *   AND  x14, x10, x11  # x14 = 7 & 3 = 3
 *   OR   x15, x10, x11  # x15 = 7 | 3 = 7
 *   XOR  x16, x10, x11  # x16 = 7 ^ 3 = 4
 *   SLT  x17, x11, x10  # x17 = (3 < 7) = 1 (signed)
 *   SLTU x18, x11, x10  # x18 = (3 < 7) = 1 (unsigned)
 *   SLL  x19, x10, x11  # x19 = 7 << 3 = 56
 *   SRL  x20, x10, x11  # x20 = 7 >> 3 = 0
 *   EBREAK
 * ================================================================ */
static void test_rv32i_alu_ops(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(10, 0, 7),    /* 0:  x10 = 7 */
        RV_ADDI(11, 0, 3),    /* 4:  x11 = 3 */
        RV_ADD(12, 10, 11),   /* 8:  x12 = x10 + x11 = 10 */
        RV_SUB(13, 10, 11),   /* 12: x13 = x10 - x11 = 4 */
        RV_AND(14, 10, 11),   /* 16: x14 = x10 & x11 = 3 */
        RV_OR(15, 10, 11),    /* 20: x15 = x10 | x11 = 7 */
        RV_XOR(16, 10, 11),   /* 24: x16 = x10 ^ x11 = 4 */
        RV_SLT(17, 11, 10),   /* 28: x17 = (3 < 7) signed = 1 */
        RV_SLTU(18, 11, 10),  /* 32: x18 = (3 < 7) unsigned = 1 */
        RV_SLL(19, 10, 11),   /* 36: x19 = 7 << 3 = 56 */
        RV_SRL(20, 10, 11),   /* 40: x20 = 7 >> 3 = 0 */
        RV_EBREAK             /* 44 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 12);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 30; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "alu_ops halted");


    /* x12 = 10 = 0xA, LSB-first = "01010000..." */
    char *x12 = qsim_session_eval_str(sess, "x12");
    mu_assert_not_null(x12);
    mu_assert(strcmp(x12, "01010000000000000000000000000000") == 0, "x12 = 10 (ADD)");
    free(x12);

    /* x13 = 4, LSB-first = "00100000..." */
    char *x13 = qsim_session_eval_str(sess, "x13");
    mu_assert_not_null(x13);
    mu_assert(strcmp(x13, "00100000000000000000000000000000") == 0, "x13 = 4 (SUB)");
    free(x13);

    /* x14 = 3, LSB-first = "11000000..." */
    char *x14 = qsim_session_eval_str(sess, "x14");
    mu_assert_not_null(x14);
    mu_assert(strcmp(x14, "11000000000000000000000000000000") == 0, "x14 = 3 (AND)");
    free(x14);

    /* x15 = 7, LSB-first = "11100000..." */
    char *x15 = qsim_session_eval_str(sess, "x15");
    mu_assert_not_null(x15);
    mu_assert(strcmp(x15, "11100000000000000000000000000000") == 0, "x15 = 7 (OR)");
    free(x15);

    /* x16 = 4, LSB-first = "00100000..." */
    char *x16 = qsim_session_eval_str(sess, "x16");
    mu_assert_not_null(x16);
    mu_assert(strcmp(x16, "00100000000000000000000000000000") == 0, "x16 = 4 (XOR)");
    free(x16);

    /* x17 = 1, LSB-first = "10000000..." */
    char *x17 = qsim_session_eval_str(sess, "x17");
    mu_assert_not_null(x17);
    mu_assert(strcmp(x17, "10000000000000000000000000000000") == 0, "x17 = 1 (SLT)");
    free(x17);

    /* x18 = 1, same as x17 */
    char *x18 = qsim_session_eval_str(sess, "x18");
    mu_assert_not_null(x18);
    mu_assert(strcmp(x18, "10000000000000000000000000000000") == 0, "x18 = 1 (SLTU)");
    free(x18);

    /* x19 = 56 = 0x38, LSB-first = "00011101..." */
    char *x19 = qsim_session_eval_str(sess, "x19");
    mu_assert_not_null(x19);
    mu_assert(strcmp(x19, "00011100000000000000000000000000") == 0, "x19 = 56 (SLL)");
    free(x19);

    /* x20 = 0, all zeros */
    char *x20 = qsim_session_eval_str(sess, "x20");
    mu_assert_not_null(x20);
    mu_assert(strcmp(x20, "00000000000000000000000000000000") == 0, "x20 = 0 (SRL)");
    free(x20);

    printf("    alu_ops: all 9 ALU results verified\n");
    fflush(stdout);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: CSR read/write (CSRRW + CSRRS)
 *   ADDI  x12, x0, 8       # x12 = 8 (MIE = bit 3)
 *   CSRRW x0,  x12, 0x300  # mstatus = 8 (write, rd=x0 discarded)
 *   CSRRS x10, x0,  0x300  # x10 = mstatus (read via CSRRS with rs1=x0)
 *   EBREAK
 * Expected: x10 = 8 = 0x00000008
 * ================================================================ */
static void test_rv32i_csr(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    uint32_t program[] = {
        RV_ADDI(12, 0, 8),        /* 0:  x12 = 8 (MIE bit) */
        RV_CSRRW(0, 12, 0x300),   /* 4:  mstatus = 8 */
        RV_CSRRS(10, 0, 0x300),   /* 8:  x10 = mstatus = 8 */
        RV_EBREAK                 /* 12 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 4);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 20; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "csr halted");

    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* x10 = 8 = 0x00000008, binary bit3=1, LSB-first: pos 3 = 1 */
    mu_assert(strcmp(x10, "00010000000000000000000000000000") == 0,
              "x10 = 8 (CSRRS mstatus)");
    printf("    csr: x10=%s cycle=%d\n", x10, halted_cycle);
    fflush(stdout);
    free(x10);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Timer interrupt
 *   Program sets timer_compare=30, enables MIE+MTIE, sets mtvec to
 *   EBREAK address (word 64 = byte 0x100), enters wait loop.
 *   Interrupt fires when timer_counter >= 30, CPU jumps to mtvec=0x100,
 *   hits EBREAK, halts.
 * Expected: halted at mtvec, mcause = 0x80000007, mepc != 0
 * ================================================================ */
static void test_rv32i_timer_interrupt(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    /* Program layout:
     *   0x00: LUI    x10, 0x20000    # x10 = 0x20000000
     *   0x04: ADDI   x11, x0, 30     # x11 = 30
     *   0x08: SW     x11, 0(x10)     # timer_compare = 30
     *   0x0C: ADDI   x12, x0, 0x8    # x12 = 8 (MIE bit)
     *   0x10: CSRRW  x0, x12, 0x300  # mstatus = 8
     *   0x14: ADDI   x13, x0, 0x80   # x13 = 128 (MTIE bit 7)
     *   0x18: CSRRW  x0, x13, 0x304  # mie = 0x80
     *   0x1C: ADDI   x14, x0, 0x100  # x14 = 0x100 (mtvec)
     *   0x20: CSRRW  x0, x14, 0x305  # mtvec = 0x100
     *   0x24: ADDI   x15, x0, 500    # wait loop counter
     *   0x28: ADDI   x15, x15, -1    # delay:
     *   0x2C: BNE    x15, x0, -8     # if x15 != 0, goto 0x28
     *   0x30: EBREAK                 # should not reach (interrupt fires first)
     *
     * Unused imem slots filled with EBREAK, so imem[64] (byte 0x100) = EBREAK.
     * When interrupt fires, CPU goes to mtvec=0x100 -> EBREAK -> halt.
     */
    uint32_t program[] = {
        RV_LUI(10, 0x20000),         /* 0x00 */
        RV_ADDI(11, 0, 30),          /* 0x04 */
        RV_SW(11, 10, 0),            /* 0x08 */
        RV_ADDI(12, 0, 0x8),         /* 0x0C */
        RV_CSRRW(0, 12, 0x300),      /* 0x10 */
        RV_ADDI(13, 0, 0x80),        /* 0x14 */
        RV_CSRRW(0, 13, 0x304),      /* 0x18 */
        RV_ADDI(14, 0, 0x100),       /* 0x1C */
        RV_CSRRW(0, 14, 0x305),      /* 0x20 */
        RV_ADDI(15, 0, 500),         /* 0x24 */
        RV_ADDI(15, 15, -1),         /* 0x28 */
        RV_BNE(15, 0, -4),          /* 0x2C */
        RV_EBREAK                    /* 0x30 */
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 13);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 200; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "timer_interrupt halted");

    /* Check mcause = 0x80000007 (machine timer interrupt) */
    char *mcause = qsim_session_eval_str(sess, "csr_mcause");
    mu_assert_not_null(mcause);
    /* 0x80000007: bit31=1, bits2-0=111, LSB-first = 11100000...0001 */
    mu_assert(strcmp(mcause, "11100000000000000000000000000001") == 0,
              "mcause = 0x80000007");
    printf("    timer_int: mcause=%s cycle=%d\n", mcause, halted_cycle);

    /* Check mepc != 0 (interrupted at some valid PC) */
    char *mepc = qsim_session_eval_str(sess, "csr_mepc");
    mu_assert_not_null(mepc);
    /* mepc should be non-zero: at least bit 0 of some value set */
    int mepc_nonzero = 0;
    for (int i = 0; i < 32; i++)
        if (mepc[i] == '1') { mepc_nonzero = 1; break; }
    mu_assert(mepc_nonzero != 0, "mepc != 0");
    printf("    timer_int: mepc=%s\n", mepc);

    /* Check mstatus[3] = 0 (MIE cleared on interrupt) */
    char *mstatus = qsim_session_eval_str(sess, "csr_mstatus");
    mu_assert_not_null(mstatus);
    /* mstatus[3] = LSB-first char at index 3 should be '0' */
    mu_assert(mstatus[3] == '0', "mstatus[3] = 0 (MIE cleared)");
    printf("    timer_int: mstatus=%s\n", mstatus);

    /* Check pc = 0x100 (mtvec target) */
    char *pc_val = qsim_session_eval_str(sess, "pc");
    mu_assert_not_null(pc_val);
    /* 0x100 = byte 256, binary: bit8=1, LSB-first pos 8 = 1 */
    mu_assert(pc_val[8] == '1', "pc = 0x100 (mtvec)");
    printf("    timer_int: pc=%s\n", pc_val);

    fflush(stdout);
    free(mcause); free(mepc); free(mstatus); free(pc_val);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: MRET (interrupt entry + return)
 *   Setup timer, enable MIE+MTIE, mtvec = handler at 0x80.
 *   Handler: clear timer_compare (SW x0, 0(x10)), then MRET.
 *   After return: wait loop completes, set x10=123, EBREAK halt.
 * Expected: x10 = 123 after full interrupt/return sequence
 * ================================================================ */
static void test_rv32i_mret(void) {
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    mu_assert_not_null(src);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    mu_assert(ok, "compile rv32i_top.v");
    free(src);

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    /* Program layout:
     *   0x00: LUI    x10, 0x20000    # x10 = 0x20000000 (timer addr)
     *   0x04: ADDI   x11, x0, 10     # x11 = 10 (timer compare)
     *   0x08: SW     x11, 0(x10)     # timer_compare = 10
     *   0x0C: ADDI   x12, x0, 8      # x12 = 8 (MIE)
     *   0x10: CSRRW  x0, x12, 0x300  # mstatus = 8
     *   0x14: ADDI   x13, x0, 0x80   # x13 = 128 (MTIE)
     *   0x18: CSRRW  x0, x13, 0x304  # mie = 0x80
     *   0x1C: LUI    x14, 0x00000    # x14 = 0
     *   0x20: ADDI   x14, x0, 0x80   # x14 = 0x80 (mtvec)
     *   0x24: CSRRW  x0, x14, 0x305  # mtvec = 0x80
     *   0x28: ADDI   x15, x0, 100    # wait loop counter
     *   0x2C: ADDI   x15, x15, -1    # delay:
     *   0x30: BNE    x15, x0, -8     # if x15 != 0, goto 0x2C
     *   0x34: ADDI   x10, x0, 123    # x10 = 123 (result)
     *   0x38: EBREAK                 # halt
     *
     * Padding at indices 15-31 (NOPs = 0s).
     * Handler at 0x80 (word 32):
     *   SW x0, 0(x10)   # clear timer_compare (x10 still = 0x20000000)
     *   MRET
     */
    uint32_t program[34] = {0};
    program[0]  = RV_LUI(10, 0x20000);
    program[1]  = RV_ADDI(11, 0, 10);
    program[2]  = RV_SW(11, 10, 0);
    program[3]  = RV_ADDI(12, 0, 8);
    program[4]  = RV_CSRRW(0, 12, 0x300);
    program[5]  = RV_ADDI(13, 0, 0x80);
    program[6]  = RV_CSRRW(0, 13, 0x304);
    program[7]  = RV_LUI(14, 0);
    program[8]  = RV_ADDI(14, 0, 0x80);
    program[9]  = RV_CSRRW(0, 14, 0x305);
    program[10] = RV_ADDI(15, 0, 100);
    program[11] = RV_ADDI(15, 15, -1);
    program[12] = RV_BNE(15, 0, -4);
    program[13] = RV_ADDI(10, 0, 123);
    program[14] = RV_EBREAK;
    /* Handler at word 32 (byte 0x80) */
    program[32] = RV_SW(0, 10, 0);            /* sw x0, 0(x10): clear timer_compare */
    program[33] = RV_MRET;                    /* return from interrupt */

    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 34);
    ok = qsim_session_force_str(sess, "imem", imem_str);
    mu_assert(ok != 0, "force imem");

    session_reset(sess);

    int halted_cycle = -1;
    for (int cycle = 0; cycle < 2000; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { halted_cycle = cycle; free(h); break; }
        free(h);
    }
    mu_assert(halted_cycle >= 0, "mret halted");

    /* x10 should be 123 after MRET + wait loop completes */
    char *x10 = qsim_session_eval_str(sess, "x10");
    mu_assert_not_null(x10);
    /* 123 = 0x7B = 0111 1011, LSB-first = "11011110" + 24 zeros */
    mu_assert(strcmp(x10, "11011110000000000000000000000000") == 0,
              "x10 = 123 after MRET");
    printf("    mret: x10=%s cycle=%d\n", x10, halted_cycle);

    /* Verify MIE=1 after MRET */
    char *mstatus = qsim_session_eval_str(sess, "csr_mstatus");
    mu_assert_not_null(mstatus);
    mu_assert(mstatus[3] == '1', "mstatus[3] = 1 after MRET (MIE restored)");

    fflush(stdout);
    free(x10); free(mstatus);
    qsim_session_free(sess);
}

/* ================================================================
 * Registration
 * ================================================================ */
void register_rv32i_tests(void) {
    printf("[RV32I CPU]\n");
    mu_run_test(test_rv32i_addi);
    mu_run_test(test_rv32i_fib10);
    mu_run_test(test_rv32i_mul);
    mu_run_test(test_rv32i_lw_sw);
    mu_run_test(test_rv32i_lw_sw_high);
    mu_run_test(test_rv32i_lw_sw_high_0x3F8);
    mu_run_test(test_rv32i_jal_jalr);
    mu_run_test(test_rv32i_lui_auipc);
    mu_run_test(test_rv32i_alu_ops);
    mu_run_test(test_rv32i_csr);
    mu_run_test(test_rv32i_timer_interrupt);
    mu_run_test(test_rv32i_mret);
    printf("\n");
}
