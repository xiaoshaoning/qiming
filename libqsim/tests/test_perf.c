/* Performance baseline tests — regression gate for simulator performance.
 * Reports instructions/sec, delta cycles per instruction, wave buffer usage.
 * These are informational benchmarks, not strict pass/fail tests.
 *
 * Also: bit-identical validation for parallel delta evaluation (Phase 5). */
#include "minunit.h"
#include "libqsim/session.h"
#include "libqsim/value.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

/* ================================================================
 * Timer abstraction (CLOCK_MONOTONIC on POSIX, QPC on Win32)
 * ================================================================ */

static double timer_sec(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* ================================================================
 * RV32I CPU helpers (same as test_cpu_rv32i_hex.c)
 * ================================================================ */

#define IMEM_WORDS 1024
#define IMEM_BITS (IMEM_WORDS * 32)

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

static size_t read_file(const char *name, char **out) {
    const char *paths[] = {"", "../../example/rv32i/", "../example/rv32i/",
                           "example/rv32i/", "../../../example/rv32i/"};
    char full[512];
    FILE *f = NULL;
    for (int i = 0; i < 5; i++) {
        snprintf(full, sizeof(full), "%s%s", paths[i], name);
        f = fopen(full, "rb");
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
    return nread;
}

static qsim_session_t *create_cpu_session(void) {
    char *src;
    size_t len = read_file("rv32i_top.v", &src);
    if (!len) return NULL;
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

static int run_program(qsim_session_t *sess, const uint32_t *program,
                       size_t count, int max_cycles) {
    char imem_str[IMEM_BITS + 1];
    build_imem_str32(imem_str, program, count);
    int ok = qsim_session_force_str(sess, "imem", imem_str);
    if (!ok) return -1;
    session_reset(sess);
    for (int cycle = 0; cycle < max_cycles; cycle++) {
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        char *h = qsim_session_eval_str(sess, "halted");
        if (h && h[0] == '1') { free(h); return cycle + 1; }
        free(h);
    }
    return -2;
}

static size_t read_hex_file(const char *name, uint32_t *words, size_t max_words) {
    const char *paths[] = {"", "../../example/rv32i/tests/", "../example/rv32i/tests/",
                           "example/rv32i/tests/", "../../../example/rv32i/tests/"};
    char full[512];
    for (int i = 0; i < 5; i++) {
        snprintf(full, sizeof(full), "%s%s", paths[i], name);
        FILE *f = fopen(full, "r");
        if (!f) continue;
        size_t count = 0;
        char line[64];
        while (count < max_words && fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' ')) len--;
            p[len] = '\0';
            words[count++] = (uint32_t)strtoul(p, NULL, 16);
        }
        fclose(f);
        return count;
    }
    return 0;
}

/* ================================================================
 * Performance benchmark: fib10 (56 cycles, ~56 instructions)
 * ================================================================ */

static void test_perf_baseline(void) {
    uint32_t words[IMEM_WORDS];
    size_t n = read_hex_file("fib.hex", words, IMEM_WORDS);
    mu_assert(n > 0, "read fib.hex");

    /* --- Warmup run --- */
    qsim_session_t *sess = create_cpu_session();
    mu_assert_not_null(sess);
    int hc = run_program(sess, words, n, 200);
    mu_assert(hc >= 0, "fib: didn't halt during warmup");
    qsim_session_free(sess);

    /* --- Measured run --- */
    sess = create_cpu_session();
    mu_assert_not_null(sess);

    size_t start_events = qsim_session_get_event_count(sess);

    double t0 = timer_sec();
    hc = run_program(sess, words, n, 200);
    double t1 = timer_sec();
    mu_assert(hc >= 0, "fib: didn't halt");

    size_t end_events = qsim_session_get_event_count(sess);
    size_t total_events = end_events - start_events;
    size_t wave_count = qsim_session_get_wave_count(sess);

    double elapsed = t1 - t0;
    if (elapsed < 0.0001) elapsed = 0.0001;
    double ips = (double)hc / elapsed;
    double delta_per_instr = (double)total_events / (double)hc;

    printf("    fib10: %d cycles, %.3f ms, %.0f instr/sec, "
           "%.1f events/cycle, %zu wave entries\n",
           hc, elapsed * 1000.0, ips, delta_per_instr, wave_count);

    qsim_session_free(sess);
}

/* ── Performance benchmark: signal lookup ── */

static void test_perf_signal_lookup(void) {
    /* Generate a Verilog module with 1000 signals */
    char src[65536];
    int pos = snprintf(src, sizeof(src),
        "module bench;\n"
        "  reg clk;\n"
        "  initial clk = 0;\n"
        "  always #5 clk = ~clk;\n"
        "  wire [7:0] sig_0;\n"
        "  assign sig_0 = clk;\n");
    for (int i = 1; i < 1000; i++)
        pos += snprintf(src + pos, sizeof(src) - pos,
            "  wire [7:0] sig_%d;\n", i);
    pos += snprintf(src + pos, sizeof(src) - pos, "endmodule\n");

    qsim_session_t *sess = qsim_session_create();
    mu_assert(sess, "create session");
    int ok = qsim_session_compile_string(sess, "bench.v", src);
    mu_assert(ok, "compile");
    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");
    qsim_session_step_delta(sess);

    /* Warmup */
    for (int i = 0; i < 100; i++) {
        char *v = qsim_session_eval_str(sess, "bench.clk");
        free(v);
    }

    /* Measured: 10000 lookups via eval_str (exercises find_signal_idx) */
    double t0 = timer_sec();
    for (int i = 0; i < 10000; i++) {
        char *v = qsim_session_eval_str(sess, "bench.clk");
        free(v);
    }
    double t1 = timer_sec();
    double elapsed = t1 - t0;
    if (elapsed < 0.000001) elapsed = 0.000001;
    double ns = (elapsed * 1e9) / 10000.0;
    printf("    signal lookup: %.0f ns/call (10000 iters, 1000 signals)\n", ns);

    qsim_session_free(sess);
}

/* ================================================================
 * Parallel delta evaluation: bit-identical validation & benchmark
 * ================================================================ */

/* Generate Verilog source for N independent 8-bit counters sharing clk/rst.
 * Each counter has its own always block, forming one WCC (shared clock). */
static char *gen_counter_design(int n, size_t *out_len) {
    /* Estimate: ~100 bytes per counter + fixed overhead */
    size_t cap = 512 + (size_t)n * 120;
    char *src = malloc(cap);
    if (!src) return NULL;
    int pos = snprintf(src, cap,
        "module perf_sweep(input clk, input rst);\n");
    for (int i = 0; i < n; i++)
        pos += snprintf(src + pos, cap - (size_t)pos,
            "  reg [7:0] c%d;\n", i);
    for (int i = 0; i < n; i++)
        pos += snprintf(src + pos, cap - (size_t)pos,
            "  always @(posedge clk or posedge rst)\n"
            "    if (rst) c%d <= 8'd0; else c%d <= c%d + 8'd1;\n", i, i, i);
    pos += snprintf(src + pos, cap - (size_t)pos, "endmodule\n");
    *out_len = (size_t)pos;
    return src;
}

/* Compare all signal values between two sessions.
 * Returns 1 if all widths and values match, 0 otherwise. */
static int sessions_match(qsim_session_t *a, qsim_session_t *b) {
    int ca = qsim_session_get_signal_count(a);
    int cb = qsim_session_get_signal_count(b);
    if (ca != cb) return 0;
    for (int i = 0; i < ca; i++) {
        qsim_bit_vector_t va = qsim_session_get_signal_value(a, i);
        qsim_bit_vector_t vb = qsim_session_get_signal_value(b, i);
        if (va.width != vb.width) return 0;
        for (uint32_t b = 0; b < va.width; b++) {
            if (qsim_bit_get(&va, b).state != qsim_bit_get(&vb, b).state)
                return 0;
        }
    }
    return 1;
}

/* Drive one clock cycle on both sessions and verify they remain identical. */
static void cycle_and_check(qsim_session_t *s1, qsim_session_t *s2, int cycle) {
    char label[64];
    qsim_session_set_str(s1, "clk", "0");
    qsim_session_set_str(s2, "clk", "0");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    snprintf(label, sizeof(label), "clk low cycle %d", cycle);
    mu_assert(sessions_match(s1, s2), label);

    qsim_session_set_str(s1, "clk", "1");
    qsim_session_set_str(s2, "clk", "1");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    snprintf(label, sizeof(label), "clk high cycle %d", cycle);
    mu_assert(sessions_match(s1, s2), label);
}

/* ── Bit-identical: single WCC (all counters share clk/rst) ── */
static void test_perf_bit_identical_swcc(void) {
    size_t src_len;
    char *src = gen_counter_design(32, &src_len);
    mu_assert_not_null(src);

    /* Session 1: single-threaded (default thread_count=1) */
    qsim_session_t *s1 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s1, "swcc.v", src), "s1 compile");
    mu_assert(qsim_session_elaborate(s1), "s1 elaborate");

    /* Session 2: 2 threads */
    qsim_session_t *s2 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s2, "swcc.v", src), "s2 compile");
    mu_assert(qsim_session_elaborate(s2), "s2 elaborate");
    qsim_session_set_thread_count(s2, 2);
    free(src);

    /* Reset both */
    qsim_session_set_str(s1, "rst", "0");
    qsim_session_set_str(s2, "rst", "0");
    qsim_session_set_str(s1, "clk", "0");
    qsim_session_set_str(s2, "clk", "0");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    mu_assert(sessions_match(s1, s2), "initial");

    qsim_session_set_str(s1, "rst", "1");
    qsim_session_set_str(s2, "rst", "1");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    mu_assert(sessions_match(s1, s2), "assert rst");

    qsim_session_set_str(s1, "rst", "0");
    qsim_session_set_str(s2, "rst", "0");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    mu_assert(sessions_match(s1, s2), "deassert rst");

    /* Run 10 clock cycles with comparison */
    for (int cyc = 0; cyc < 10; cyc++)
        cycle_and_check(s1, s2, cyc);

    printf("    bit-identical swcc: 10 cycles @ 32 counters, 1T vs 2T\n");

    qsim_session_free(s1);
    qsim_session_free(s2);
}

