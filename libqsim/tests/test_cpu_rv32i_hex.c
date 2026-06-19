/* RV32I tests loading hex programs from assembly/C compiled output.
 * Each test reads a .hex file (one 32-bit hex word per line), loads it
 * into the RV32I CPU simulator, runs to halt, and checks x10 (a0).
 * Supports both assembly-generated and C-compiled hex files. */
#include "minunit.h"
#include "libqsim/session.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ================================================================
 * Hex file loader
 * ================================================================ */

/* Read a hex file into a uint32_t array (up to max_words).
 * Returns number of words read, or 0 on failure. */
static size_t read_hex_file(const char *path, uint32_t *words, size_t max_words) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t count = 0;
    char line[64];
    while (count < max_words && fgets(line, sizeof(line), f)) {
        /* Skip blank lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        /* Parse hex word �� trim whitespace/newline */
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' ')) len--;
        p[len] = '\0';
        unsigned long v = strtoul(p, NULL, 16);
        words[count++] = (uint32_t)v;
    }
    fclose(f);
    return count;
}

/* ================================================================
 * IMEM string builder (same as test_cpu_rv32i.c)
 * ================================================================ */

/* IMEM: 1024 words x 32 bits = 32768 bits */
#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

static void build_imem_str32(char *out, const uint32_t *program, size_t count) {
    for (int i = 0; i < IMEM_WORDS; i++) {
        uint32_t val = ((size_t)i < count) ? program[i] : 0x00100073;  /* EBREAK */
        for (int b = 0; b < 32; b++) {
            int pos = IMEM_BITS - 1 - (i * 32 + b);
            out[pos] = ((val >> b) & 1) ? '1' : '0';
        }
    }
    out[IMEM_BITS] = '\0';
}

/* ================================================================
 * Helpers
 * ================================================================ */

