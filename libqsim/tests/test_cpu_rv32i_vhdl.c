/* RV32I VHDL tests: compile rv32i_top.vhd and run hex program tests.
 * Tests that the VHDL version of the 5-stage pipeline CPU works correctly
 * with the qsim VHDL parser and simulation engine. */
#include "minunit.h"
#include "libqsim/session.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ================================================================
 * VHDL source reader
 * ================================================================ */

static size_t read_rv32i_vhdl(size_t *out_len, char **out) {
    const char *paths[] = {"../../example/rv32i_vhdl/rv32i_top.vhd",
                           "../../../example/rv32i_vhdl/rv32i_top.vhd",
                           "example/rv32i_vhdl/rv32i_top.vhd",
                           "../example/rv32i_vhdl/rv32i_top.vhd",
                           "../libqsim/../example/rv32i_vhdl/rv32i_top.vhd"};
    FILE *f = NULL;
    for (int i = 0; i < 5; i++) {
        f = fopen(paths[i], "rb");
        if (f) break;
    }
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)len + 1);
    if (!src) { fclose(f); return 0; }
    size_t nread = fread(src, 1, (size_t)len, f);
    src[nread] = '\0';
    fclose(f);
    *out = src;
    *out_len = nread;
    return (size_t)len;
}

static qsim_session_t *create_vhdl_session(void) {
    size_t src_len;
    char *src;
    if (!read_rv32i_vhdl(&src_len, &src)) return NULL;
    qsim_session_t *sess = qsim_session_create();
    if (!sess) { free(src); return NULL; }
    int ok = qsim_session_compile_string(sess, "rv32i_top.vhd", src);
    free(src);
    if (!ok) { qsim_session_free(sess); return NULL; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { qsim_session_free(sess); return NULL; }
    return sess;
}

/* ================================================================
 * Helpers (same as test_cpu_rv32i_hex.c)
 * ================================================================ */

#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

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

static void session_reset(qsim_session_t *sess) {
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "1");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_step_delta(sess);
}

static char *session_clock_cycle(qsim_session_t *sess) {
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "1");
    qsim_session_step_delta(sess);
    return qsim_session_eval_str(sess, "halted");
}

static int run_program(qsim_session_t *sess, const uint32_t *program,
                       size_t count, int max_cycles) {
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, count);
    int ok = qsim_session_force_str(sess, "imem_port", imem_str);
    if (!ok) return -1;
    session_reset(sess);
    for (int cycle = 0; cycle < max_cycles; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { free(h); return cycle; }
        free(h);
    }
    return -2;
}

static int check_reg_str(qsim_session_t *sess, const char *reg,
                         const char *expected) {
    char *val = qsim_session_eval_str(sess, reg);
    if (!val) return 0;
    int match = strcmp(val, expected) == 0;
    free(val);
    return match;
}

static void val_to_lsb(char *out, uint32_t val) {
    for (int b = 0; b < 32; b++)
        out[b] = ((val >> b) & 1) ? '1' : '0';
    out[32] = '\0';
}

/* Resolve hex file path relative to test binary location */
static size_t read_hex_file_resolve(const char *name, uint32_t *words, size_t max_words) {
    const char *paths[] = {"", "../../example/rv32i/tests/", "../example/rv32i/tests/",
                           "example/rv32i/tests/", "../../../example/rv32i/tests/"};
    char full[512];
    for (int i = 0; i < 5; i++) {
        snprintf(full, sizeof(full), "%s%s", paths[i], name);
        size_t n = read_hex_file(full, words, max_words);
        if (n > 0) return n;
    }
    return 0;
}

/* ================================================================
 * Test: ADDI x10, x0, 42 via hex file => x10 = 42
 * ================================================================ */
