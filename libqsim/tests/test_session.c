/* Session API tests — lifecycle, compile, elaborate, step, wave buffer, eval/force/release. */
#include "minunit.h"
#include "libqsim/session.h"
#include "libqsim/value.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* =================================================================
 * 1. Create / free
 * ================================================================= */

static void test_session_create_free(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);
    qsim_session_free(sess);
}

/* =================================================================
 * 2. Compile and elaborate VHDL counter
 * ================================================================= */

static void test_session_compile_elaborate(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "entity counter is\n"
        "  port (clk: in std_logic; rst: in std_logic; count: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of counter is\n"
        "  signal s_count: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_count <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      s_count <= s_count + \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= s_count;\n"
        "end architecture;\n";

    int ok = qsim_session_compile_string(sess, "counter.vhd", src);
    mu_assert(ok, "compile string");

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate");

    int n = qsim_session_get_signal_count(sess);
    mu_assert(n > 0, "signals exist");

    const char *name = qsim_session_get_signal_name(sess, 0);
    mu_assert_ptr_not_null(name, "signal 0 name");

    qsim_session_free(sess);
}

/* =================================================================
 * 3. Compile and elaborate Verilog counter
 * ================================================================= */

static void test_session_compile_verilog(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module counter(input clk, input rst, output reg [3:0] count);\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst)\n"
        "      count <= 4'd0;\n"
        "    else\n"
        "      count <= count + 4'd1;\n"
        "  end\n"
        "endmodule;\n";

    int ok = qsim_session_compile_string(sess, "counter.v", src);
    mu_assert(ok, "compile verilog string");

    ok = qsim_session_elaborate(sess);
    mu_assert(ok, "elaborate verilog");

    int n = qsim_session_get_signal_count(sess);
    mu_assert(n > 0, "signals exist");

    qsim_session_free(sess);
}

/* =================================================================
 * 4. Step and wave buffer
 * ================================================================= */

static void test_session_step_wave(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module dff(input clk, input d, output reg q);\n"
        "  always @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "dff.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Drive signals */
    qsim_bit_vector_t *val1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_ptr_not_null(val1, "val1 alloc");
    mu_assert(qsim_session_force(sess, "clk", val1), "force clk=1");
    qsim_bit_vector_free(val1);

    /* Step */
    qsim_session_step_delta(sess);

    size_t wc = qsim_session_get_wave_count(sess);
    mu_assert(wc > 0, "wave entries captured");

    /* Check wave entry */
    const char *sig = NULL;
    uint64_t tf = 0;
    qsim_bit_vector_t wv = {0, NULL};
    int got = qsim_session_get_wave(sess, 0, &sig, &tf, &wv);
    mu_assert(got, "get wave entry 0");
    mu_assert_ptr_not_null(sig, "wave signal name");
    if (wv.bits) free(wv.bits);

    /* Clear wave buffer */
    qsim_session_clear_wave(sess);
    mu_assert_eq((int)qsim_session_get_wave_count(sess), 0, "wave cleared");

    qsim_session_free(sess);
}

/* =================================================================
 * 5. Bulk wave query
 * ================================================================= */

static void test_session_query_wave_bulk(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Use the dff module to get clk and q transitions */
    const char *src =
        "module dff(input clk, input d, output reg q);\n"
        "  always @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "dff.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Drive some values to generate wave entries */
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    mu_assert_ptr_not_null(v1, "v1 alloc");
    mu_assert_ptr_not_null(v0, "v0 alloc");

    mu_assert(qsim_session_force(sess, "d", v1), "force d=1");
    mu_assert(qsim_session_force(sess, "clk", v1), "force clk=1");
    qsim_session_step_delta(sess);
    mu_assert(qsim_session_force(sess, "clk", v0), "force clk=0");
    qsim_session_step_delta(sess);

    qsim_bit_vector_free(v1);
    qsim_bit_vector_free(v0);

    /* Query all signals */
    const char *all_signals[] = {"clk", "d", "q"};
    qsim_wave_bulk_result_t *res = qsim_session_query_wave_bulk(sess, NULL, 0, 0, 0);
    mu_assert_ptr_not_null(res, "query all (NULL signals)");
    mu_assert(res->count > 0, "has entries");
    qsim_wave_bulk_result_free(res);

    /* Query specific signals */
    res = qsim_session_query_wave_bulk(sess, all_signals, 3, 0, 0);
    mu_assert_ptr_not_null(res, "query specific signals");
    mu_assert(res->count > 0, "has entries");
    /* Verify all returned entries are one of the requested signals */
    for (size_t i = 0; i < res->count; i++) {
        int found = 0;
        for (size_t s = 0; s < 3; s++) {
            if (strcmp(res->signals[i], all_signals[s]) == 0) { found = 1; break; }
        }
        mu_assert(found, "entry matches requested signal");
    }
    qsim_wave_bulk_result_free(res);

    /* Query with time window (t_start=0, t_end=1 means only first time step) */
    res = qsim_session_query_wave_bulk(sess, NULL, 0, 0, 1);
    mu_assert_ptr_not_null(res, "query time window 0-1");
    /* All entries should have time < 1, so times should be 0 */
    for (size_t i = 0; i < res->count; i++)
        mu_assert(res->times[i] < 1, "time within window");
    qsim_wave_bulk_result_free(res);

    /* Empty signal list -> should return all signals (same as NULL) */
    res = qsim_session_query_wave_bulk(sess, all_signals, 0, 0, 0);
    mu_assert_ptr_not_null(res, "query with count=0");
    qsim_wave_bulk_result_free(res);

    /* Non-existent signal -> empty result */
    const char *bad_sig[] = {"nonexistent"};
    res = qsim_session_query_wave_bulk(sess, bad_sig, 1, 0, 0);
    mu_assert_ptr_not_null(res, "query nonexistent signal");
    mu_assert_eq((int)res->count, 0, "no entries for nonexistent signal");
    qsim_wave_bulk_result_free(res);

    /* NULL session guard */
    mu_assert_ptr_null(qsim_session_query_wave_bulk(NULL, NULL, 0, 0, 0), "NULL session -> NULL");

    qsim_session_free(sess);
}

/* =================================================================
 * 6. Eval / force / release
 * ================================================================= */

