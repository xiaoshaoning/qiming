/* Debug VHDL CPU: full program load + pipeline trace */
#include "libqsim/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

static size_t read_vhdl(size_t *out_len, char **out) {
    const char *paths[] = {"../../example/rv32i_vhdl/rv32i_top.vhd","../../../example/rv32i_vhdl/rv32i_top.vhd","../example/rv32i_vhdl/rv32i_top.vhd","example/rv32i_vhdl/rv32i_top.vhd"};
    FILE *f = NULL;
    for (int i = 0; i < 4; i++) { f = fopen(paths[i], "rb"); if (f) break; }
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)len + 1);
    size_t nread = fread(src, 1, (size_t)len, f); src[nread] = '\0';
    fclose(f); *out = src; *out_len = nread; return (size_t)len;
}

static void show(qsim_session_t *s, const char *n) {
    char *v = qsim_session_eval_str(s, n);
    printf("  %s=%s\n", n, v ? v : "NULL");
    free(v);
}

/* Convert hex words to LSB-first bit string */
static void build_imem_str32(char *out, const uint32_t *program, size_t count) {
    for (int i = 0; i < IMEM_WORDS; i++) {
        uint32_t val = ((size_t)i < count) ? program[i] : 0x00100073;
        for (int b = 0; b < 32; b++) {
            int pos = IMEM_BITS - 1 - (i * 32 + b);
            out[pos] = ((val >> b) & 1) ? '1' : '0';
        }
    }
    out[IMEM_BITS] = '\0';
}

static size_t read_hex_file(const char *path, uint32_t *words, size_t max_words) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t count = 0;
    char line[64];
    while (count < max_words && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' ')) len--;
        p[len] = '\0';
        unsigned long v = strtoul(p, NULL, 16);
        words[count++] = (uint32_t)v;
    }
    fclose(f);
    return count;
}

int main(void) {
    /* Read VHDL source */
    size_t len; char *src;
    if (!read_vhdl(&len, &src)) { fprintf(stderr, "Failed to read VHDL\n"); return 1; }

    /* Create session */
    qsim_session_t *sess = qsim_session_create();
    if (!qsim_session_compile_string(sess, "rv32i_top.vhd", src)) {
        fprintf(stderr, "Compile failed\n"); return 1;
    }
    free(src);
    if (!qsim_session_elaborate(sess)) {
        fprintf(stderr, "Elaboration failed\n"); return 1;
    }

    /* Load hex program (addi_test.hex: ADDI x10, x0, 42, then EBREAK) */
    uint32_t *program = calloc(IMEM_WORDS, sizeof(uint32_t));
    if (!program) { fprintf(stderr, "OOM\n"); return 1; }
    const char *hex_paths[] = {"../../example/rv32i/tests/addi_test.hex",
                                "../example/rv32i/tests/addi_test.hex",
                                "example/rv32i/tests/addi_test.hex",
                                "../../../example/rv32i/tests/addi_test.hex"};
    size_t nwords = 0;
    for (int i = 0; i < 4; i++) {
        nwords = read_hex_file(hex_paths[i], program, IMEM_WORDS);
        if (nwords > 0) break;
    }
    printf("Loaded %zu words from addi_test.hex\n", nwords);

    /* Initialize IMEM — heap-allocate for large bit vector */
    char *imem_str = malloc(IMEM_BITS + 1);
    if (!imem_str) { free(program); fprintf(stderr, "OOM\n"); return 1; }
    build_imem_str32(imem_str, program, nwords);
    int ok = qsim_session_force_str(sess, "imem_port", imem_str);

    /* Dump key signals */
    size_t nsig = qsim_session_get_signal_count(sess);
    for (size_t i = 0; i < nsig; i++) {
        const char *n = qsim_session_get_signal_name(sess, i);
        if (n && (strcmp(n, "pc_reg") == 0 || strcmp(n, "halted_reg") == 0 ||
                  strcmp(n, "ifid_instr") == 0 || strcmp(n, "if_instr") == 0 ||
                  strcmp(n, "idex_alu_control") == 0 || strcmp(n, "ex_is_ebreak") == 0 ||
                  strcmp(n, "reg_x10") == 0))
            printf("sig[%zu] = %s\n", i, n);
    }

    /* Reset */
    printf("\n=== Reset ===\n");
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);

    qsim_session_set_str(sess, "rst", "1");
    qsim_session_step_delta(sess);

    qsim_session_set_str(sess, "rst", "0");
    qsim_session_step_delta(sess);

    show(sess, "pc_reg"); show(sess, "halted_reg");

    /* Clock cycles with pipeline tracing */
    printf("\n=== Clock cycles ===\n");
    printf("Cycle pc_reg       ifid_instr    if_instr      idex_alu_ctrl ex_is_ebreak halted_reg\n");
    for (int i = 0; i < 20; i++) {
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);

        char *pcv = qsim_session_eval_str(sess, "pc_reg");
        char *ifid = qsim_session_eval_str(sess, "ifid_instr");
        char *iff = qsim_session_eval_str(sess, "if_instr");
        char *ctrl = qsim_session_eval_str(sess, "idex_alu_control");
        char *ebrk = qsim_session_eval_str(sess, "ex_is_ebreak");
        char *halt = qsim_session_eval_str(sess, "halted_reg");
        printf("%3d pos: %s %s %s %s %s %s\n",
               i+1, pcv ? pcv : "?", ifid ? ifid : "?", iff ? iff : "?",
               ctrl ? ctrl : "?", ebrk ? ebrk : "?", halt ? halt : "?");
        free(pcv); free(ifid); free(iff); free(ctrl); free(ebrk); free(halt);

        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);

        if (i < 3) {
            char *x10 = qsim_session_eval_str(sess, "reg_x10");
            printf("       reg_x10=%s\n", x10 ? x10 : "?");
            free(x10);
        }
    }

    qsim_session_free(sess);
    free(program);
    free(imem_str);
    return 0;
}
