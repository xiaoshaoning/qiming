/* Preprocessor tests — standalone executable for focused testing.
 * Build with: cl test_verilog_preprocessor.c /I..\include /link qsim.lib
 * via cmake (see CMakeLists.txt). Run: ./Debug/test_verilog_preprocessor.exe */

#include "libqsim/verilog_preprocessor.h"
#include "libqsim/verilog_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* ── Minimal test helpers ── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static const char *current_test = "";

static int test_assert_failed = 0;

#define TEST(name) do { \
    current_test = #name; \
    tests_run++; \
    test_assert_failed = 0; \
    name(); \
    if (test_assert_failed) { \
        tests_failed++; \
    } else { \
        tests_passed++; \
        printf("  PASS: %s\n", #name); \
    } \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s: %s (line %d)\n", current_test, msg, __LINE__); \
        test_assert_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: %s: expected \"%s\", got \"%s\" (line %d)\n", \
               current_test, (b), (a), __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ── Test: `define ── */

static void test_define_simple(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, "create preprocessor");

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`define WIDTH 32\n"
        "reg [WIDTH-1:0] data;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, "process should succeed");
    ASSERT(strstr(out, "32") != NULL, "WIDTH should be substituted to 32");
    ASSERT(strstr(out, "WIDTH") == NULL, "WIDTH should not appear in output");
    free(out);
    out = NULL;

    /* Verify parse with preprocessor */
    parse_result_t r = verilog_parse("test.v", "module test;\n"
        "`define WIDTH 32\n"
        "reg [WIDTH-1:0] data;\n"
        "endmodule\n", 0);
    ASSERT(r.success == 0, "parse without preprocessor should fail (WIDTH is not a type)");

    verilog_preprocessor_destroy(pp);
}

static void test_define_no_value(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define FLAG\n"
        "module test;\n"
        "`ifdef FLAG\n"
        "wire a;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, "process should succeed");
    ASSERT(strstr(out, "wire a") != NULL, "FLAG defined, wire a should appear");
    verilog_preprocessor_destroy(pp);
    free(out);
}

static void test_define_undef(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define DEBUG\n"
        "`undef DEBUG\n"
        "`ifdef DEBUG\n"
        "wire dbg;\n"
        "`endif\n"
        "module end;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, "process should succeed");
    /* wire dbg should NOT appear since DEBUG was undefined */
    verilog_preprocessor_destroy(pp);
    free(out);
}

static void test_define_redefine(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define WIDTH 8\n"
        "`define WIDTH 16\n"
        "module test;\n"
        "reg [WIDTH-1:0] data;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "16") != NULL, "redefined WIDTH should be 16");
    free(out);
    verilog_preprocessor_destroy(pp);
}

/* ── Test: `include ── */

static void test_include_simple(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    /* Write an include file first */
    FILE *f = fopen("_pp_test_inc.v", "w");
    ASSERT(f != NULL, "create include file");
    fprintf(f, "wire inc_signal;\n");
    fclose(f);

    /* Use filename with a slash so current_dir is extracted */
    char *out = verilog_preprocessor_process(pp, "./test.v",
        "module test;\n"
        "`include \"_pp_test_inc.v\"\n"
        "endmodule\n", 0);
    if (!out) {
        printf("  Error: %s\n", verilog_preprocessor_get_error(pp));
    }
    ASSERT(out != NULL, "include should succeed");
    ASSERT(strstr(out, "inc_signal") != NULL, "include content should appear");

    free(out);
    verilog_preprocessor_destroy(pp);
    remove("_pp_test_inc.v");
}

