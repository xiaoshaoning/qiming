#include "libqsim/session.h"
#include "libqsim/uir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define mu_assert(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while(0)

#define mu_run_test(test) do { \
    fprintf(stdout, "  " #test "... "); \
    fflush(stdout); \
    int ret = test(); \
    if (ret) { \
        fprintf(stdout, "FAIL\n"); \
        failures++; \
    } else { \
        fprintf(stdout, "PASS\n"); \
    } \
} while(0)

static int failures = 0;

/* ── helpers ── */

static char *gen_counter_design(int n, size_t *out_len) {
    /* Buffer: header(~50) + reg lines(~25 each) + always lines(~90 each) + footer */
    size_t cap = 256 + (size_t)n * 120;
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

/* Track step/cycle context for debug output */
static int debug_cycle = -1;
static int debug_step = 0;

static int sessions_match(qsim_session_t *a, qsim_session_t *b) {
    int ca = qsim_session_get_signal_count(a);
    int cb = qsim_session_get_signal_count(b);
    if (ca != cb) {
        fprintf(stderr, "  [DIV] cycle=%d step=%d: signal count mismatch %d vs %d\n",
                debug_cycle, debug_step, ca, cb);
        return 0;
    }
    for (int i = 0; i < ca; i++) {
        qsim_bit_vector_t va = qsim_session_get_signal_value(a, i);
        qsim_bit_vector_t vb = qsim_session_get_signal_value(b, i);
        if (va.width != vb.width) {
            fprintf(stderr, "  [DIV] cycle=%d step=%d sig[%d] %s: width %d vs %d\n",
                    debug_cycle, debug_step, i, qsim_session_get_signal_name(a, i),
                    va.width, vb.width);
            return 0;
        }
        for (uint32_t bit = 0; bit < va.width; bit++) {
            if (qsim_bit_get(&va, bit).state != qsim_bit_get(&vb, bit).state) {
                fprintf(stderr, "  [DIV] cycle=%d step=%d sig[%d] %s bit %d: "
                        "s1=%d s2=%d\n",
                        debug_cycle, debug_step, i, qsim_session_get_signal_name(a, i),
                        bit,
                        (int)qsim_bit_get(&va, bit).state,
                        (int)qsim_bit_get(&vb, bit).state);
                return 0;
            }
        }
    }
    return 1;
}

static int cycle(qsim_session_t *s, int do_rst) {
    if (do_rst) {
        if (!qsim_session_set_str(s, "rst", "1")) return 0;
        if (!qsim_session_set_str(s, "clk", "0")) return 0;
        qsim_session_step_delta(s);
        qsim_session_set_str(s, "rst", "0");
    } else {
        if (!qsim_session_set_str(s, "clk", "0")) return 0;
        qsim_session_step_delta(s);
    }
    if (!qsim_session_set_str(s, "clk", "1")) return 0;
    qsim_session_step_delta(s);
    if (!qsim_session_set_str(s, "clk", "0")) return 0;
    qsim_session_step_delta(s);
    return 1;
}

static int cycle_pair_check(qsim_session_t *s1, qsim_session_t *s2, int do_rst) {
    if (do_rst) {
        if (!qsim_session_set_str(s1, "rst", "1")) return 0;
        if (!qsim_session_set_str(s2, "rst", "1")) return 0;
        if (!qsim_session_set_str(s1, "clk", "0")) return 0;
        if (!qsim_session_set_str(s2, "clk", "0")) return 0;
        debug_step = 0;
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        if (!sessions_match(s1, s2)) return 0;
        if (!qsim_session_set_str(s1, "rst", "0")) return 0;
        if (!qsim_session_set_str(s2, "rst", "0")) return 0;
    } else {
        if (!qsim_session_set_str(s1, "clk", "0")) return 0;
        if (!qsim_session_set_str(s2, "clk", "0")) return 0;
    }
    debug_step = 1;
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    if (!sessions_match(s1, s2)) return 0;

    if (!qsim_session_set_str(s1, "clk", "1")) return 0;
    if (!qsim_session_set_str(s2, "clk", "1")) return 0;
    debug_step = 2;
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    if (!sessions_match(s1, s2)) return 0;

    if (!qsim_session_set_str(s1, "clk", "0")) return 0;
    if (!qsim_session_set_str(s2, "clk", "0")) return 0;
    debug_step = 3;
    qsim_session_step_delta(s1);
    qsim_session_step_delta(s2);
    if (!sessions_match(s1, s2)) return 0;

    return 1;
}

/* ── Test: single WCC (32 counters sharing clk/rst), 1T vs 2T ── */

static int test_bit_identical_swcc(void) {
    size_t len;
    char *src = gen_counter_design(32, &len);
    mu_assert(src != NULL, "gen_counter_design failed");

    qsim_session_t *s1 = qsim_session_create();
    qsim_session_t *s2 = qsim_session_create();
    mu_assert(s1 && s2, "session create");

    int ok = qsim_session_compile_string(s1, "perf_sweep", src);
    ok = ok && qsim_session_compile_string(s2, "perf_sweep", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s1);
    ok = ok && qsim_session_elaborate(s2);
    mu_assert(ok, "elaborate");

    qsim_session_set_thread_count(s1, 1);
    qsim_session_set_thread_count(s2, 2);

    for (int i = 0; i < 10; i++) {
        debug_cycle = i;
        ok = cycle_pair_check(s1, s2, i == 0);
        mu_assert(ok, "cycle mismatch");
    }

    qsim_session_free(s1);
    qsim_session_free(s2);
    free(src);
    return 0;
}

/* ── Test: multi-WCC (4 independent cluster modules), 1T vs 2T ── */

static int test_bit_identical_mwcc(void) {
    /* 4 independent modules — each has its own clk and counter.
     * This creates 4 WCCs because signals don't cross module boundaries. */
    const char *src =
        "module ctr(input clk, input rst, output reg [7:0] val);\n"
        "  always @(posedge clk or posedge rst)\n"
        "    if (rst) val <= 8'd0; else val <= val + 8'd1;\n"
        "endmodule\n"
        "module top(input clk_a, input clk_b, input clk_c, input clk_d,\n"
        "            input rst, output [7:0] va, [7:0] vb, [7:0] vc, [7:0] vd);\n"
        "  ctr u1(.clk(clk_a), .rst(rst), .val(va));\n"
        "  ctr u2(.clk(clk_b), .rst(rst), .val(vb));\n"
        "  ctr u3(.clk(clk_c), .rst(rst), .val(vc));\n"
        "  ctr u4(.clk(clk_d), .rst(rst), .val(vd));\n"
        "endmodule\n";

    qsim_session_t *s1 = qsim_session_create();
    qsim_session_t *s2 = qsim_session_create();
    mu_assert(s1 && s2, "session create");

    int ok = qsim_session_compile_string(s1, "mwcc", src);
    ok = ok && qsim_session_compile_string(s2, "mwcc", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s1);
    ok = ok && qsim_session_elaborate(s2);
    mu_assert(ok, "elaborate");

    int sc1 = qsim_session_get_signal_count(s1);
    int sc2 = qsim_session_get_signal_count(s2);
    fprintf(stdout, "\n    signals: %d / %d\n", sc1, sc2);
    for (int i = 0; i < sc1 && i < 20; i++) {
        fprintf(stdout, "    [%d] %s\n", i, qsim_session_get_signal_name(s1, i));
    }

    qsim_session_set_thread_count(s1, 1);
    qsim_session_set_thread_count(s2, 2);

    /* Drive 4 independent clocks */
    const char *clks[] = {"clk_a", "clk_b", "clk_c", "clk_d"};

    for (int cycle_i = 0; cycle_i < 8; cycle_i++) {
        int do_rst = (cycle_i == 0);

        if (do_rst) {
            for (int c = 0; c < 4; c++) {
                qsim_session_set_str(s1, clks[c], "0");
                qsim_session_set_str(s2, clks[c], "0");
            }
            qsim_session_set_str(s1, "rst", "1");
            qsim_session_set_str(s2, "rst", "1");
            qsim_session_step_delta(s1);
            qsim_session_step_delta(s2);
            mu_assert(sessions_match(s1, s2), "mwcc reset phase");
            qsim_session_set_str(s1, "rst", "0");
            qsim_session_set_str(s2, "rst", "0");
        }

        /* Negedge */
        for (int c = 0; c < 4; c++) {
            qsim_session_set_str(s1, clks[c], "0");
            qsim_session_set_str(s2, clks[c], "0");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        mu_assert(sessions_match(s1, s2), "mwcc negedge mismatch");

        /* Posedge */
        for (int c = 0; c < 4; c++) {
            qsim_session_set_str(s1, clks[c], "1");
            qsim_session_set_str(s2, clks[c], "1");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        mu_assert(sessions_match(s1, s2), "mwcc posedge mismatch");
    }

    qsim_session_free(s1);
    qsim_session_free(s2);
    return 0;
}

/* ── Test: 1T vs 4T (16 counters, 6 cycles) ── */

static int test_bit_identical_4t(void) {
    size_t len;
    char *src = gen_counter_design(16, &len);
    mu_assert(src != NULL, "gen_counter_design");

    qsim_session_t *s1 = qsim_session_create();
    qsim_session_t *s4 = qsim_session_create();
    mu_assert(s1 && s4, "session create");

    int ok = qsim_session_compile_string(s1, "perf_sweep", src);
    ok = ok && qsim_session_compile_string(s4, "perf_sweep", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s1);
    ok = ok && qsim_session_elaborate(s4);
    mu_assert(ok, "elaborate");

    qsim_session_set_thread_count(s1, 1);
    qsim_session_set_thread_count(s4, 4);

    for (int i = 0; i < 6; i++) {
        ok = cycle_pair_check(s1, s4, i == 0);
        mu_assert(ok, "4t cycle mismatch");
    }

    qsim_session_free(s1);
    qsim_session_free(s4);
    free(src);
    return 0;
}

/* ── Windows high-resolution timer ── */

#ifdef _WIN32
#include <windows.h>
static double timer_sec(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double timer_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

/* ── Benchmark: sweep thread counts ── */

static int test_parallel_sweep(void) {
    size_t len;
    char *src = gen_counter_design(128, &len);
    mu_assert(src != NULL, "gen_counter_design");

    int thread_counts[] = {1, 2, 4};
    int nt = 3;
    double times[3];

    for (int t = 0; t < nt; t++) {
        qsim_session_t *sess = qsim_session_create();
        mu_assert(sess != NULL, "session create");

        int ok = qsim_session_compile_string(sess, "perf_sweep", src);
        mu_assert(ok, "compile");

        ok = qsim_session_elaborate(sess);
        mu_assert(ok, "elaborate");

        int tc = thread_counts[t];
        qsim_session_set_thread_count(sess, tc);

        /* Warmup: 5 cycles */
        for (int i = 0; i < 5; i++)
            cycle(sess, i == 0);

        /* Fresh session for measured run */
        qsim_session_free(sess);
        sess = qsim_session_create();
        mu_assert(sess != NULL, "session create");
        ok = qsim_session_compile_string(sess, "perf_sweep", src);
        mu_assert(ok, "compile");
        ok = qsim_session_elaborate(sess);
        mu_assert(ok, "elaborate");
        qsim_session_set_thread_count(sess, tc);

        /* Measured: 50 cycles */
        double t0 = timer_sec();
        for (int i = 0; i < 50; i++)
            cycle(sess, i == 0);
        double t1 = timer_sec();
        times[t] = (t1 - t0) * 1000.0;

        fprintf(stdout, "  %d thread(s): %.1f ms\n", tc, times[t]);
        fflush(stdout);

        qsim_session_free(sess);
    }

    fprintf(stdout, "  Speedup 2T/1T: %.2fx\n", times[0] / times[1]);
    fprintf(stdout, "  Speedup 4T/1T: %.2fx\n", times[0] / times[2]);

    mu_assert(times[1] < times[0] * 2.0, "2T too slow (>2x 1T)");
    mu_assert(times[2] < times[0] * 3.0, "4T too slow (>3x 1T)");

    free(src);
    return 0;
}

/* ── Single-threaded baseline test (verifies the counter design works) ── */

static int test_single_thread_baseline(void) {
    qsim_session_t *s = qsim_session_create();
    mu_assert(s != NULL, "session create");

    size_t len;
    char *src = gen_counter_design(4, &len);
    mu_assert(src != NULL, "gen_counter_design");

    int ok = qsim_session_compile_string(s, "perf_sweep", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s);
    mu_assert(ok, "elaborate");

    qsim_session_set_thread_count(s, 1);

    for (int i = 0; i < 5; i++)
        cycle(s, i == 0);

    /* Check counters incremented correctly */
    char *v0 = qsim_session_eval_str(s, "c0");
    char *v1 = qsim_session_eval_str(s, "c1");
    mu_assert(v0 != NULL, "c0 exists");
    mu_assert(v1 != NULL, "c1 exists");

    /* After 5 cycles (5 posedges after reset): c0 = 5 */
    int c0_val = 0;
    for (int b = 0; b < 8 && b < (int)strlen(v0); b++)
        if (v0[b] == '1') c0_val |= (1 << b);
    fprintf(stdout, "\n    c0 = %s (%d)\n", v0, c0_val);
    mu_assert(c0_val == 5, "c0 should be 5 after 5 cycles with reset");

    free(v0); free(v1);
    qsim_session_free(s);
    free(src);
    return 0;
}

/* ── Multi-WCC benchmark helpers ── */

static char *gen_mwcc_counter_design(int n, size_t *out_len) {
    size_t cap = 512 + (size_t)n * 120;
    char *src = malloc(cap);
    if (!src) return NULL;

    int pos = snprintf(src, cap,
        "module ctr(input clk, input rst, output reg [15:0] val);\n"
        "  always @(posedge clk or posedge rst)\n"
        "    if (rst) val <= 16'd0; else val <= val + 16'd1;\n"
        "endmodule\n"
        "module mwcc_top(\n");

    for (int i = 0; i < n; i++)
        pos += snprintf(src + pos, cap - (size_t)pos,
            "  input clk_%d, input rst_%d, output [15:0] val_%d%s\n",
            i, i, i, i < n - 1 ? "," : "");

    pos += snprintf(src + pos, cap - (size_t)pos, ");\n");

    for (int i = 0; i < n; i++)
        pos += snprintf(src + pos, cap - (size_t)pos,
            "  ctr u%d(.clk(clk_%d), .rst(rst_%d), .val(val_%d));\n",
            i, i, i, i);

    pos += snprintf(src + pos, cap - (size_t)pos, "endmodule\n");
    *out_len = (size_t)pos;
    return src;
}

static int mwcc_cycle(qsim_session_t *s, int n, int do_rst) {
    char name[32];
    if (do_rst) {
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "rst_%d", i);
            qsim_session_set_str(s, name, "1");
        }
        qsim_session_step_delta(s);
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "rst_%d", i);
            qsim_session_set_str(s, name, "0");
        }
    }

    for (int phase = 0; phase < 3; phase++) {
        int val = (phase == 1) ? 1 : 0;
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "clk_%d", i);
            qsim_session_set_str(s, name, val ? "1" : "0");
        }
        qsim_session_step_delta(s);
    }
    return 1;
}