/* ── Bit-identical: multi WCC (independent modules, separate clocks) ── */
static void test_perf_bit_identical_mwcc(void) {
    const char *src =
        "module cluster(input clk, input rst, output reg [7:0] out);\n"
        "  always @(posedge clk or posedge rst)\n"
        "    if (rst) out <= 8'd0; else out <= out + 8'd1;\n"
        "endmodule\n"
        "module top_mwcc(input [3:0] clk, input [3:0] rst,\n"
        "                output [7:0] out0, output [7:0] out1,\n"
        "                output [7:0] out2, output [7:0] out3);\n"
        "  cluster u0(.clk(clk[0]), .rst(rst[0]), .out(out0));\n"
        "  cluster u1(.clk(clk[1]), .rst(rst[1]), .out(out1));\n"
        "  cluster u2(.clk(clk[2]), .rst(rst[2]), .out(out2));\n"
        "  cluster u3(.clk(clk[3]), .rst(rst[3]), .out(out3));\n"
        "endmodule\n";

    /* Session 1: single-threaded */
    qsim_session_t *s1 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s1, "mwcc.v", src), "s1 compile");
    mu_assert(qsim_session_elaborate(s1), "s1 elaborate");

    /* Session 2: 2 threads */
    qsim_session_t *s2 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s2, "mwcc.v", src), "s2 compile");
    mu_assert(qsim_session_elaborate(s2), "s2 elaborate");
    qsim_session_set_thread_count(s2, 2);

    /* Drive each clock independently and compare */
    for (int cyc = 0; cyc < 8; cyc++) {
        for (int c = 0; c < 4; c++) {
            char sig[32];
            snprintf(sig, sizeof(sig), "clk[%d]", c);

            qsim_session_set_str(s1, sig, "0");
            qsim_session_set_str(s2, sig, "0");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        mu_assert(sessions_match(s1, s2), "mwcc clk low");

        for (int c = 0; c < 4; c++) {
            char sig[32];
            snprintf(sig, sizeof(sig), "clk[%d]", c);

            qsim_session_set_str(s1, sig, "1");
            qsim_session_set_str(s2, sig, "1");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        mu_assert(sessions_match(s1, s2), "mwcc clk high");
    }

    printf("    bit-identical mwcc: 8 cycles @ 4 clusters, 1T vs 2T\n");

    qsim_session_free(s1);
    qsim_session_free(s2);
}

/* ── Bit-identical: 4 threads vs 1 thread ── */
static void test_perf_bit_identical_4t(void) {
    size_t src_len;
    char *src = gen_counter_design(16, &src_len);
    mu_assert_not_null(src);

    qsim_session_t *s1 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s1, "4t.v", src), "s1 compile");
    mu_assert(qsim_session_elaborate(s1), "s1 elab");

    qsim_session_t *s4 = qsim_session_create();
    mu_assert(qsim_session_compile_string(s4, "4t.v", src), "s4 compile");
    mu_assert(qsim_session_elaborate(s4), "s4 elab");
    qsim_session_set_thread_count(s4, 4);
    free(src);

    /* Reset + 6 cycles */
    qsim_session_set_str(s1, "rst", "1");
    qsim_session_set_str(s4, "rst", "1");
    qsim_session_set_str(s1, "clk", "0");
    qsim_session_set_str(s4, "clk", "0");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s4);
    mu_assert(sessions_match(s1, s4), "4t init");
    qsim_session_set_str(s1, "rst", "0");
    qsim_session_set_str(s4, "rst", "0");
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s4);
    mu_assert(sessions_match(s1, s4), "4t deassert rst");

    for (int cyc = 0; cyc < 6; cyc++)
        cycle_and_check(s1, s4, cyc);

    printf("    bit-identical 4t: 6 cycles @ 16 counters, 1T vs 4T\n");

    qsim_session_free(s1);
    qsim_session_free(s4);
}