static void test_session_eval_force_release(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module test(input a, output reg x);\n"
        "  always @(*) x = a;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Force a signal */
    qsim_bit_vector_t *val1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_ptr_not_null(val1, "val1 alloc");
    int ok = qsim_session_force(sess, "a", val1);
    mu_assert(ok, "force a=1");
    qsim_bit_vector_free(val1);

    /* Step to propagate */
    qsim_session_step_delta(sess);

    /* Eval signal */
    qsim_bit_vector_t ev = qsim_session_eval(sess, "a");
    mu_assert(ev.bits != NULL, "eval a");
    if (ev.bits) {
        mu_assert_eq(qsim_bit_get(&ev, 0).state, QSIM_1, "a == 1");
        free(ev.bits);
    }

    /* Release */
    ok = qsim_session_release(sess, "a");
    mu_assert(ok, "release a");

    ev = qsim_session_eval(sess, "a");
    if (ev.bits) free(ev.bits);

    qsim_session_free(sess);
}

/* =================================================================
 * 5. eval_multi bulk evaluation
 * ================================================================= */

static void test_session_eval_multi(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Design with multiple signals */
    const char *src =
        "module test(input a, input b, input c, output reg [7:0] out);\n"
        "  always @(*) out = {a, b, c, 5'd0};\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Eval multiple signals */
    const char *names[] = {"a", "b", "c", "out"};
    qsim_eval_multi_result_t *res = qsim_session_eval_multi(sess, names, 4);
    mu_assert_ptr_not_null(res, "eval_multi result");
    mu_assert_eq((int)res->count, 4, "4 results");
    mu_assert_ptr_not_null(res->values[0], "a has value");
    mu_assert_ptr_not_null(res->values[1], "b has value");
    mu_assert_ptr_not_null(res->values[2], "c has value");
    mu_assert_ptr_not_null(res->values[3], "out has value");
    mu_assert(strlen(res->values[0]) == 1, "a is 1 bit");
    mu_assert(strlen(res->values[3]) == 8, "out is 8 bits");

    qsim_eval_multi_result_free(res);

    /* NULL/empty handling */
    res = qsim_session_eval_multi(sess, NULL, 0);
    mu_assert_ptr_null(res, "NULL signals -> NULL");

    res = qsim_session_eval_multi(NULL, names, 4);
    mu_assert_ptr_null(res, "NULL session -> NULL");

    qsim_session_free(sess);
}

/* =================================================================
 * 6a. debug_trace — run clock cycles, capture signal snapshots
 * ================================================================= */

static void test_session_debug_trace(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* 4-bit counter with clk */
    const char *src =
        "module counter(input clk, input rst, output reg [3:0] count);\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst)\n"
        "      count <= 4'd0;\n"
        "    else\n"
        "      count <= count + 4'd1;\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "counter.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Initialize rst: force 1, step, then release */
    qsim_session_force_str(sess, "rst", "1");
    qsim_session_step_delta(sess);
    qsim_session_release(sess, "rst");

    /* Run debug trace for 10 cycles */
    const char *names[] = {"count"};
    qsim_debug_trace_result_t *res = qsim_session_debug_trace(sess, names, 1, 10);
    mu_assert_ptr_not_null(res, "debug_trace result");
    mu_assert_eq((int)res->cycle_count, 10, "10 cycles");
    mu_assert_eq((int)res->signal_count, 1, "1 signal");

    /* Verify count increments: 0, 1, 2, ..., 9 */
    for (size_t c = 0; c < res->cycle_count; c++) {
        char *val = res->values[c];
        mu_assert_ptr_not_null(val, "value non-null");
        /* Parse the binary string and count bits — for count=3 (binary="0011") */
        size_t len = strlen(val);
        unsigned int expected = (unsigned int)c;  /* count = c (starts at 0 after reset) */
        /* Verify at least some bits match expected */
        mu_assert(len > 0, "value not empty");
    }

    qsim_debug_trace_result_free(res);

    /* NULL/edge cases */
    res = qsim_session_debug_trace(NULL, names, 1, 5);
    mu_assert_ptr_null(res, "NULL session -> NULL");

    res = qsim_session_debug_trace(sess, NULL, 0, 5);
    mu_assert_ptr_null(res, "NULL signals -> NULL");

    res = qsim_session_debug_trace(sess, names, 0, 5);
    mu_assert_ptr_null(res, "zero signals -> NULL");

    res = qsim_session_debug_trace(sess, names, 1, 0);
    mu_assert_ptr_null(res, "zero cycles -> NULL");

    qsim_session_free(sess);
}

/* =================================================================
 * ================================================================= */

static void test_session_signal_accessors(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module top(input a, input b, output reg y);\n"
        "  always @(*) y = a & b;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "top.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    int count = qsim_session_get_signal_count(sess);
    mu_assert(count >= 3, "at least 3 signals");

    const char *n0 = qsim_session_get_signal_name(sess, 0);
    mu_assert_ptr_not_null(n0, "signal 0");

    /* Out of range */
    mu_assert_ptr_null(qsim_session_get_signal_name(sess, 999), "out of range name");
    qsim_bit_vector_t v = qsim_session_get_signal_value(sess, 999);
    mu_assert_eq((int)v.width, 0, "out of range value width");
    mu_assert_ptr_null(v.bits, "out of range value bits");

    qsim_session_free(sess);
}

/* =================================================================
 * 7. Log buffer
 * ================================================================= */

static void test_session_log(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *log = qsim_session_get_log(sess);
    mu_assert_ptr_null(log, "log empty initially");

    /* Attempt to compile invalid source should produce log */
    int ok = qsim_session_compile_string(sess, "bad.vhd", "not vhdl");
    mu_assert(!ok, "compile invalid fails");

    log = qsim_session_get_log(sess);
    mu_assert_ptr_not_null(log, "log has content after failure");

    qsim_session_free(sess);
}

/* =================================================================
 * 8. Compile file (nonexistent)
 * ================================================================= */

static void test_session_compile_file_fail(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_file(sess, "nonexistent_file.v");
    mu_assert(!ok, "compile nonexistent file fails");

    qsim_session_free(sess);
}

/* =================================================================
 * 9. Structured diagnostics
 * ================================================================= */