static int mwcc_cycle_pair_check(qsim_session_t *s1, qsim_session_t *s2,
                                  int n, int do_rst) {
    char name[32];
    if (do_rst) {
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "rst_%d", i);
            qsim_session_set_str(s1, name, "1");
            qsim_session_set_str(s2, name, "1");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        if (!sessions_match(s1, s2)) return 0;
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "rst_%d", i);
            qsim_session_set_str(s1, name, "0");
            qsim_session_set_str(s2, name, "0");
        }
    }

    for (int phase = 0; phase < 3; phase++) {
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "clk_%d", i);
            qsim_session_set_str(s1, name, (phase == 1) ? "1" : "0");
            qsim_session_set_str(s2, name, (phase == 1) ? "1" : "0");
        }
        qsim_session_step_delta(s1);
        qsim_session_step_delta(s2);
        if (!sessions_match(s1, s2)) return 0;
    }
    return 1;
}

/* ── Multi-WCC tests ── */

static int test_mwcc_bit_identical_2t(void) {
    int n_ctrs = 8;
    size_t len;
    char *src = gen_mwcc_counter_design(n_ctrs, &len);
    mu_assert(src != NULL, "gen_mwcc_counter_design");

    qsim_session_t *s1 = qsim_session_create();
    qsim_session_t *s2 = qsim_session_create();
    mu_assert(s1 && s2, "session create");

    int ok = qsim_session_compile_string(s1, "mwcc_top", src);
    ok = ok && qsim_session_compile_string(s2, "mwcc_top", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s1);
    ok = ok && qsim_session_elaborate(s2);
    mu_assert(ok, "elaborate");

    qsim_session_set_thread_count(s1, 1);
    qsim_session_set_thread_count(s2, 2);

    for (int i = 0; i < 10; i++) {
        debug_cycle = i;
        ok = mwcc_cycle_pair_check(s1, s2, n_ctrs, i == 0);
        mu_assert(ok, "2t cycle mismatch");
    }

    qsim_session_free(s1);
    qsim_session_free(s2);
    free(src);
    return 0;
}

