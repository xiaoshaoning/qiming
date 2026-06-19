/* Standalone diagnostic: run just the RV32I ADDI test with cycle trace */
#include "libqsim/session.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Copy the needed defines and helpers from test_cpu_rv32i.c */
#define RV_EBREAK 0x00100073
#define RV_I_TYPE(funct3, rd, rs1, imm12) \
    ((((imm12) & 0xFFF) << 20) | ((rs1) << 15) | ((funct3) << 12) | ((rd) << 7) | 0x13)
#define RV_ADDI(rd, rs1, imm12)  RV_I_TYPE(0, rd, rs1, imm12)

#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

static char *read_rv32i_verilog(size_t *out_len) {
    const char *paths[] = {"../../example/rv32i/", "../../../example/rv32i/",
                           "../example/rv32i/", "example/rv32i/",
                           "../libqsim/../example/rv32i/"};
    char full[512]; FILE *f = NULL;
    for (int i = 0; i < 5; i++) {
        snprintf(full, sizeof(full), "%s%s", paths[i], "rv32i_top.v");
        f = fopen(full, "rb");
        if (f) break;
    }
    if (!f) {
        /* Final fallback: absolute path */
        f = fopen("D:/codes/qiming/example/rv32i/rv32i_top.v", "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = (char*)malloc((size_t)len + 1);
    if (!src) { fclose(f); return NULL; }
    size_t nread = fread(src, 1, (size_t)len, f); fclose(f);
    src[nread] = '\0';
    *out_len = nread;
    return src;
}

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

static void session_reset(qsim_session_t *sess) {
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "1");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_step_delta(sess);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("starting...\n");
    size_t src_len;
    char *src = read_rv32i_verilog(&src_len);
    if (!src) { printf("FAIL: could not read rv32i_top.v\n"); return 1; }

    qsim_session_t *sess = qsim_session_create();
    if (!sess) { printf("FAIL: create session\n"); free(src); return 1; }

    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    if (!ok) { printf("FAIL: compile\n"); free(src); qsim_session_free(sess); return 1; }
    free(src);

    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("FAIL: elaborate\n"); qsim_session_free(sess); return 1; }

    uint32_t program[] = {
        RV_ADDI(10, 0, 42),
        RV_EBREAK
    };
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, 2);

    /* Print signal list */
    int sig_count = qsim_session_get_signal_count(sess);
    printf("signal_count=%d\n", sig_count);
    for (int si = 0; si < sig_count && si < 120; si++) {
        const char *sn = qsim_session_get_signal_name(sess, si);
        if (sn) printf("  [%d] %s\n", si, sn);
    }

    ok = qsim_session_force_str(sess, "imem", imem_str);
    if (!ok) { printf("FAIL: force imem\n"); qsim_session_free(sess); return 1; }

    session_reset(sess);

    /* Print remaining signal names */
    for (int si = 120; si < sig_count; si++) {
        const char *sn = qsim_session_get_signal_name(sess, si);
        if (sn) printf("  [%d] %s\n", si, sn);
    }

    for (int cycle = 0; cycle < 15; cycle++) {
        /* Read state BEFORE posedge (after previous NBAs settled) */
        char *h = qsim_session_eval_str(sess, "halted");
        char *pc = qsim_session_eval_str(sess, "pc");
        char *x10 = qsim_session_eval_str(sess, "x10");
        char *ifid = qsim_session_eval_str(sess, "ifid_instr");
        char *idex_rd_raw = qsim_session_eval_str(sess, "idex_rd");
        char *idex_rs2_raw = qsim_session_eval_str(sess, "idex_rs2");
        char *idex_pc_raw = qsim_session_eval_str(sess, "idex_pc");
        char *bubble = qsim_session_eval_str(sess, "bubble_idex");

        printf("PRE%d: pc=%s ifid=%s rd=%s rs2=%s ipc=%s bub=%s x10=%s halt=%s\n",
               cycle, pc?pc:"?", ifid?ifid:"?", idex_rd_raw?idex_rd_raw:"?",
               idex_rs2_raw?idex_rs2_raw:"?", idex_pc_raw?idex_pc_raw:"?",
               bubble?bubble:"?", x10?x10:"?", h?h:"?");
        free(pc); free(x10); free(ifid); free(idex_rd_raw); free(idex_rs2_raw);
        free(idex_pc_raw); free(bubble); free(h);

        /* Posedge */
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);

        /* Read state AFTER posedge */
        h = qsim_session_eval_str(sess, "halted");
        pc = qsim_session_eval_str(sess, "pc");
        x10 = qsim_session_eval_str(sess, "x10");
        ifid = qsim_session_eval_str(sess, "ifid_instr");
        char *id_rd = qsim_session_eval_str(sess, "id_rd");
        char *id_rs2 = qsim_session_eval_str(sess, "id_rs2");
        /* Read idex_rd by index: signal [99] vs [129] */
        qsim_bit_vector_t idex_rd_idx99_v  = qsim_session_get_signal_value(sess, 99);
        qsim_bit_vector_t idex_rd_idx129_v = qsim_session_get_signal_value(sess, 129);
        /* Use LSB-first format: manually extract bits */
        char idex_rd_bf99[64], idex_rd_bf129[64];
        for (uint32_t bi = 0; bi < idex_rd_idx99_v.width && bi < 63; bi++)
            idex_rd_bf99[bi] = (idex_rd_idx99_v.bits[bi].state == QSIM_1) ? '1' : '0';
        idex_rd_bf99[idex_rd_idx99_v.width] = '\0';
        for (uint32_t bi = 0; bi < idex_rd_idx129_v.width && bi < 63; bi++)
            idex_rd_bf129[bi] = (idex_rd_idx129_v.bits[bi].state == QSIM_1) ? '1' : '0';
        idex_rd_bf129[idex_rd_idx129_v.width] = '\0';
        char wd_str[32];
        snprintf(wd_str, sizeof(wd_str), "w99=%u w129=%u", idex_rd_idx99_v.width, idex_rd_idx129_v.width);
        idex_rd_raw = qsim_session_eval_str(sess, "idex_rd");
        idex_rs2_raw = qsim_session_eval_str(sess, "idex_rs2");
        idex_pc_raw = qsim_session_eval_str(sess, "idex_pc");
        bubble = qsim_session_eval_str(sess, "bubble_idex");
        char *flush = qsim_session_eval_str(sess, "debug_flush_ifid");
        char *stall = qsim_session_eval_str(sess, "debug_load_use_stall");
        char *debug_idex_rd = qsim_session_eval_str(sess, "debug_idex_rd");
        char *debug_idex_pc = qsim_session_eval_str(sess, "debug_idex_pc");
        char *opcode = qsim_session_eval_str(sess, "id_opcode");

        printf("CYC%d:", cycle);
        printf(" pc=%s", pc?pc:"?");
        printf(" ifid=%s", ifid?ifid:"?");
        printf(" op=%s", opcode?opcode:"?");
        printf(" rd=[99]=%s [129]=%s(%s) dbg=%s", idex_rd_bf99, idex_rd_bf129, wd_str, debug_idex_rd?debug_idex_rd:"?");
        printf(" rs2=%s", idex_rs2_raw?idex_rs2_raw:"?");
        printf(" id_rd=%s id_rs2=%s", id_rd?id_rd:"?", id_rs2?id_rs2:"?");
        printf(" ipc=%s", idex_pc_raw?idex_pc_raw:"?");
        printf(" bub=%s fl=%s st=%s", bubble?bubble:"?", flush?flush:"?", stall?stall:"?");
        printf(" x10=%s", x10?x10:"?");
        printf(" halt=%s", h?h:"?");
        printf("\n");

        free(h); free(pc); free(x10); free(ifid);
        free(id_rd); free(id_rs2);
        free(idex_rd_raw); free(idex_rs2_raw); free(idex_pc_raw);
        free(bubble); free(flush); free(stall);
        free(debug_idex_rd); free(debug_idex_pc); free(opcode);

        if (h && h[0] == '1') break;
    }

    qsim_session_free(sess);
    printf("DONE\n");
    return 0;
}