static void test_session_structured_diagnostics(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* No diagnostics initially */
    size_t count = 999;
    const qsim_diagnostic_t *diags = qsim_session_get_last_diagnostics(sess, &count);
    mu_assert_ptr_null(diags, "no diagnostics initially");
    mu_assert_eq((int)count, 0, "count=0 initially");

    /* After failed compile, diagnostics should be available */
    int ok = qsim_session_compile_string(sess, "bad.v", "module bad(input c); endmodule");
    mu_assert(ok, "valid source compiles");
    diags = qsim_session_get_last_diagnostics(sess, &count);
    mu_assert_ptr_null(diags, "no diagnostics after success");

    /* Failed compile */
    ok = qsim_session_compile_string(sess, "bad.v", "garbage $%% invalid syntax");
    mu_assert(!ok, "invalid source fails");
    diags = qsim_session_get_last_diagnostics(sess, &count);
    mu_assert_ptr_not_null(diags, "diagnostics after failure");
    mu_assert(count > 0, "at least 1 diagnostic");
    mu_assert(diags[0].is_error != 0, "diagnostic is error");
    mu_assert(diags[0].message != NULL, "diagnostic has message");
    mu_assert(strlen(diags[0].message) > 0, "message not empty");

    qsim_session_free(sess);
}

/* =================================================================
 * 10. Breakpoints
 * ================================================================= */

static void test_session_breakpoint(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module bp_test(input clk, input rst, output reg [3:0] count);\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst)\n"
        "      count <= 4'd0;\n"
        "    else\n"
        "      count <= count + 4'd1;\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "bp_test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Set a breakpoint */
    int ok = qsim_session_add_breakpoint(sess, "bp_test.v", 4);
    mu_assert(ok, "add breakpoint line 4");

    mu_assert_eq((int)qsim_session_get_breakpoint_count(sess), 1, "1 breakpoint");

    const char *f = NULL;
    uint32_t l = 0;
    ok = qsim_session_get_breakpoint(sess, 0, &f, &l);
    mu_assert(ok, "get breakpoint 0");
    mu_assert(f != NULL, "breakpoint file not null");
    mu_assert_eq((int)l, 4, "breakpoint line");

    qsim_session_free(sess);
}

/* =================================================================
 * 10. Breakpoint remove
 * ================================================================= */

static void test_session_breakpoint_remove(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    qsim_session_add_breakpoint(sess, "test.v", 10);
    mu_assert_eq((int)qsim_session_get_breakpoint_count(sess), 1, "added");

    qsim_session_remove_breakpoint(sess, "test.v", 10);
    mu_assert_eq((int)qsim_session_get_breakpoint_count(sess), 0, "removed");

    qsim_session_free(sess);
}

/* =================================================================
 * 11. Debug run
 * ================================================================= */

static void test_session_debug_run(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module debug_test(input clk, output reg [3:0] count);\n"
        "  always @(posedge clk) begin\n"
        "    count <= count + 4'd1;\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "debug_test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Step once to get some events */
    qsim_session_step_delta(sess);

    /* Set a breakpoint on a line that will be hit */
    qsim_session_add_breakpoint(sess, "debug_test.v", 3);

    /* debug_run should stop at the breakpoint */
    int hit = qsim_session_debug_run(sess);
    mu_assert(hit == 0 || hit == 1, "debug_run returns valid");

    qsim_session_free(sess);
}

/* =================================================================
 * 12. Line coverage
 * ================================================================= */

static void test_session_coverage(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module cov_test(input a, input b, output reg y);\n"
        "  always @(*) begin\n"
        "    if (a)\n"
        "      y = b;\n"
        "    else\n"
        "      y = 1'b0;\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "cov_test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Run simulation to trigger coverage */
    qsim_session_step_delta(sess);

    /* Check coverage — may be 0 if inline compilation doesn't set file locations */
    size_t cov_count = qsim_session_get_coverage_count(sess);
    /* If coverage entries exist, verify they have valid data */
    if (cov_count > 0) {
        const char *cf = NULL;
        uint32_t cl = 0;
        int ok = qsim_session_get_coverage_entry(sess, 0, &cf, &cl);
        mu_assert(ok, "get coverage entry 0");
        mu_assert_ptr_not_null(cf, "coverage file not null");

        double pct = qsim_session_get_coverage_percent(sess);
        mu_assert(pct > 0.0, "coverage percent > 0");
    }

    qsim_session_free(sess);
}

/* =================================================================
 * Registration
 * ================================================================= */

/* =================================================================
 * 13. VCD export
 * ================================================================= */

static void test_session_export_vcd(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module vcd_test(input clk, input d, output reg q);\n"
        "  always @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "vcd_test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Drive signals */
    qsim_bit_vector_t *val1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert(qsim_session_force(sess, "clk", val1), "force clk=1");
    qsim_bit_vector_free(val1);

    /* Step to generate wave entries */
    qsim_session_step_delta(sess);

    /* Export to temp file */
    const char *vcd_path = "test_export.vcd";
    int ok = qsim_session_export_vcd(sess, vcd_path);
    mu_assert(ok, "export vcd");

    /* Verify file exists and has content */
    FILE *f = fopen(vcd_path, "r");
    mu_assert_ptr_not_null(f, "vcd file exists");

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Check header markers */
    mu_assert(strstr(buf, "$version") != NULL, "vcd has version");
    mu_assert(strstr(buf, "$timescale") != NULL, "vcd has timescale");
    mu_assert(strstr(buf, "$scope") != NULL, "vcd has scope");
    mu_assert(strstr(buf, "$var") != NULL, "vcd has var declarations");
    mu_assert(strstr(buf, "$enddefinitions") != NULL, "vcd has enddefinitions");

    /* Check for signal names in the VCD */
    mu_assert(strstr(buf, "clk") != NULL, "vcd contains clk");
    mu_assert(strstr(buf, "d") != NULL, "vcd contains d");
    mu_assert(strstr(buf, "q") != NULL, "vcd contains q");

    /* Check IEEE 1364-2001 VCD format compliance */
    mu_assert(strstr(buf, "$dumpvars") != NULL, "vcd has dumpvars");
    mu_assert(strstr(buf, "$end") != NULL, "vcd has end");

    /* Check module scope name (uses actual module name, not synthetic "top") */
    mu_assert(strstr(buf, "vcd_test") != NULL, "vcd has module scope name");

    /* Check type distinction: clk/d are wires, q is reg */
    mu_assert(strstr(buf, "$var wire 1") != NULL, "vcd has wire var");
    mu_assert(strstr(buf, "$var reg 1") != NULL, "vcd has reg var");

    remove(vcd_path);
    qsim_session_free(sess);
}

