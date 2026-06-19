/* Standalone test for Phase 4 — Specify block parsing */
#include "libqsim/verilog_parser.h"
#include "libqsim/uir.h"
#include <stdio.h>
#include <string.h>

static int tests = 0, passed = 0;
#define CHECK(cond, msg) do { tests++; if (cond) { passed++; } else { printf("  FAIL: %s\n", msg); } } while(0)
#define CHECK_STR_EQ(a, b, msg) do { tests++; if (strcmp(a, b) == 0) { passed++; } else { printf("  FAIL: %s (got '%s', expected '%s')\n", msg, a, b); } } while(0)

int main(void)
{
    /* 1. Empty specify block */
    {
        const char *src =
            "module test;\n"
            "  specify\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "empty specify parse");
        CHECK(r.unit->specify_count >= 1, "has specify block");
        uir_destroy_design_unit(r.unit);
    }

    /* 2. Specparam declaration */
    {
        const char *src =
            "module test;\n"
            "  specify\n"
            "    specparam t_rise = 10, t_fall = 12;\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "specparam parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->specparam_count >= 2, "has 2 specparams");
        CHECK_STR_EQ(sp->specparams[0].hier_path, "t_rise", "first specparam name");
        CHECK_STR_EQ(sp->specparams[1].hier_path, "t_fall", "second specparam name");
        CHECK(sp->specparams[0].value != NULL, "first value not null");
        CHECK(sp->specparams[1].value != NULL, "second value not null");
        uir_destroy_design_unit(r.unit);
    }

    /* 3. Parallel path (a => b) = (rise, fall) */
    {
        const char *src =
            "module test;\n"
            "  input a; output b;\n"
            "  specify\n"
            "    (a => b) = (10, 12);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "parallel path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK_STR_EQ(sp->paths[0].src, "a", "path src");
        CHECK_STR_EQ(sp->paths[0].dst, "b", "path dst");
        CHECK(sp->paths[0].type == UIR_PATH_PARALLEL, "type parallel");
        CHECK(sp->paths[0].rise_delay != NULL, "rise delay");
        CHECK(sp->paths[0].fall_delay != NULL, "fall delay");
        uir_destroy_design_unit(r.unit);
    }

    /* 4. Full path (a *> b) = (rise, fall, z, x) */
    {
        const char *src =
            "module test;\n"
            "  input a; output b;\n"
            "  specify\n"
            "    (a *> b) = (10, 12, 15, 18);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "full path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK_STR_EQ(sp->paths[0].src, "a", "path src");
        CHECK_STR_EQ(sp->paths[0].dst, "b", "path dst");
        CHECK(sp->paths[0].type == UIR_PATH_FULL, "type full");
        CHECK(sp->paths[0].rise_delay != NULL, "rise");
        CHECK(sp->paths[0].fall_delay != NULL, "fall");
        CHECK(sp->paths[0].z_delay != NULL, "z delay");
        CHECK(sp->paths[0].x_delay != NULL, "x delay");
        uir_destroy_design_unit(r.unit);
    }

    /* 5. Multiple paths in specify block */
    {
        const char *src =
            "module test(a, b, c);\n"
            "  input a, b; output c;\n"
            "  wire c;\n"
            "  specify\n"
            "    (a => c) = (5, 7);\n"
            "    (b => c) = (6, 8);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "multi-path");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 2, "has 2 paths");
        CHECK_STR_EQ(sp->paths[0].src, "a", "first src");
        CHECK_STR_EQ(sp->paths[1].src, "b", "second src");
        uir_destroy_design_unit(r.unit);
    }

    /* 6. Conditional path: if (cond) (a => b) = (rise, fall) */
    {
        const char *src =
            "module test;\n"
            "  input a, rst; output b;\n"
            "  specify\n"
            "    if (rst) (a => b) = (3, 5);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "conditional path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK(sp->paths[0].condition != NULL, "has condition");
        uir_destroy_design_unit(r.unit);
    }

    /* 7. Edge-sensitive path: (posedge clk => q) = (3, 5) */
    {
        const char *src =
            "module test;\n"
            "  input clk; output q;\n"
            "  specify\n"
            "    (posedge clk => q) = (3, 5);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "posedge path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK(sp->paths[0].src_edge == 1, "src edge = posedge");
        CHECK_STR_EQ(sp->paths[0].src, "clk", "src is clk");
        CHECK_STR_EQ(sp->paths[0].dst, "q", "dst is q");
        uir_destroy_design_unit(r.unit);
    }

    /* 8. Edge-sensitive path: (negedge clk => q) = (2, 4) */
    {
        const char *src =
            "module test;\n"
            "  input clk; output q;\n"
            "  specify\n"
            "    (negedge clk => q) = (2, 4);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "negedge path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK(sp->paths[0].src_edge == -1, "src edge = negedge");
        uir_destroy_design_unit(r.unit);
    }

    /* 9. Edge path with polarity: (posedge clk => q +:) = (3, 5) */
    {
        const char *src =
            "module test;\n"
            "  input clk; output q;\n"
            "  specify\n"
            "    (posedge clk => q +:) = (3, 5);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "polarity path parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->path_count >= 1, "has path");
        CHECK(sp->paths[0].src_edge == 1, "posedge");
        CHECK(sp->paths[0].dst_polarity == 1, "polarity +:");
        uir_destroy_design_unit(r.unit);
    }

    /* 10. $setup timing check */
    {
        const char *src =
            "module test;\n"
            "  input d, clk;\n"
            "  specify\n"
            "    $setup(d, clk, 10);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "$setup parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->timing_check_count >= 1, "has timing check");
        CHECK(sp->timing_checks[0].kind == UIR_TIMING_SETUP, "kind setup");
        CHECK_STR_EQ(sp->timing_checks[0].data_pin, "d", "data pin");
        CHECK_STR_EQ(sp->timing_checks[0].ref_pin, "clk", "ref pin");
        CHECK(sp->timing_checks[0].limit != NULL, "has limit");
        uir_destroy_design_unit(r.unit);
    }

    /* 11. $hold timing check */
    {
        const char *src =
            "module test;\n"
            "  input d, clk;\n"
            "  specify\n"
            "    $hold(clk, d, 5);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "$hold parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->timing_check_count >= 1, "has timing check");
        CHECK(sp->timing_checks[0].kind == UIR_TIMING_HOLD, "kind hold");
        CHECK_STR_EQ(sp->timing_checks[0].ref_pin, "clk", "ref pin");
        CHECK_STR_EQ(sp->timing_checks[0].data_pin, "d", "data pin");
        uir_destroy_design_unit(r.unit);
    }

    /* 12. $width timing check */
    {
        const char *src =
            "module test;\n"
            "  input clk;\n"
            "  specify\n"
            "    $width(clk, 10);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "$width parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->timing_check_count >= 1, "has timing check");
        CHECK(sp->timing_checks[0].kind == UIR_TIMING_WIDTH, "kind width");
        CHECK_STR_EQ(sp->timing_checks[0].ref_pin, "clk", "ref pin");
        uir_destroy_design_unit(r.unit);
    }

    /* 13. $period timing check */
    {
        const char *src =
            "module test;\n"
            "  input clk;\n"
            "  specify\n"
            "    $period(clk, 20);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "$period parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->timing_check_count >= 1, "has timing check");
        CHECK(sp->timing_checks[0].kind == UIR_TIMING_PERIOD, "kind period");
        CHECK_STR_EQ(sp->timing_checks[0].ref_pin, "clk", "ref pin");
        uir_destroy_design_unit(r.unit);
    }

    /* 14. $setuphold timing check */
    {
        const char *src =
            "module test;\n"
            "  input d, clk;\n"
            "  specify\n"
            "    $setuphold(clk, d, 10, 5);\n"
            "  endspecify\n"
            "endmodule\n";
        parse_result_t r = verilog_parse("test.v", src, strlen(src));
        CHECK(r.success, "$setuphold parse");
        CHECK(r.unit->specify_count >= 1, "has specify");
        uir_specify_t *sp = r.unit->specifies[0];
        CHECK(sp->timing_check_count >= 1, "has timing check");
        CHECK(sp->timing_checks[0].kind == UIR_TIMING_SETUPHOLD, "kind setuphold");
        CHECK_STR_EQ(sp->timing_checks[0].ref_pin, "clk", "ref pin");
        CHECK_STR_EQ(sp->timing_checks[0].data_pin, "d", "data pin");
        uir_destroy_design_unit(r.unit);
    }

    printf("Specify-parse: %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