static void test_vhdl_addi(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("addi_test.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read addi_test.hex");

    qsim_session_t *sess = create_vhdl_session();
    mu_assert_not_null(sess);

    /* Print program words */
    for (size_t i = 0; i < n && i < 8; i++)
        printf("    [%zu] 0x%08X\n", i, (unsigned)words[i]);

    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, words, n);
    int ok = qsim_session_force_str(sess, "imem_port", imem_str);
    mu_assert(ok, "force imem_port");
    session_reset(sess);

    int hc = -2;
    for (int cycle = 0; cycle < 30; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { hc = cycle; free(h); break; }
        /* Print debug signals each cycle */
        char *pcv = qsim_session_eval_str(sess, "pc");
        char *instrv = qsim_session_eval_str(sess, "instruction");
        char *x10v = qsim_session_eval_str(sess, "reg_x10");
        char *h2 = qsim_session_eval_str(sess, "halted");
        char *dbg_branch = qsim_session_eval_str(sess, "debug_idex_branch");
        char *dbg_br_taken = qsim_session_eval_str(sess, "debug_branch_taken");
        char *dbg_jal = qsim_session_eval_str(sess, "debug_idex_pc");
        char *dbg_imm = qsim_session_eval_str(sess, "debug_idex_imm");
        char *dbg_br_target = qsim_session_eval_str(sess, "debug_ex_branch_target");
        char *dbg_stall = qsim_session_eval_str(sess, "debug_load_use_stall");
        char *dbg_flush = qsim_session_eval_str(sess, "debug_flush_ifid");
        char *dbg_idex_rd = qsim_session_eval_str(sess, "debug_idex_rd");
        /* Convert LSB strings to uint32_t for readability */
        uint32_t pc_val = 0, instr_val = 0, imm_val = 0;
        if (pcv) for (int b=0; b<32 && b<(int)strlen(pcv); b++) if (pcv[b]=='1') pc_val |= (1u<<b);
        if (instrv) for (int b=0; b<32 && b<(int)strlen(instrv); b++) if (instrv[b]=='1') instr_val |= (1u<<b);
        if (dbg_imm) for (int b=0; b<32 && b<(int)strlen(dbg_imm); b++) if (dbg_imm[b]=='1') imm_val |= (1u<<b);
        uint32_t opcode = instr_val & 0x7F;
        printf("    cycle=%d pc=0x%04X instr=0x%08X op=0x%02X imm=0x%08X br=%s taken=%s idex_pc=%s targ=%s stall=%s flush=%s rd=%s\n",
               cycle, pc_val, instr_val, opcode, imm_val,
               dbg_branch ? dbg_branch : "?", dbg_br_taken ? dbg_br_taken : "?",
               dbg_jal ? dbg_jal : "?", dbg_br_target ? dbg_br_target : "?",
               dbg_stall ? dbg_stall : "?", dbg_flush ? dbg_flush : "?",
               dbg_idex_rd ? dbg_idex_rd : "?");
        free(pcv); free(instrv); free(x10v); free(h2);
        free(dbg_branch); free(dbg_br_taken); free(dbg_jal);
        free(dbg_imm); free(dbg_br_target); free(dbg_stall); free(dbg_flush); free(dbg_idex_rd);
        free(h);
    }
    mu_assert(hc >= 0, "addi: didn't halt");

    char expect[33];
    val_to_lsb(expect, 42);
    mu_assert(check_reg_str(sess, "reg_x10", expect), "x10 = 42");
    printf("    vhdl_addi: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Fibonacci(10) = 55
 * ================================================================ */
static void test_vhdl_fib10(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("fib.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read fib.hex");

    qsim_session_t *sess = create_vhdl_session();
    mu_assert_not_null(sess);

    /* Print program */
    printf("    fib program (%zu words):\n", n);
    for (size_t i = 0; i < n && i < 14; i++)
        printf("      [%zu] 0x%08X\n", i, (unsigned)words[i]);

    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, words, n);
    int ok = qsim_session_force_str(sess, "imem_port", imem_str);
    mu_assert(ok, "force imem_port");
    session_reset(sess);

    int hc = -2;
    for (int cycle = 0; cycle < 200; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { hc = cycle; free(h); break; }
        char *pcv = qsim_session_eval_str(sess, "pc");
        char *instrv = qsim_session_eval_str(sess, "instruction");
        char *x10v = qsim_session_eval_str(sess, "reg_x10");
        char *x14v = qsim_session_eval_str(sess, "x14"); /* n counter */
        char *x15v = qsim_session_eval_str(sess, "x15"); /* fib cur */
        char *x13v = qsim_session_eval_str(sess, "x13"); /* fib prev */
        char *h2 = qsim_session_eval_str(sess, "halted");
        char *dbg_branch = qsim_session_eval_str(sess, "debug_idex_branch");
        char *dbg_br_taken = qsim_session_eval_str(sess, "debug_branch_taken");
        char *dbg_flush = qsim_session_eval_str(sess, "debug_flush_ifid");
        char *dbg_stall = qsim_session_eval_str(sess, "debug_load_use_stall");
        char *dbg_idex_rd = qsim_session_eval_str(sess, "debug_idex_rd");
        char *dbg_fwd_rs1 = qsim_session_eval_str(sess, "debug_forward_rs1_val");
        char *dbg_fwd_rs2 = qsim_session_eval_str(sess, "debug_forward_rs2_val");
        char *dbg_idex_rs1 = qsim_session_eval_str(sess, "debug_idex_rs1");
        char *dbg_idex_rs2 = qsim_session_eval_str(sess, "debug_idex_rs2");
        char *dbg_idex_imm = qsim_session_eval_str(sess, "debug_idex_imm");
        char *dbg_ax = qsim_session_eval_str(sess, "alu_result");
        char *dbg_alu_a = qsim_session_eval_str(sess, "debug_alu_a");
        char *dbg_alu_b = qsim_session_eval_str(sess, "debug_alu_b");
        char *dbg_ex_alu_final = qsim_session_eval_str(sess, "debug_ex_alu_final");
        char *dbg_exmem_wdata = qsim_session_eval_str(sess, "debug_exmem_write_data");
        char *dbg_imm_real = qsim_session_eval_str(sess, "debug_idex_imm_real");
        uint32_t pc_val = 0, instr_val = 0;
        if (pcv) for (int b=0; b<32 && b<(int)strlen(pcv); b++) if (pcv[b]=='1') pc_val |= (1u<<b);
        if (instrv) for (int b=0; b<32 && b<(int)strlen(instrv); b++) if (instrv[b]=='1') instr_val |= (1u<<b);
        uint32_t opcode = instr_val & 0x7F;
        printf("    fib: c=%d pc=0x%04X instr=0x%08X op=0x%02X x10=%s x13=%s x14=%s x15=%s "
               "br=%s tk=%s fl=%s st=%s rd=%s f1=%s f2=%s rs1=%s rs2=%s imm=%s"
               " alu=%s aa=%s ab=%s ef=%s wd=%s ir=%s\n",
               cycle, pc_val, instr_val, opcode,
               x10v ? x10v : "?", x13v ? x13v : "?", x14v ? x14v : "?", x15v ? x15v : "?",
               dbg_branch ? dbg_branch : "?", dbg_br_taken ? dbg_br_taken : "?",
               dbg_flush ? dbg_flush : "?", dbg_stall ? dbg_stall : "?",
               dbg_idex_rd ? dbg_idex_rd : "?",
               dbg_fwd_rs1 ? dbg_fwd_rs1 : "?", dbg_fwd_rs2 ? dbg_fwd_rs2 : "?",
               dbg_idex_rs1 ? dbg_idex_rs1 : "?", dbg_idex_rs2 ? dbg_idex_rs2 : "?",
               dbg_idex_imm ? dbg_idex_imm : "?", dbg_ax ? dbg_ax : "?",
               dbg_alu_a ? dbg_alu_a : "?", dbg_alu_b ? dbg_alu_b : "?",
               dbg_ex_alu_final ? dbg_ex_alu_final : "?",
               dbg_exmem_wdata ? dbg_exmem_wdata : "?",
               dbg_imm_real ? dbg_imm_real : "?");
        free(pcv); free(instrv); free(x10v); free(x14v); free(x15v); free(h2);
        free(dbg_branch); free(dbg_br_taken); free(dbg_flush); free(dbg_stall); free(dbg_idex_rd);
        free(dbg_fwd_rs1); free(dbg_fwd_rs2); free(dbg_idex_rs1); free(dbg_idex_rs2);
        free(dbg_idex_imm); free(dbg_ax);
        free(dbg_alu_a); free(dbg_alu_b); free(dbg_ex_alu_final); free(dbg_exmem_wdata); free(dbg_imm_real);
        free(h);
    }
    mu_assert(hc >= 0, "fib: didn't halt");

    char expect[33];
    val_to_lsb(expect, 55);
    mu_assert(check_reg_str(sess, "reg_x10", expect), "x10 = 55");
    printf("    vhdl_fib10: x10=55 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Multiply 7 * 6 = 42
 * ================================================================ */
static void test_vhdl_mul(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("multiply.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read multiply.hex");

    qsim_session_t *sess = create_vhdl_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 20);
    mu_assert(hc >= 0, "mul: didn't halt");

    char expect[33];
    val_to_lsb(expect, 42);
    mu_assert(check_reg_str(sess, "reg_x10", expect), "x10 = 42");
    printf("    vhdl_mul: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Stack-based C program
 * ================================================================ */
static void test_vhdl_stack(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("stack_test.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read stack_test.hex");

    qsim_session_t *sess = create_vhdl_session();
    mu_assert_not_null(sess);

    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, words, n);
    qsim_session_force_str(sess, "imem_port", imem_str);
    session_reset(sess);

    int hc = -1;
    for (int cycle = 0; cycle < 200; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { hc = cycle; free(h); break; }
        free(h);
    }
    mu_assert(hc >= 0, "stack: didn't halt");

    char expect[33];
    val_to_lsb(expect, 42);
    mu_assert(check_reg_str(sess, "reg_x10", expect), "x10 = 42");
    printf("    vhdl_stack: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Bubble sort (16 elements via C-compiled hex)
 * ================================================================ */
static void test_vhdl_bubble_sort(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("bubble_sort.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read bubble_sort.hex");

    qsim_session_t *sess = create_vhdl_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 5000);
    mu_assert(hc >= 0, "bubble_sort: didn't halt");

    char expect[33];
    val_to_lsb(expect, 3024457992u);
    mu_assert(check_reg_str(sess, "reg_x10", expect), "x10 = 3024457992");

    printf("    vhdl_bubble_sort: x10=3024457992 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Registration
 * ================================================================ */
void register_rv32i_vhdl_tests(void) {
    printf("[RV32I VHDL]\n");
    mu_run_test(test_vhdl_addi);
    mu_run_test(test_vhdl_fib10);
    mu_run_test(test_vhdl_mul);
    mu_run_test(test_vhdl_stack);
    mu_run_test(test_vhdl_bubble_sort);
    printf("\n");
}