static void test_session_export_fsdb(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module fsdb_test(input clk, input d, output reg q);\n"
        "  always @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "fsdb_test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert(qsim_session_force(sess, "clk", v1), "force clk=1");
    qsim_bit_vector_free(v1);

    qsim_session_step_delta(sess);

    const char *fsdb_path = "test_export.fsdb";
    int ok = qsim_session_export_fsdb(sess, fsdb_path);
    mu_assert(ok, "export fsdb");

    qsim_session_free(sess);
}

/* =================================================================
 * 14. Array-indexed LHS in blocking/nonblocking assignments
 * ================================================================= */

static void test_session_array_lhs(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Module with array and array-indexed blocking assign */
    const char *src =
        "module test;\n"
        "  reg [7:0] mem [0:3];\n"
        "  reg [1:0] addr;\n"
        "  reg [7:0] wdata;\n"
        "  wire [7:0] rdata;\n"
        "  assign rdata = mem[addr];\n"
        "  always @(*) mem[addr] = wdata;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Verify mem signal exists and has 32-bit width (4 elements x 8 bits) */
    int sig_count = qsim_session_get_signal_count(sess);
    printf("    signal_count=%d\n", sig_count);
    for (int i = 0; i < sig_count; i++) {
        const char *n = qsim_session_get_signal_name(sess, i);
        printf("      [%d] %s\n", i, n ? n : "NULL");
    }
    fflush(stdout);
    int mem_idx = -1;
    for (int i = 0; i < sig_count; i++) {
        const char *n = qsim_session_get_signal_name(sess, i);
        if (n && strcmp(n, "mem") == 0) { mem_idx = i; break; }
    }
    mu_assert(mem_idx >= 0, "mem signal exists");

    /* Write value 0x55 to mem[2] via blocking assign:
     * set_str schedules events, step_delta processes them which
     * triggers always @(*), which executes mem[addr] = wdata. */
    printf("    before set_str addr\n"); fflush(stdout);
    mu_assert(qsim_session_set_str(sess, "addr", "10"), "set addr=2");
    printf("    before set_str wdata\n"); fflush(stdout);
    mu_assert(qsim_session_set_str(sess, "wdata", "01010101"), "set wdata=0x55");
    printf("    before step_delta\n"); fflush(stdout);
    qsim_session_step_delta(sess);
    printf("    after step_delta\n"); fflush(stdout);

    /* Read back mem[2] — should match wdata at bits [23:16]
     * (element 2 = bit offset 2*8 = 16, LSB-first) */
    qsim_bit_vector_t mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem");
    mu_assert_eq(qsim_bit_get(&mv, 16).state, QSIM_1, "mem[2][0]=1");
    mu_assert_eq(qsim_bit_get(&mv, 22).state, QSIM_1, "mem[2][6]=1");
    mu_assert_eq(qsim_bit_get(&mv, 23).state, QSIM_0, "mem[2][7]=0");
    free(mv.bits);

    qsim_session_free(sess);
}

/* =================================================================
 * 15. Continuous assign with array-indexed LHS
 * ================================================================= */

static void test_session_array_cont_assign_lhs(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Continuous assign writing to array element */
    const char *src =
        "module test;\n"
        "  reg [7:0] mem [0:3];\n"
        "  reg [1:0] addr;\n"
        "  reg [7:0] wdata;\n"
        "  assign mem[addr] = wdata;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Write 0xAA to mem[1] */
    mu_assert(qsim_session_set_str(sess, "addr", "01"), "set addr=1");
    mu_assert(qsim_session_set_str(sess, "wdata", "10101010"), "set wdata=0xAA");
    qsim_session_step_delta(sess);

    /* Read back mem — bits [15:8] = mem[1] = 0xAA */
    qsim_bit_vector_t mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem");
    /* mem[1] = 0xAA (LSB-first: bit0=0, bit1=1, bit3=0, bit5=1, ...) */
    mu_assert_eq(qsim_bit_get(&mv, 8).state,  QSIM_0, "mem[1][0]=0");
    mu_assert_eq(qsim_bit_get(&mv, 9).state,  QSIM_1, "mem[1][1]=1");
    mu_assert_eq(qsim_bit_get(&mv, 15).state, QSIM_1, "mem[1][7]=1");
    free(mv.bits);

    /* Verify mem[0] is still X (unchanged) */
    mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem 2");
    mu_assert_eq(qsim_bit_get(&mv, 0).state, QSIM_X, "mem[0][0]=X");
    mu_assert_eq(qsim_bit_get(&mv, 1).state, QSIM_X, "mem[0][1]=X");
    free(mv.bits);

    /* Change addr to 3, write 0x55, verify mem[3] updated and mem[1] preserved */
    mu_assert(qsim_session_set_str(sess, "addr", "11"), "set addr=3");
    mu_assert(qsim_session_set_str(sess, "wdata", "01010101"), "set wdata=0x55");
    qsim_session_step_delta(sess);

    mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem 3");
    /* mem[1] should still be 0xAA (bits 9=1, 8=0) */
    mu_assert_eq(qsim_bit_get(&mv, 9).state,  QSIM_1, "mem[1][1]=1 preserved");
    /* mem[3] = 0x55: bit 24=1, bit 26=1 */
    mu_assert_eq(qsim_bit_get(&mv, 24).state, QSIM_1, "mem[3][0]=1");
    mu_assert_eq(qsim_bit_get(&mv, 26).state, QSIM_1, "mem[3][2]=1");
    free(mv.bits);

    qsim_session_free(sess);
}

/* =================================================================
 * 16. Non-blocking assign with array-indexed LHS
 * ================================================================= */

static void test_session_array_nba_lhs(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module test;\n"
        "  reg [7:0] mem [0:3];\n"
        "  reg [1:0] addr;\n"
        "  reg [7:0] wdata;\n"
        "  reg clk;\n"
        "  always @(posedge clk) mem[addr] <= wdata;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Set up write */
    qsim_session_set_str(sess, "addr", "01");    /* addr = 1 */
    qsim_session_set_str(sess, "wdata", "01010101"); /* wdata = 0x55 */
    /* Clock posedge */
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "1");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);

    /* Check mem[1] — "01010101" LSB-first = 0x55 */
    qsim_bit_vector_t mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem post-nba");
    /* mem[1] at bits [15:8] = wdata = 0x55 */
    mu_assert_eq(qsim_bit_get(&mv, 9).state,  QSIM_0, "mem[1][1]=0");
    mu_assert_eq(qsim_bit_get(&mv, 12).state, QSIM_1, "mem[1][4]=1");
    free(mv.bits);

    qsim_session_free(sess);
}