static void test_include_not_found(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`include \"nonexistent_file.v\"\n"
        "module test;\n"
        "endmodule\n", 0);
    ASSERT(out == NULL, "include of nonexistent file should fail");

    const char *err = verilog_preprocessor_get_error(pp);
    ASSERT(err != NULL, "error message should be set");
    ASSERT(strstr(err, "not found") != NULL, "error should mention not found");

    verilog_preprocessor_destroy(pp);
}

/* ── Test: `ifdef / `ifndef / `else / `elsif / `endif ── */

static void test_ifdef_true(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define A\n"
        "module test;\n"
        "`ifdef A\n"
        "wire a_exists;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "a_exists") != NULL, "wire should appear when A defined");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_false(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`ifdef UNDEFINED\n"
        "wire hidden;\n"
        "`endif\n"
        "wire visible;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "hidden") == NULL, "wire should NOT appear");
    ASSERT(strstr(out, "visible") != NULL, "wire visible should appear");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_else(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`ifdef UNDEFINED\n"
        "wire taken;\n"
        "`else\n"
        "wire alternate;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "taken") == NULL, "if-branch should be hidden");
    ASSERT(strstr(out, "alternate") != NULL, "else-branch should appear");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifndef_true(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`ifndef NOTDEFINED\n"
        "wire created;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "created") != NULL, "wire should appear when NOTDEFINED is not defined");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_elsif(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define A\n"
        "module test;\n"
        "`ifdef A\n"
        "wire first;\n"
        "`elsif B\n"
        "wire second;\n"
        "`else\n"
        "wire third;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "first") != NULL, "first branch should be taken");
    ASSERT(strstr(out, "second") == NULL, "second branch hidden");
    ASSERT(strstr(out, "third") == NULL, "third branch hidden");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_elsif_second(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define B\n"
        "module test;\n"
        "`ifdef A\n"
        "wire first;\n"
        "`elsif B\n"
        "wire second;\n"
        "`else\n"
        "wire third;\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "first") == NULL, "first branch hidden");
    ASSERT(strstr(out, "second") != NULL, "second branch should be taken");
    ASSERT(strstr(out, "third") == NULL, "third branch hidden");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_nested(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define OUTER\n"
        "`define INNER\n"
        "module test;\n"
        "`ifdef OUTER\n"
        "wire outer;\n"
        "`ifdef INNER\n"
        "wire inner;\n"
        "`endif\n"
        "`endif\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "outer") != NULL, "outer should appear");
    ASSERT(strstr(out, "inner") != NULL, "inner should appear");
    free(out);
    verilog_preprocessor_destroy(pp);
}

static void test_ifdef_unterminated(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`ifdef A\n"
        "wire broken;\n", 0);
    ASSERT(out == NULL, "unterminated ifdef should fail");
    const char *err = verilog_preprocessor_get_error(pp);
    ASSERT(err != NULL, NULL);
    ASSERT(strstr(err, "unterminated") != NULL, "error should mention unterminated");
    verilog_preprocessor_destroy(pp);
}

/* ── Test: `timescale ── */

static void test_timescale_preserved(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`timescale 1 ns / 1 ps\n"
        "module test;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    /* timescale should be preserved as comment */
    ASSERT(strstr(out, "1 ns") != NULL, "timescale should be preserved");
    free(out);
    verilog_preprocessor_destroy(pp);
}

/* ── Test: macro substitution ── */

static void test_macro_subst_basic(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "`define W 32\n"
        "module test;\n"
        "reg [W-1:0] data;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    /* W should be replaced with 32 */
    ASSERT(strstr(out, "32-1") != NULL, "W should be substituted");
    free(out);
    verilog_preprocessor_destroy(pp);
}

/* ── Test: include path support ── */

static void test_include_path_search(void)
{
    /* Create include file in a subdirectory */
    system("mkdir -p _pp_test_dir 2>nul || mkdir _pp_test_dir 2>nul || echo ignore");
    FILE *f = fopen("_pp_test_dir/_header.v", "w");
    ASSERT(f != NULL, NULL);
    fprintf(f, "parameter VERSION = 2;\n");
    fclose(f);

    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);
    verilog_preprocessor_add_include_path(pp, "_pp_test_dir");

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "`include \"_header.v\"\n"
        "endmodule\n", 0);
    if (!out) {
        printf("  Error: %s\n", verilog_preprocessor_get_error(pp));
    }
    ASSERT(out != NULL, "include from search path should succeed");
    ASSERT(strstr(out, "VERSION") != NULL, "included content should appear");

    free(out);
    verilog_preprocessor_destroy(pp);
    remove("_pp_test_dir/_header.v");
    rmdir("_pp_test_dir");
}

/* ── Test: passthrough of non-preprocessor code ── */

static void test_passthrough(void)
{
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    ASSERT(pp != NULL, NULL);

    char *out = verilog_preprocessor_process(pp, "test.v",
        "module test;\n"
        "wire a;\n"
        "wire b;\n"
        "endmodule\n", 0);
    ASSERT(out != NULL, NULL);
    ASSERT(strstr(out, "module test") != NULL, "module preserved");
    ASSERT(strstr(out, "wire a") != NULL, "wire a preserved");
    ASSERT(strstr(out, "wire b") != NULL, "wire b preserved");
    ASSERT(strstr(out, "endmodule") != NULL, "endmodule preserved");
    free(out);
    verilog_preprocessor_destroy(pp);
}

/* ── Test: parse with preprocessor via verilog_parse_file_ex ── */

static void test_parse_file_preprocessed(void)
{
    /* Write a Verilog file that uses `define — macro value must be a plain
     * number since the PEG parser's range rule expects NUMBER COLON NUMBER. */
    FILE *f = fopen("_pp_parse_test.v", "w");
    ASSERT(f != NULL, NULL);
    fprintf(f, "`define W 4\n");
    fprintf(f, "module pp_parse_test(input clk, input [W:0] d, output reg [W:0] q);\n");
    fprintf(f, "always @(posedge clk) q <= d;\n");
    fprintf(f, "endmodule\n");
    fclose(f);

    /* Parse file with preprocessor (via verilog_parse_file_ex) */
    const char *paths[] = {"."};
    parse_result_t r = verilog_parse_file_ex("_pp_parse_test.v", paths, 1);
    ASSERT(r.success == 1, "parse with preprocessor should succeed");

    if (r.unit) uir_destroy_design_unit(r.unit);
    remove("_pp_parse_test.v");
}

/* ── Main ── */

int main(void)
{
    printf("Verilog Preprocessor Test Suite\n");
    printf("========================================\n\n");

    /* Basic `define */
    printf("[Define]\n");
    TEST(test_define_simple);
    TEST(test_define_no_value);
    TEST(test_define_undef);
    TEST(test_define_redefine);

    /* `include */
    printf("\n[Include]\n");
    TEST(test_include_simple);
    TEST(test_include_not_found);
    TEST(test_include_path_search);

    /* Conditionals */
    printf("\n[Conditionals]\n");
    TEST(test_ifdef_true);
    TEST(test_ifdef_false);
    TEST(test_ifdef_else);
    TEST(test_ifndef_true);
    TEST(test_ifdef_elsif);
    TEST(test_ifdef_elsif_second);
    TEST(test_ifdef_nested);
    TEST(test_ifdef_unterminated);

    /* Other directives */
    printf("\n[Other Directives]\n");
    TEST(test_timescale_preserved);

    /* Macro substitution */
    printf("\n[Macro Substitution]\n");
    TEST(test_macro_subst_basic);

    /* Integration */
    printf("\n[Integration]\n");
    TEST(test_passthrough);
    TEST(test_parse_file_preprocessed);

    printf("\n========================================\n");
    printf("Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