static size_t read_rv32i_verilog(size_t *out_len, char **out) {
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

static qsim_session_t *create_session(void) {
    size_t src_len;
    char *src;
    if (!read_rv32i_verilog(&src_len, &src)) return NULL;
    qsim_session_t *sess = qsim_session_create();
    if (!sess) { free(src); return NULL; }
    int ok = qsim_session_compile_string(sess, "rv32i_top.v", src);
    free(src);
    if (!ok) { qsim_session_free(sess); return NULL; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { qsim_session_free(sess); return NULL; }
    return sess;
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
    int ok = qsim_session_force_str(sess, "imem", imem_str);
    if (!ok) return -1;
    session_reset(sess);
    for (int cycle = 0; cycle < max_cycles; cycle++) {
        char *h = session_clock_cycle(sess);
        if (h && h[0] == '1') { free(h); return cycle; }
        free(h);
    }
    return -2;  /* didn't halt */
}

static int check_reg_str(qsim_session_t *sess, const char *reg,
                         const char *expected) {
    char *val = qsim_session_eval_str(sess, reg);
    if (!val) return 0;
    int match = strcmp(val, expected) == 0;
    free(val);
    return match;
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

/* LSB-first string for a 32-bit value */
static void val_to_lsb(char *out, uint32_t val) {
    for (int b = 0; b < 32; b++)
        out[b] = ((val >> b) & 1) ? '1' : '0';
    out[32] = '\0';
}

static uint32_t lsbfirst_to_u32(const char *s) {
    uint32_t val = 0;
    for (int i = 0; i < 32 && s[i]; i++)
        if (s[i] == '1') val |= (1u << i);
    return val;
}

/* Read one 32-bit word from dmem at array index idx (0..255).
 * Returns 0 on failure, 1 on success. */
static int read_dmem_word(qsim_session_t *sess, int idx, uint32_t *val) {
    char *s = qsim_session_eval_str(sess, "dmem");
    if (!s) return 0;
    uint32_t v = 0;
    for (int b = 0; b < 32; b++)
        if (s[idx * 32 + b] == '1') v |= (1u << b);
    *val = v;
    free(s);
    return 1;
}

/* ================================================================
 * Test: ADDI x10, x0, 42 via hex file  =>  x10 = 42
 * ================================================================ */
static void test_hex_addi(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("addi_test.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read addi_test.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 30);
    mu_assert(hc >= 0, "addi: didn't halt");

    /* Expected: x10 = 42 = 0x2A, LSB-first 32 bits */
    char expect[33];
    val_to_lsb(expect, 42);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 42");
    printf("    hex_addi: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Fibonacci(10) = 55
 * ================================================================ */
static void test_hex_fib10(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("fib.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read fib.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 200);
    mu_assert(hc >= 0, "fib: didn't halt");

    char expect[33];
    val_to_lsb(expect, 55);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 55");
    printf("    hex_fib10: x10=55 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Multiply 7 * 6 = 42
 * ================================================================ */
static void test_hex_mul(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("multiply.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read multiply.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 20);
    mu_assert(hc >= 0, "mul: didn't halt");

    char expect[33];
    val_to_lsb(expect, 42);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 42");
    printf("    hex_mul: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Stack-based C program (volatile locals via sp-relative LW/SW)
 * ================================================================ */
static void test_hex_stack(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("stack_test.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read stack_test.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, words, n);
    qsim_session_force_str(sess, "imem", imem_str);
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
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 42");
    printf("    hex_stack: x10=42 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Bubble sort of 16 elements via C-compiled hex
 * Verifies each element via DMEM dump, plus hash checksum.
 * ================================================================ */
static void test_hex_bubble_sort(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("bubble_sort.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read bubble_sort.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 5000);
    mu_assert(hc >= 0, "bubble_sort: didn't halt");

    /* Verify hash checksum */
    char expect[33];
    val_to_lsb(expect, 3024457992u);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 3024457992 (0xB4459108)");

    /* Read and verify each sorted element from DMEM dump */
    printf("    bubble_sort sorted: ");
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        mu_assert(read_dmem_word(sess, i, &v), "bubble_sort: read dmem word");
        printf("%s%d", i ? " " : "", v);
        mu_assert(v == (uint32_t)(i + 1), "bubble_sort: element value");
    }
    printf("\n    hex_bubble_sort: x10=3024457992 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Recursive quicksort of 16 elements via C-compiled hex
 * Verifies function calls (JAL/JALR), stack save/restore, recursion.
 * Verifies each element via DMEM dump, plus hash checksum.
 * ================================================================ */
static void test_hex_qsort(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("qsort.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read qsort.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 5000);
    mu_assert(hc >= 0, "qsort: didn't halt");

    /* Verify hash checksum */
    char expect[33];
    val_to_lsb(expect, 3024457992u);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 3024457992 (0xB4459108)");

    /* Read and verify each sorted element from DMEM dump */
    printf("    qsort sorted: ");
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        mu_assert(read_dmem_word(sess, i, &v), "qsort: read dmem word");
        printf("%s%d", i ? " " : "", v);
        mu_assert(v == (uint32_t)(i + 1), "qsort: element value");
    }
    printf("\n    hex_qsort: x10=3024457992 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: CRC16-CCITT computation via C-compiled hex
 * Verifies bitwise XOR/AND/shift, unsigned short masking, loops.
 * CRC16 of test vector = 0xD99A = 55706.
 * Reads back input data and CRC result from DMEM dump.
 * ================================================================ */
static void test_hex_crc16(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("crc16.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read crc16.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 5000);
    mu_assert(hc >= 0, "crc16: didn't halt");

    /* Verify hash checksum */
    char expect[33];
    val_to_lsb(expect, 55706);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 55706 (0xD99A)");

    /* Read and verify input data from DMEM dump */
    printf("    crc16 input: ");
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        mu_assert(read_dmem_word(sess, i, &v), "crc16: read data word");
        printf("%s%d", i ? " " : "", v);
        mu_assert(v == (uint32_t)(i * 0x37), "crc16: data value");
    }

    /* Read and verify CRC result from DMEM slot 16 */
    uint32_t crc_val;
    mu_assert(read_dmem_word(sess, 16, &crc_val), "crc16: read crc result");
    mu_assert(crc_val == 55706, "crc16: DMEM CRC value");

    printf("\n    hex_crc16: x10=55706 crc_dmem=55706 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: Integer division via software shift-and-subtract algorithm.
 * Verifies: SLLI/SRLI, ANDI, SLTU, subtraction, conditional branch,
 *           loop with downward counter (31..0).
 * 8 test cases with results dumped to DMEM, verified element-wise.
 * ================================================================ */
static void test_hex_divide(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("divide.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read divide.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    int hc = run_program(sess, words, n, 30000);
    mu_assert(hc >= 0, "divide: didn't halt");

    /* Expected DMEM values: pairs of (quotient, remainder) for each test */
    uint32_t inputs[] = {12345, 1000000, 999, 0, 65535, 0xDEAD, 0x10000, 0xFFFFFFFEu};
    uint32_t divisors[] = {10, 3, 999, 1234, 1, 0xFF, 0x100, 2};

    printf("    divide:");
    for (int i = 0; i < 8; i++) {
        uint32_t q_exp = inputs[i] / divisors[i];
        uint32_t r_exp = inputs[i] % divisors[i];
        uint32_t q_val, r_val;
        mu_assert(read_dmem_word(sess, i * 2, &q_val), "divide: read quot");
        mu_assert(read_dmem_word(sess, i * 2 + 1, &r_val), "divide: read rem");
        printf(" %u/%u", inputs[i], divisors[i]);
        mu_assert(q_val == q_exp, "divide: quotient mismatch");
        mu_assert(r_val == r_exp, "divide: remainder mismatch");
    }

    /* Verify hash checksum */
    uint32_t cs = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        mu_assert(read_dmem_word(sess, i, &v), "divide: read hash word");
        cs = cs * 31 + v;
    }
    char expect_cs[33];
    val_to_lsb(expect_cs, 1491939117u);
    mu_assert(check_reg_str(sess, "x10", expect_cs), "x10 = 1491939117 (0x58ED2F2D)");
    printf("hash=1491939117 cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Test: printf via memory-mapped UART
 * Verifies: UART address decode, putchar, mpaland/printf integration
 * ================================================================ */
static void test_hex_printf(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file_resolve("printf_test.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read printf_test.hex");

    qsim_session_t *sess = create_session();
    mu_assert_not_null(sess);

    /* Debug: print signal count and names */
    int nsig = qsim_session_get_signal_count(sess);
    printf("    signals: %d\n", nsig);
    for (int i = 0; i < nsig && i < 30; i++)
        printf("      [%d] %s\n", i, qsim_session_get_signal_name(sess, i));

    /* Debug: read UART signals before running */
    char *v = qsim_session_eval_str(sess, "uart_tx_valid");
    char *d = qsim_session_eval_str(sess, "uart_tx_data");
    printf("    pre-run: uart_tx_valid=%s uart_tx_data=%s\n",
           v ? v : "NULL", d ? d : "NULL");
    free(v); free(d);

    int hc = run_program(sess, words, n, 10000);
    mu_assert(hc >= 0, "printf: didn't halt");

    /* Debug: read UART signals after run */
    v = qsim_session_eval_str(sess, "uart_tx_valid");
    d = qsim_session_eval_str(sess, "uart_tx_data");
    printf("    post-run: uart_tx_valid=%s uart_tx_data=%s\n",
           v ? v : "NULL", d ? d : "NULL");
    free(v); free(d);

    /* Debug: check UART output buffer */
    const char *uart = qsim_session_get_uart_output(sess);
    printf("    uart_output=%s (len=%zu)\n", uart ? uart : "NULL", uart ? strlen(uart) : 0);

    /* Verify return value (printf returns chars written) */
    char expect[33];
    val_to_lsb(expect, 0);
    mu_assert(check_reg_str(sess, "x10", expect), "x10 = 0");

    /* Verify UART output */
    mu_assert_not_null(uart);
    const char *expected = "Hello, RV32I! x10=42\n";
    mu_assert(strcmp(uart, expected) == 0,
              "printf UART output mismatch");
    printf("    hex_printf: OK cycle=%d\n", hc);
    qsim_session_free(sess);
}

/* ================================================================
 * Registration
 * ================================================================ */
void register_rv32i_hex_tests(void) {
    printf("[RV32I HEX]\n");
    mu_run_test(test_hex_addi);
    mu_run_test(test_hex_fib10);
    mu_run_test(test_hex_mul);
    mu_run_test(test_hex_stack);
    mu_run_test(test_hex_bubble_sort);
    mu_run_test(test_hex_qsort);
    mu_run_test(test_hex_crc16);
    mu_run_test(test_hex_divide);
    mu_run_test(test_hex_printf);
    printf("\n");
}