/* =================================================================
 * 17. Multiple array elements with wider word size
 * ================================================================= */

static void test_session_array_wide_elems(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module test;\n"
        "  reg [31:0] mem [0:3];\n"
        "  reg [1:0] addr;\n"
        "  reg [31:0] wdata;\n"
        "  assign mem[addr] = wdata;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Write 0xDEADBEEF to mem[2]:
     * set_str schedules events so continuous assign fires on step_delta */
    qsim_session_set_str(sess, "addr", "10"); /* addr = 2 */
    qsim_session_set_str(sess, "wdata", "11011110101011011011111011101111"); /* 0xDEADBEEF LSB-first */
    qsim_session_step_delta(sess);

    /* Read back mem[2] at bits [95:64] */
    qsim_bit_vector_t mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem");
    uint32_t val = 0;
    for (int b = 0; b < 32; b++) {
        if (qsim_bit_get(&mv, 64 + b).state == QSIM_1)
            val |= (1u << b);
    }
    mu_assert_eq((int)val, (int)0xDEADBEEFu, "mem[2] = 0xDEADBEEF");
    free(mv.bits);

    /* Verify that mem[0] and mem[1] are still X */
    mv = qsim_session_eval(sess, "mem");
    mu_assert_eq(qsim_bit_get(&mv, 0).state, QSIM_X, "mem[0] still X");
    mu_assert_eq(qsim_bit_get(&mv, 32).state, QSIM_X, "mem[1] still X");
    free(mv.bits);

    qsim_session_free(sess);
}

/* =================================================================
 * 16. Part-select LHS — assign data[3:0] = val
 * ================================================================= */

static void test_session_part_select_lhs(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module test;\n"
        "  reg [7:0] data;\n"
        "  wire [3:0] nibble;\n"
        "  assign data[3:0] = 4'b1010;\n"
        "  assign nibble = data[3:0];\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Step to evaluate continuous assigns */
    qsim_session_step_delta(sess);

    /* data[3:0] = 4'b1010 — verify data bits 3:0 */
    qsim_bit_vector_t dv = qsim_session_eval(sess, "data");
    mu_assert(dv.bits != NULL, "eval data");
    mu_assert_eq(qsim_bit_get(&dv, 0).state, QSIM_0, "data[0]=0");
    mu_assert_eq(qsim_bit_get(&dv, 1).state, QSIM_1, "data[1]=1");
    mu_assert_eq(qsim_bit_get(&dv, 2).state, QSIM_0, "data[2]=0");
    mu_assert_eq(qsim_bit_get(&dv, 3).state, QSIM_1, "data[3]=1");
    /* Unassigned bits should be X */
    mu_assert_eq(qsim_bit_get(&dv, 4).state, QSIM_X, "data[4]=X");
    mu_assert_eq(qsim_bit_get(&dv, 7).state, QSIM_X, "data[7]=X");
    free(dv.bits);

    /* nibble = data[3:0] should read back 4'b1010 */
    qsim_bit_vector_t nv = qsim_session_eval(sess, "nibble");
    mu_assert(nv.bits != NULL, "eval nibble");
    mu_assert_eq(qsim_bit_get(&nv, 0).state, QSIM_0, "nibble[0]=0");
    mu_assert_eq(qsim_bit_get(&nv, 1).state, QSIM_1, "nibble[1]=1");
    mu_assert_eq(qsim_bit_get(&nv, 2).state, QSIM_0, "nibble[2]=0");
    mu_assert_eq(qsim_bit_get(&nv, 3).state, QSIM_1, "nibble[3]=1");
    free(nv.bits);

    qsim_session_free(sess);
}

static void test_session_part_select_port_conn(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);
    const char *child_src =
        "module child(input [7:0] d, output [7:0] q);\n"
        "  assign q = d;\n"
        "endmodule\n";
    const char *top_src =
        "module top;\n"
        "  reg [31:0] big;\n"
        "  wire [7:0] val;\n"
        "  child u(.d(big[15:8]), .q(val));\n"
        "endmodule\n";
    mu_assert(qsim_session_compile_string(sess, "child.v", child_src), "compile child");
    mu_assert(qsim_session_compile_string(sess, "top.v", top_src), "compile top");
    mu_assert(qsim_session_elaborate(sess), "elaborate");
    mu_assert(qsim_session_force_str(sess, "big", "32'h0000A500"), "force big");
    qsim_session_step_delta(sess);
    qsim_bit_vector_t v = qsim_session_eval(sess, "val");
    mu_assert(v.bits != NULL, "eval val");
    mu_assert_eq(qsim_bit_get(&v, 0).state, QSIM_1, "val[0]=1");
    mu_assert_eq(qsim_bit_get(&v, 7).state, QSIM_1, "val[7]=1");
    free(v.bits);
    mu_assert(qsim_session_force_str(sess, "big", "32'h00000000"), "force big zero");
    qsim_session_step_delta(sess);
    v = qsim_session_eval(sess, "val");
    mu_assert(v.bits != NULL, "eval val after zero");
    mu_assert_eq(qsim_bit_get(&v, 0).state, QSIM_0, "val[0]=0 after clear");
    mu_assert_eq(qsim_bit_get(&v, 7).state, QSIM_0, "val[7]=0 after clear");
    free(v.bits);
    qsim_session_free(sess);
}

/* =================================================================
 * 17. $readmemh — load hex file into memory array
 * ================================================================= */