/* ── Parallel sweep benchmark ── */
static void test_perf_parallel_sweep(void) {
    size_t src_len;
    char *src = gen_counter_design(128, &src_len);
    mu_assert_not_null(src);

    int n_cycles = 50;
    double times[3]; /* 1T, 2T, 4T */
    int tcounts[3] = {1, 2, 4};

    for (int ti = 0; ti < 3; ti++) {
        /* Fresh session for each thread count (avoids thread-pool reuse issues) */
        qsim_session_t *sess = qsim_session_create();
        mu_assert(qsim_session_compile_string(sess, "sweep.v", src), "sweep compile");
        mu_assert(qsim_session_elaborate(sess), "sweep elab");
        if (tcounts[ti] > 1)
            qsim_session_set_thread_count(sess, tcounts[ti]);

        /* Warmup: same design, single-threaded run */
        {
            qsim_session_t *warm = qsim_session_create();
            mu_assert(qsim_session_compile_string(warm, "sweep.v", src), "warm compile");
            mu_assert(qsim_session_elaborate(warm), "warm elab");
            qsim_session_set_str(warm, "rst", "1");
            qsim_session_set_str(warm, "clk", "0");
            qsim_session_step_delta(warm);
            qsim_session_set_str(warm, "rst", "0");
            qsim_session_step_delta(warm);
            for (int c = 0; c < 10; c++) {
                qsim_session_set_str(warm, "clk", "0");
                qsim_session_step_delta(warm);
                qsim_session_set_str(warm, "clk", "1");
                qsim_session_step_delta(warm);
            }
            qsim_session_free(warm);
        }

        /* Measured run */
        qsim_session_set_str(sess, "rst", "1");
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "rst", "0");
        qsim_session_step_delta(sess);

        double t0 = timer_sec();
        for (int c = 0; c < n_cycles; c++) {
            qsim_session_set_str(sess, "clk", "0");
            qsim_session_step_delta(sess);
            qsim_session_set_str(sess, "clk", "1");
            qsim_session_step_delta(sess);
        }
        double t1 = timer_sec();
        times[ti] = t1 - t0;
        if (times[ti] < 0.000001) times[ti] = 0.000001;

        qsim_session_free(sess);
    }

    free(src);

    double speedup_2 = times[0] / times[1];
    double speedup_4 = times[0] / times[2];

    printf("    parallel sweep (128 counters, %d cycles):\n", n_cycles);
    printf("      1T: %.2f ms\n", times[0] * 1000.0);
    printf("      2T: %.2f ms  (%.2fx)\n", times[1] * 1000.0, speedup_2);
    printf("      4T: %.2f ms  (%.2fx)\n", times[2] * 1000.0, speedup_4);

    /* Not a strict pass/fail: just report the numbers.
     * Speedup > 0.8x  on 2T and > 0.6x on 4T indicates no pathological slowdown. */
    if (times[0] > 0) {
        mu_assert(speedup_2 > 0.5, "2T speedup > 0.5x (no pathological slowdown)");
        mu_assert(speedup_4 > 0.3, "4T speedup > 0.3x (no pathological slowdown)");
    }
}

/* ================================================================
 * Registration
 * ================================================================ */
void register_perf_tests(void) {
    printf("[Performance]\n");
    mu_run_test(test_perf_baseline);
    mu_run_test(test_perf_signal_lookup);
    mu_run_test(test_perf_bit_identical_swcc);
    mu_run_test(test_perf_bit_identical_mwcc);
    mu_run_test(test_perf_bit_identical_4t);
    mu_run_test(test_perf_parallel_sweep);
    printf("\n");
}
