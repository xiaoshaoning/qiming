/* Regression: Verilog real type — decls, float literals, IEEE arithmetic,
 * $bitstoreal/$realtobits/$sqrt/$floor/$ceil/$rtoi/$itor, 64-bit literals. */
#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int tests = 0, fails = 0;

static double bv_double(const char *s) {
    uint64_t v = 0;
    for (size_t i = 0; s[i]; i++)
        if (s[i] == '1') v |= (1ULL << i);
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static void run_real(const char *label, const char *src, const char *sig,
                     double expect) {
    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "real.v", src);
    if (!ok) { printf("  %-12s PARSE FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("  %-12s ELAB FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    qsim_session_step_time(sess, 5);
    char *val = qsim_session_eval_str(sess, sig);
    int good = val && fabs(bv_double(val) - expect) < 1e-9;
    printf("  %-12s %s = %g (expect %g) %s\n", label, sig,
           val ? bv_double(val) : -1.0, expect, good ? "OK" : "FAIL");
    if (!good) fails++;
    tests++;
    free(val);
    qsim_session_free(sess);
}

static void run_int(const char *label, const char *src, const char *sig,
                    int64_t expect) {
    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "real.v", src);
    if (!ok) { printf("  %-12s PARSE FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("  %-12s ELAB FAILED\n", label); tests++; fails++; qsim_session_free(sess); return; }
    qsim_session_step_time(sess, 5);
    char *val = qsim_session_eval_str(sess, sig);
    int64_t got = 0;
    int good = 0;
    if (val) {
        uint64_t v = 0;
        for (size_t i = 0; val[i]; i++)
            if (val[i] == '1') v |= (1ULL << i);
        got = (int64_t)v;
        good = got == expect;
    }
    printf("  %-12s %s = %lld (expect %lld) %s\n", label, sig,
           (long long)got, (long long)expect, good ? "OK" : "FAIL");
    if (!good) fails++;
    tests++;
    free(val);
    qsim_session_free(sess);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    run_real("arith_add",
        "module t;\n real a, b, r;\n initial begin a=1.5; b=2.25; r=a+b; end\nendmodule\n",
        "r", 3.75);
    run_real("arith_mul",
        "module t;\n real a, b, r;\n initial begin a=2.0; b=3.5; r=a*b; end\nendmodule\n",
        "r", 7.0);
    run_int("compare",
        "module t;\n real a; reg f;\n initial begin a=1.5; f=(a>1.0); end\nendmodule\n",
        "f", 1);
    run_real("sqrt",
        "module t;\n real r;\n initial r=$sqrt(16.0);\nendmodule\n",
        "r", 4.0);
    run_real("floor",
        "module t;\n real r;\n initial r=$floor(2.7);\nendmodule\n",
        "r", 2.0);
    run_real("ceil",
        "module t;\n real r;\n initial r=$ceil(2.3);\nendmodule\n",
        "r", 3.0);
    run_int("rtoi_real",
        "module t;\n real a; integer j;\n initial begin a=1.5; j=$rtoi(a); end\nendmodule\n",
        "j", 1);
    run_int("rtoi_int",
        "module t;\n integer j;\n initial j=$rtoi(3);\nendmodule\n",
        "j", 3);
    run_int("itor",
        "module t;\n real r; reg [63:0] b;\n initial begin r=$itor(5); b=$realtobits(r); end\nendmodule\n",
        "b", 0x4014000000000000LL);  /* 5.0 IEEE */
    run_int("bitstoreal",
        "module t;\n real r; reg [63:0] b;\n initial begin r=$bitstoreal(64'h3FF0000000000000); b=$realtobits(r); end\nendmodule\n",
        "b", 0x3FF0000000000000LL);  /* 1.0 round trip */
    run_int("lit64",
        "module t;\n reg [63:0] b;\n initial b=64'h3FF0000000000000;\nendmodule\n",
        "b", 0x3FF0000000000000LL);  /* 64-bit literal (was truncated on Windows) */

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