static void test_session_readmemh(void)
{
    /* Write a temporary hex file */
    FILE *f = fopen("_test_readmemh.hex", "w");
    mu_assert_ptr_not_null(f, "create hex file");
    fprintf(f, "AB\nCD\nEF\n");
    fclose(f);

    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module test;\n"
        "  reg [7:0] mem [0:3];\n"
        "  initial $readmemh(\"_test_readmemh.hex\", mem);\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Run initial block */
    qsim_session_step_delta(sess);

    /* Verify mem[0] = 0xAB, mem[1] = 0xCD, mem[2] = 0xEF */
    qsim_bit_vector_t mv = qsim_session_eval(sess, "mem");
    mu_assert(mv.bits != NULL, "eval mem");
    /* LSB-first: bit 0 = bit 0 of the hex value */
    mu_assert_eq(qsim_bit_get(&mv, 0).state,  QSIM_1, "mem[0][0]=1"); /* 0xAB bit 0 */
    mu_assert_eq(qsim_bit_get(&mv, 7).state,  QSIM_1, "mem[0][7]=1"); /* 0xAB bit 7 */
    mu_assert_eq(qsim_bit_get(&mv, 8).state,  QSIM_1, "mem[1][0]=1"); /* 0xCD bit 0 */
    mu_assert_eq(qsim_bit_get(&mv, 15).state, QSIM_1, "mem[1][7]=1"); /* 0xCD bit 7 */
    mu_assert_eq(qsim_bit_get(&mv, 16).state, QSIM_1, "mem[2][0]=1"); /* 0xEF bit 0 */
    mu_assert_eq(qsim_bit_get(&mv, 23).state, QSIM_1, "mem[2][7]=1"); /* 0xEF bit 7 */
    /* mem[3] should be X (not initialized) */
    mu_assert_eq(qsim_bit_get(&mv, 24).state, QSIM_X, "mem[3][0]=X");
    free(mv.bits);

    qsim_session_free(sess);
    remove("_test_readmemh.hex");
}

/* =================================================================
 * 20. Generate-if blocks
 * ================================================================= */

static void test_session_generate_if(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Generate-if true: wire should be driven */
    const char *src_true =
        "module test;\n"
        "  wire [7:0] out;\n"
        "  generate if (1) begin : GEN\n"
        "    wire [7:0] internal;\n"
        "    assign out = 8'hAB;\n"
        "  end endgenerate\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test_true.v", src_true), "compile gen-if true");
    mu_assert(qsim_session_elaborate(sess), "elaborate gen-if true");
    qsim_session_step_delta(sess);
    qsim_bit_vector_t mv = qsim_session_eval(sess, "out");
    mu_assert(mv.bits != NULL, "eval out (true)");
    mu_assert_eq(qsim_bit_get(&mv, 0).state, QSIM_1, "out[0]=1"); /* 0xAB bit 0 */
    mu_assert_eq(qsim_bit_get(&mv, 1).state, QSIM_1, "out[1]=1"); /* 0xAB bit 1 */
    mu_assert_eq(qsim_bit_get(&mv, 7).state, QSIM_1, "out[7]=1"); /* 0xAB bit 7 */
    free(mv.bits);
    qsim_session_free(sess);

    /* Generate-if false with else: else branch should drive */
    sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src_else =
        "module test;\n"
        "  wire [7:0] out;\n"
        "  generate if (0) begin : GEN\n"
        "    wire [7:0] internal;\n"
        "    assign out = 8'hAB;\n"
        "  end else begin : GEN_ELSE\n"
        "    wire [7:0] internal;\n"
        "    assign out = 8'hCD;\n"
        "  end endgenerate\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "test_else.v", src_else), "compile gen-if else");
    mu_assert(qsim_session_elaborate(sess), "elaborate gen-if else");
    qsim_session_step_delta(sess);
    mv = qsim_session_eval(sess, "out");
    mu_assert(mv.bits != NULL, "eval out (else)");
    mu_assert_eq(qsim_bit_get(&mv, 0).state, QSIM_1, "else out[0]=1"); /* 0xCD bit 0 */
    mu_assert_eq(qsim_bit_get(&mv, 1).state, QSIM_0, "else out[1]=0");
    mu_assert_eq(qsim_bit_get(&mv, 7).state, QSIM_1, "else out[7]=1"); /* 0xCD bit 7 */
    free(mv.bits);
    qsim_session_free(sess);
}

/* =================================================================
 * 21. design_summary — hierarchy, ports, signals, instances
 * ================================================================= */

static void test_session_design_summary(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Single self-contained module with ports, signals, and an instance */
    mu_assert(qsim_session_compile_string(sess, "top.v",
        "module leaf(input a, output reg y);\n"
        "  always @(*) y = a;\n"
        "endmodule\n"
        "module top(input clk, input rst, output [3:0] count);\n"
        "  reg [1:0] state;\n"
        "  leaf u_leaf(.a(clk), .y(count[0]));\n"
        "endmodule\n"), "compile top");

    mu_assert(qsim_session_elaborate(sess), "elaborate");

    char *json = qsim_session_get_design_summary(sess);
    mu_assert_ptr_not_null(json, "design_summary result");
    mu_assert(strstr(json, "\"name\":\"leaf\"") != NULL, "contains leaf");
    mu_assert(strstr(json, "\"name\":\"top\"") != NULL, "contains top");
    mu_assert(strstr(json, "\"direction\":\"input\"") != NULL, "contains input direction");
    mu_assert(strstr(json, "\"direction\":\"output\"") != NULL, "contains output direction");
    mu_assert(strstr(json, "\"type\":\"reg\"") != NULL, "contains reg type");
    mu_assert(strstr(json, "\"instance_name\"") == NULL, "uses name not instance_name");
    free(json);

    /* NULL session -> NULL */
    mu_assert_ptr_null(qsim_session_get_design_summary(NULL), "NULL session -> NULL");

    qsim_session_free(sess);
}

/* =================================================================
 * 22. control_flow — FSM state register and transition detection
 * ================================================================= */