static int test_mwcc_bit_identical_4t(void) {
    int n_ctrs = 8;
    size_t len;
    char *src = gen_mwcc_counter_design(n_ctrs, &len);
    mu_assert(src != NULL, "gen_mwcc_counter_design");

    qsim_session_t *s1 = qsim_session_create();
    qsim_session_t *s4 = qsim_session_create();
    mu_assert(s1 && s4, "session create");

    int ok = qsim_session_compile_string(s1, "mwcc_top", src);
    ok = ok && qsim_session_compile_string(s4, "mwcc_top", src);
    mu_assert(ok, "compile");

    ok = qsim_session_elaborate(s1);
    ok = ok && qsim_session_elaborate(s4);
    mu_assert(ok, "elaborate");

    qsim_session_set_thread_count(s1, 1);
    qsim_session_set_thread_count(s4, 4);

    for (int i = 0; i < 10; i++) {
        ok = mwcc_cycle_pair_check(s1, s4, n_ctrs, i == 0);
        mu_assert(ok, "4t cycle mismatch");
    }

    qsim_session_free(s1);
    qsim_session_free(s4);
    free(src);
    return 0;
}

static int test_mwcc_parallel_sweep(void) {
    int n_ctrs = 64;
    size_t len;
    char *src = gen_mwcc_counter_design(n_ctrs, &len);
    mu_assert(src != NULL, "gen_mwcc_counter_design");

    int thread_counts[] = {1, 2, 4};
    int nt = 3;
    double times[3];

    for (int t = 0; t < nt; t++) {
        /* Warmup */
        qsim_session_t *ws = qsim_session_create();
        mu_assert(ws, "warmup session");
        int ok = qsim_session_compile_string(ws, "mwcc_top", src);
        mu_assert(ok, "warmup compile");
        ok = qsim_session_elaborate(ws);
        mu_assert(ok, "warmup elaborate");
        qsim_session_set_thread_count(ws, thread_counts[t]);
        for (int i = 0; i < 5; i++)
            mwcc_cycle(ws, n_ctrs, i == 0);
        qsim_session_free(ws);

        /* Measured run */
        qsim_session_t *sess = qsim_session_create();
        mu_assert(sess, "session create");
        ok = qsim_session_compile_string(sess, "mwcc_top", src);
        mu_assert(ok, "compile");
        ok = qsim_session_elaborate(sess);
        mu_assert(ok, "elaborate");
        qsim_session_set_thread_count(sess, thread_counts[t]);

        double t0 = timer_sec();
        for (int i = 0; i < 50; i++)
            mwcc_cycle(sess, n_ctrs, i == 0);
        double t1 = timer_sec();
        times[t] = (t1 - t0) * 1000.0;

        fprintf(stdout, "  %d thread(s): %.1f ms\n", thread_counts[t], times[t]);
        fflush(stdout);
        qsim_session_free(sess);
    }

    fprintf(stdout, "  Speedup 2T/1T: %.2fx\n", times[0] / times[1]);
    fprintf(stdout, "  Speedup 4T/1T: %.2fx\n", times[0] / times[2]);

    mu_assert(times[1] < times[0] * 1.5, "2T too slow (>1.5x 1T)");
    mu_assert(times[2] < times[0] * 2.5, "4T too slow (>2.5x 1T)");

    free(src);
    return 0;
}

int main(void) {
    fprintf(stdout, "Parallel delta evaluation benchmarks\n\n");

    mu_run_test(test_single_thread_baseline);
    mu_run_test(test_bit_identical_swcc);
    mu_run_test(test_bit_identical_mwcc);
    mu_run_test(test_bit_identical_4t);
    mu_run_test(test_parallel_sweep);
    mu_run_test(test_mwcc_bit_identical_2t);
    mu_run_test(test_mwcc_bit_identical_4t);
    mu_run_test(test_mwcc_parallel_sweep);

    fprintf(stdout, "\n%d failures\n", failures);
    return failures;
}