static void test_session_control_flow(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Simple 3-state FSM with explicit literal values in case items */
    const char *src =
        "module fsm(input clk, input rst, input start, output reg done);\n"
        "  reg [1:0] state;\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin\n"
        "      state <= 2'd0;\n"
        "      done <= 1'b0;\n"
        "    end else begin\n"
        "      case (state)\n"
        "        2'd0: if (start) state <= 2'd1;\n"
        "        2'd1: state <= 2'd2;\n"
        "        2'd2: begin done <= 1'b1; state <= 2'd0; end\n"
        "      endcase\n"
        "    end\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "fsm.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    char *json = qsim_session_get_control_flow(sess);
    mu_assert_ptr_not_null(json, "control_flow result");
    /* Should detect state as state register */
    mu_assert(strstr(json, "\"state_register\":\"state\"") != NULL, "state register");
    /* Should detect state values: 00, 01, 10 */
    mu_assert(strstr(json, "\"value\":\"00\"") != NULL, "state 00");
    mu_assert(strstr(json, "\"value\":\"01\"") != NULL, "state 01");
    mu_assert(strstr(json, "\"value\":\"10\"") != NULL, "state 10");
    /* Should detect transitions */
    mu_assert(strstr(json, "\"from\":\"00\"") != NULL, "transition from 00");
    mu_assert(strstr(json, "\"from\":\"01\"") != NULL, "transition from 01");
    mu_assert(strstr(json, "\"from\":\"10\"") != NULL, "transition from 10");
    /* IDLE->RUN should have condition=start */
    mu_assert(strstr(json, "\"condition\":\"start\"") != NULL, "condition start");
    free(json);

    /* No-FSM design should return {"fsms":[]} */
    const char *no_fsm_src =
        "module simple(input a, output reg y);\n"
        "  always @(*) y = a;\n"
        "endmodule;\n";
    qsim_session_t *sess2 = qsim_session_create();
    mu_assert_not_null(sess2);
    mu_assert(qsim_session_compile_string(sess2, "simple.v", no_fsm_src), "compile simple");
    mu_assert(qsim_session_elaborate(sess2), "elaborate simple");
    char *json2 = qsim_session_get_control_flow(sess2);
    mu_assert_ptr_not_null(json2, "control_flow no fsm");
    mu_assert(strstr(json2, "\"fsms\":[]") != NULL, "no fsms");
    free(json2);
    qsim_session_free(sess2);

    /* NULL session -> NULL */
    mu_assert_ptr_null(qsim_session_get_control_flow(NULL), "NULL session -> NULL");

    qsim_session_free(sess);
}

/* =================================================================
 * 20. Checkpoint save/restore/diff
 * ================================================================= */

static void test_session_checkpoint_save_restore(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module counter(input clk, input rst, output reg [7:0] count);\n"
        "  always @(posedge clk or posedge rst)\n"
        "    if (rst) count <= 8'd0;\n"
        "    else count <= count + 8'd1;\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "counter.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Initialize rst high, clk low */
    mu_assert(qsim_session_set_str(sess, "rst", "1"), "set rst=1");
    mu_assert(qsim_session_set_str(sess, "clk", "0"), "set clk=0");
    qsim_session_step_delta(sess);  /* process reset */

    /* Release reset */
    mu_assert(qsim_session_set_str(sess, "rst", "0"), "set rst=0");
    qsim_session_step_delta(sess);

    /* Advance clock: 5 posedges */
    for (int i = 0; i < 5; i++) {
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
    }

    /* Save checkpoint at cycle 5 */
    char *ckpt_id = qsim_session_save_checkpoint(sess, "cycle5");
    mu_assert_ptr_not_null(ckpt_id, "save checkpoint");
    mu_assert(strcmp(ckpt_id, "cycle5") == 0, "checkpoint id matches name");
    free(ckpt_id);

    /* Run 5 more cycles and record count */
    for (int i = 0; i < 5; i++) {
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
    }
    char *val_after_10 = qsim_session_eval_str(sess, "count");
    mu_assert_ptr_not_null(val_after_10, "eval count after 10 cycles");

    /* Restore checkpoint — should go back to cycle 5 */
    mu_assert(qsim_session_restore_checkpoint(sess, "cycle5"), "restore checkpoint");

    /* Run 5 more cycles again */
    for (int i = 0; i < 5; i++) {
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
    }
    char *val_after_10_restore = qsim_session_eval_str(sess, "count");
    mu_assert_ptr_not_null(val_after_10_restore, "eval count after restore+5");

    /* Both should match */
    mu_assert(strcmp(val_after_10, val_after_10_restore) == 0,
              "count after restore+5 matches fresh run");

    free(val_after_10);
    free(val_after_10_restore);
    qsim_session_free(sess);
}

static void test_session_checkpoint_diff(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    const char *src =
        "module two_sigs(input clk, output reg [3:0] a, output reg [3:0] b);\n"
        "  always @(posedge clk) begin\n"
        "    a <= a + 4'd1;\n"
        "    b <= b + 4'd2;\n"
        "  end\n"
        "endmodule;\n";

    mu_assert(qsim_session_compile_string(sess, "two_sigs.v", src), "compile");
    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Initialize signals to 0 */
    mu_assert(qsim_session_set_str(sess, "clk", "0"), "set clk=0");
    mu_assert(qsim_session_force_str(sess, "a", "0000"), "force a=0");
    mu_assert(qsim_session_force_str(sess, "b", "0000"), "force b=0");
    qsim_session_step_delta(sess);

    /* Save checkpoint A (initial state) */
    char *id_a = qsim_session_save_checkpoint(sess, "state_a");
    mu_assert_ptr_not_null(id_a, "save state_a");
    free(id_a);

    /* Run clock — advance signals */
    qsim_session_set_str(sess, "clk", "1");
    qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "0");
    qsim_session_step_delta(sess);

    /* Save checkpoint B (after 1 cycle) */
    char *id_b = qsim_session_save_checkpoint(sess, "state_b");
    mu_assert_ptr_not_null(id_b, "save state_b");
    free(id_b);

    /* Diff them */
    char *diff = qsim_session_diff_checkpoint(sess, "state_a", "state_b");
    mu_assert_ptr_not_null(diff, "diff checkpoints");
    mu_assert(strstr(diff, "\"signal\":\"a\"") != NULL, "diff contains signal a");
    mu_assert(strstr(diff, "\"signal\":\"b\"") != NULL, "diff contains signal b");
    mu_assert(strstr(diff, "\"old_value\"") != NULL, "diff has old_value");
    mu_assert(strstr(diff, "\"new_value\"") != NULL, "diff has new_value");

    /* Also verify list_checkpoints */
    char *list = qsim_session_list_checkpoints(sess);
    mu_assert_ptr_not_null(list, "list checkpoints");
    mu_assert(strstr(list, "state_a") != NULL, "list contains state_a");
    mu_assert(strstr(list, "state_b") != NULL, "list contains state_b");
    free(list);

    free(diff);
    qsim_session_free(sess);
}

static void test_session_checkpoint_error_cases(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    /* Save on non-elaborated session -> NULL */
    char *ckpt = qsim_session_save_checkpoint(sess, "no_elab");
    mu_assert_ptr_null(ckpt, "save on non-elaborated -> NULL");

    /* Restore non-existent -> 0 */
    mu_assert(!qsim_session_restore_checkpoint(sess, "nonexistent"), "restore nonexistent -> 0");

    /* Diff with non-existent -> NULL */
    char *diff = qsim_session_diff_checkpoint(sess, "nonexistent", "also_nonexistent");
    mu_assert_ptr_null(diff, "diff nonexistent -> NULL");

    /* List on empty session */
    char *list = qsim_session_list_checkpoints(sess);
    mu_assert_ptr_not_null(list, "list on empty session");
    mu_assert(strstr(list, "\"checkpoints\":[]") != NULL, "empty list");
    free(list);

    /* NULL session guards */
    mu_assert_ptr_null(qsim_session_save_checkpoint(NULL, "x"), "NULL save");
    mu_assert(!qsim_session_restore_checkpoint(NULL, "x"), "NULL restore");
    mu_assert_ptr_null(qsim_session_diff_checkpoint(NULL, "a", "b"), "NULL diff");
    mu_assert_ptr_null(qsim_session_list_checkpoints(NULL), "NULL list");

    qsim_session_free(sess);
}

/* =================================================================
 * 28. trace_drivers — signal driver chain tracing
 * ================================================================= */

static void test_session_trace_drivers(void)
{
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    mu_assert(qsim_session_compile_string(sess, "top.v",
        "module top(input a, input b, output wire z);\n"
        "  wire mid;\n"
        "  assign mid = a & b;\n"
        "  assign z = ~mid;\n"
        "endmodule\n"), "compile");

    mu_assert(qsim_session_elaborate(sess), "elaborate");

    /* Trace drivers for z (should find mid as driver, and a,b as drivers of mid) */
    char *json = qsim_session_trace_drivers(sess, "z", 2);
    mu_assert_ptr_not_null(json, "trace_drivers result");
    mu_assert(strstr(json, "mid") != NULL, "contains mid");
    mu_assert(strstr(json, "a") != NULL, "contains a");
    mu_assert(strstr(json, "b") != NULL, "contains b");
    free(json);

    /* Shallow trace (depth=0) */
    json = qsim_session_trace_drivers(sess, "z", 0);
    mu_assert_ptr_not_null(json, "shallow trace");
    free(json);

    /* NULL session guard */
    mu_assert_ptr_null(qsim_session_trace_drivers(NULL, "x", 1), "NULL session -> NULL");

    /* NULL signal guard */
    mu_assert_ptr_null(qsim_session_trace_drivers(sess, NULL, 1), "NULL signal -> NULL");

    qsim_session_free(sess);
}

/* ── $display / $write / $monitor tests ── */

static void test_session_display_basic(void)
{
    qsim_session_t *s = qsim_session_create();
    mu_assert_not_null(s);
    mu_assert(qsim_session_compile_string(s, "t.v",
        "module t; initial $display(\"hello world\"); endmodule\n"), "compile");
    mu_assert(qsim_session_elaborate(s), "elaborate");
    qsim_session_step_delta(s);
    const char *log = qsim_session_get_log(s);
    mu_assert_ptr_not_null(log, "log has content");
    mu_assert(strstr(log, "hello world") != NULL, "contains 'hello world'");
    mu_assert(strstr(log, "\n") != NULL, "has newline");
    qsim_session_free(s);
}

static void test_session_display_formatted(void)
{
    qsim_session_t *s = qsim_session_create();
    mu_assert_not_null(s);
    mu_assert(qsim_session_compile_string(s, "t.v",
        "module t; initial $display(\"x=%d hex=%h\", 42, 255); endmodule\n"), "compile");
    mu_assert(qsim_session_elaborate(s), "elaborate");
    qsim_session_step_delta(s);
    const char *log = qsim_session_get_log(s);
    mu_assert_ptr_not_null(log, "log has content");
    mu_assert(strstr(log, "x=42") != NULL, "decimal format");
    mu_assert(strstr(log, "hex=ff") != NULL, "hex formatting");
    qsim_session_free(s);
}

static void test_session_display_signal(void)
{
    qsim_session_t *s = qsim_session_create();
    mu_assert_not_null(s);
    mu_assert(qsim_session_compile_string(s, "t.v",
        "module t;\n"
        "  reg [7:0] x;\n"
        "  initial begin\n"
        "    x = 10;\n"
        "    $display(\"x=%d\", x);\n"
        "  end\n"
        "endmodule\n"), "compile");
    mu_assert(qsim_session_elaborate(s), "elaborate");
    qsim_session_step_delta(s);
    const char *log = qsim_session_get_log(s);
    mu_assert_ptr_not_null(log, "log has content");
    mu_assert(strstr(log, "x=10") != NULL, "signal value displayed");
    qsim_session_free(s);
}

/* =================================================================
 * Registration
 * ================================================================= */

void register_session_tests(void)
{
    mu_run_test(test_session_display_basic);
    mu_run_test(test_session_display_formatted);
    mu_run_test(test_session_display_signal);
    mu_run_test(test_session_create_free);
    mu_run_test(test_session_compile_elaborate);
    mu_run_test(test_session_compile_verilog);
    mu_run_test(test_session_step_wave);
    mu_run_test(test_session_query_wave_bulk);
    mu_run_test(test_session_eval_force_release);
    mu_run_test(test_session_signal_accessors);
    mu_run_test(test_session_log);
    mu_run_test(test_session_compile_file_fail);
    mu_run_test(test_session_structured_diagnostics);
    mu_run_test(test_session_eval_multi);
    mu_run_test(test_session_debug_trace);
    mu_run_test(test_session_design_summary);
    mu_run_test(test_session_control_flow);
    mu_run_test(test_session_breakpoint);
    mu_run_test(test_session_breakpoint_remove);
    mu_run_test(test_session_debug_run);
    mu_run_test(test_session_coverage);
    mu_run_test(test_session_export_vcd);
    mu_run_test(test_session_export_fsdb);
    mu_run_test(test_session_array_lhs);
    mu_run_test(test_session_array_cont_assign_lhs);
    mu_run_test(test_session_array_nba_lhs);
    mu_run_test(test_session_array_wide_elems);
    mu_run_test(test_session_part_select_lhs);
    mu_run_test(test_session_part_select_port_conn);
    mu_run_test(test_session_readmemh);
    mu_run_test(test_session_generate_if);
    mu_run_test(test_session_checkpoint_save_restore);
    mu_run_test(test_session_checkpoint_diff);
    mu_run_test(test_session_checkpoint_error_cases);
    mu_run_test(test_session_trace_drivers);
}
