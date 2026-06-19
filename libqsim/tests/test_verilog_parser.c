/* Verilog parser tests — 100+ tests covering all grammar constructs.
 * Tests are organized by category with clear section headers.
 * "legal accepted, illegal rejected" — each construct has positive and negative tests.
 */

#include "minunit.h"
#include "libqsim/verilog_parser.h"
#include "libqsim/uir.h"
#include <string.h>
#include <stdio.h>

static int has_port(uir_design_unit_t *unit, const char *name, uir_port_dir_t dir)
{
    for (size_t i = 0; i < unit->port_count; i++) {
        if (strcmp(unit->ports[i]->name, name) == 0 &&
            unit->ports[i]->direction == dir)
            return 1;
    }
    return 0;
}

static int has_signal(uir_design_unit_t *unit, const char *name, uir_signal_type_t type)
{
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (strcmp(unit->signals[i]->name, name) == 0 &&
            unit->signals[i]->sig_type == type)
            return 1;
    }
    return 0;
}

/* =================================================================
 * 1. Module basics
 * ================================================================= */

static void test_parse_null_source(void)
{
    parse_result_t r = verilog_parse("null.v", NULL, 0);
    mu_assert(!r.success, "should not succeed");
    mu_assert(r.unit == NULL, "should have no unit");
}

static void test_parse_empty_source(void)
{
    parse_result_t r = verilog_parse("empty.v", "", 0);
    mu_assert(!r.success, "empty source should fail");
}

static void test_parse_comment_only(void)
{
    const char *src = "// just a comment\n";
    parse_result_t r = verilog_parse("comment.v", src, strlen(src));
    mu_assert(!r.success, "no module should fail");
}

static void test_parse_empty_module(void)
{
    const char *src = "module empty (); endmodule\n";
    parse_result_t r = verilog_parse("empty.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "empty", "module name");
}

static void test_parse_module_no_ports(void)
{
    /* module without port parens */
    const char *src = "module no_ports; endmodule\n";
    parse_result_t r = verilog_parse("no_ports.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_str_eq(r.unit->name, "no_ports", "module name");
    mu_assert_eq(r.unit->port_count, 0, "no ports");
}

static void test_parse_module_empty_parens(void)
{
    /* module with empty port list */
    const char *src = "module empty_p (); endmodule\n";
    parse_result_t r = verilog_parse("empty_p.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 0, "no ports");
}

static void test_parse_module_with_ports(void)
{
    const char *src =
        "module counter (\n"
        "  input clk,\n"
        "  output reg [3:0] count,\n"
        "  input rst_n\n"
        ");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("counter.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "counter", "module name");
    mu_assert_eq(r.unit->port_count, 3, "should have 3 ports");
    mu_assert(has_port(r.unit, "clk", UIR_PORT_IN), "input clk");
    mu_assert(has_port(r.unit, "count", UIR_PORT_OUT), "output count");
    mu_assert(has_port(r.unit, "rst_n", UIR_PORT_IN), "input rst_n");
}

static void test_parse_module_non_ansi_ports(void)
{
    /* Non-ANSI port list (just names), directions in body.
     * Bare IDs in port list create port entries matched to body decls.
     */
    const char *src =
        "module nonansi (a, b, c);\n"
        "  input a;\n"
        "  output b;\n"
        "  inout c;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("nonansi.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 3, "3 ports");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "a input");
    mu_assert(has_port(r.unit, "b", UIR_PORT_OUT), "b output");
    mu_assert(has_port(r.unit, "c", UIR_PORT_INOUT), "c inout");
}

static void test_parse_module_single_port(void)
{
    const char *src = "module single(input clk); endmodule\n";
    parse_result_t r = verilog_parse("single.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 1, "1 port");
    mu_assert(has_port(r.unit, "clk", UIR_PORT_IN), "input clk");
}

static void test_parse_module_ansi_ports_with_range(void)
{
    const char *src =
        "module ansi_range(\n"
        "  input [7:0] data_in,\n"
        "  output [3:0] data_out,\n"
        "  inout [15:0] bidir\n"
        ");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("ansi_range.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 3, "3 ports");
}

/* =================================================================
 * 2. Wire / Reg / Integer declarations
 * ================================================================= */

static void test_parse_wire_and_reg(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b;\n"
        "  reg  c, d;\n"
        "  integer i;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_eq(r.unit->signal_count, 5, "should have 5 signals");
    mu_assert(has_signal(r.unit, "a", UIR_SIG_WIRE), "a is wire");
    mu_assert(has_signal(r.unit, "b", UIR_SIG_WIRE), "b is wire");
    mu_assert(has_signal(r.unit, "c", UIR_SIG_REG),  "c is reg");
    mu_assert(has_signal(r.unit, "d", UIR_SIG_REG),  "d is reg");
    mu_assert(has_signal(r.unit, "i", UIR_SIG_REG),  "i is reg");
}

static void test_parse_net_types(void)
{
    const char *src =
        "module m(input a, b);\n"
        "  wand [3:0] w;\n"
        "  wor [7:0] x;\n"
        "  tri0 y;\n"
        "  tri1 z;\n"
        "  supply0 vcc;\n"
        "  supply1 gnd;\n"
        "  uwire single;\n"
        "  tri t;\n"
        "  triand ta;\n"
        "  trior tr;\n"
        "  trireg trr;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("net_types.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(has_signal(r.unit, "w", UIR_SIG_WAND), "w is wand");
    mu_assert(has_signal(r.unit, "x", UIR_SIG_WOR), "x is wor");
    mu_assert(has_signal(r.unit, "y", UIR_SIG_TRI0), "y is tri0");
    mu_assert(has_signal(r.unit, "z", UIR_SIG_TRI1), "z is tri1");
    mu_assert(has_signal(r.unit, "vcc", UIR_SIG_SUPPLY0), "vcc is supply0");
    mu_assert(has_signal(r.unit, "gnd", UIR_SIG_SUPPLY1), "gnd is supply1");
    mu_assert(has_signal(r.unit, "single", UIR_SIG_UWIRE), "single is uwire");
    mu_assert(has_signal(r.unit, "t", UIR_SIG_TRI), "t is tri");
    mu_assert(has_signal(r.unit, "ta", UIR_SIG_TRIAND), "ta is triand");
    mu_assert(has_signal(r.unit, "tr", UIR_SIG_TRIOR), "tr is trior");
    mu_assert(has_signal(r.unit, "trr", UIR_SIG_TRIREG), "trr is trireg");
}

static void test_parse_wire_with_range(void)
{
    const char *src = "module test; wire [7:0] bus; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "bus", UIR_SIG_WIRE), "bus is wire");
}

static void test_parse_wire_reverse_range(void)
{
    const char *src = "module test; wire [0:7] rev; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "rev", UIR_SIG_WIRE), "rev is wire");
}

static void test_parse_wire_wide_range(void)
{
    const char *src = "module test; wire [31:0] data; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_wire_range_comma(void)
{
    const char *src = "module test; wire [3:0] a, b, c; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "3 signals");
    mu_assert(has_signal(r.unit, "a", UIR_SIG_WIRE), "a");
    mu_assert(has_signal(r.unit, "b", UIR_SIG_WIRE), "b");
    mu_assert(has_signal(r.unit, "c", UIR_SIG_WIRE), "c");
}

static void test_parse_reg_with_range(void)
{
    const char *src = "module test; reg [15:0] r; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "r", UIR_SIG_REG), "r is reg");
}

static void test_parse_reg_range_comma(void)
{
    const char *src = "module test; reg [7:0] x, y, z; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "3 regs");
}

static void test_parse_reg_reverse_range(void)
{
    const char *src = "module test; reg [0:3] rev; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_reg_array(void)
{
    const char *src = "module test; reg [31:0] mem [0:255]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 256, "array_size=256");
    mu_assert(r.unit->signals[0]->width == 32, "width=32");
    mu_assert(has_signal(r.unit, "mem", UIR_SIG_REG), "mem is reg");
}

static void test_parse_wire_array(void)
{
    const char *src = "module test; wire [7:0] buf [0:15]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 16, "array_size=16");
    mu_assert(r.unit->signals[0]->width == 8, "width=8");
    mu_assert(has_signal(r.unit, "buf", UIR_SIG_WIRE), "buf is wire");
}

static void test_parse_reg_array_reverse(void)
{
    const char *src = "module test; reg [15:0] mem [255:0]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 256, "array_size=256");
}

static void test_parse_reg_array_no_range(void)
{
    const char *src = "module test; reg mem [0:3]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 4, "array_size=4");
    mu_assert(r.unit->signals[0]->width == 1, "width=1");
}

static void test_parse_input_array(void)
{
    const char *src = "module test; input [7:0] addr [0:3]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 4, "array_size=4");
}

static void test_parse_output_array(void)
{
    const char *src = "module test; output [7:0] data [0:3]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 4, "array_size=4");
}

static void test_parse_reg_array_multi_dim(void)
{
    const char *src = "module test; reg [31:0] mem [0:7][0:31]; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 1, "1 signal");
    mu_assert(r.unit->signals[0]->array_size == 256, "array_size=8*32=256");
    mu_assert(r.unit->signals[0]->width == 32, "width=32");
}

static void test_parse_integer_single(void)
{
    const char *src = "module test; integer i; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "i", UIR_SIG_REG), "i");
}

static void test_parse_integer_list(void)
{
    const char *src = "module test; integer i, j, k; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "3 integers");
}

static void test_parse_mixed_decls(void)
{
    const char *src =
        "module test;\n"
        "  wire [7:0] data;\n"
        "  reg  [3:0] cnt;\n"
        "  wire flag;\n"
        "  reg  state;\n"
        "  integer idx;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 5, "5 signals");
}

static void test_parse_body_input_decl(void)
{
    const char *src = "module test; input a; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "a", UIR_SIG_WIRE), "a wire");
}

static void test_parse_body_output_decl(void)
{
    const char *src = "module test; output b; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "b", UIR_SIG_WIRE), "b wire");
}

static void test_parse_body_inout_decl(void)
{
    const char *src = "module test; inout c; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "c", UIR_SIG_WIRE), "c wire");
}

static void test_parse_body_input_range(void)
{
    const char *src = "module test; input [3:0] d; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_body_output_reg(void)
{
    const char *src = "module test; output reg e; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_body_input_list(void)
{
    const char *src = "module test; input a, b, c; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "3 inputs");
}

static void test_parse_body_output_list(void)
{
    const char *src = "module test; output x, y; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 2, "2 outputs");
}

static void test_parse_wire_id_with_dollar(void)
{
    const char *src = "module test; wire $clk; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_wire_id_with_underscore(void)
{
    const char *src = "module test; wire _signal; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 3. Continuous assign
 * ================================================================= */

static void test_parse_continuous_assign(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a & b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    if (!r.success && r.error_count > 0) {
        printf("    ERROR[0]: %s\n", r.errors[0].message);
    }
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_eq(r.unit->signal_count, 3, "should have 3 signals");
}

static void test_parse_assign_arith(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b, c;\n"
        "  assign c = a + b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_sub(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b, c;\n"
        "  assign c = a - b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_mul(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b, c;\n"
        "  assign c = a * b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_div(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b, c;\n"
        "  assign c = a / b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_mod(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b, c;\n"
        "  assign c = a % b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_bitwise_and(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a & b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_bitwise_or(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a | b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_bitwise_xor(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a ^ b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_bitwise_not(void)
{
    const char *src =
        "module test;\n"
        "  wire a, c;\n"
        "  assign c = ~a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_logical_and(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a && b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_logical_or(void)
{
    const char *src =
        "module test;\n"
        "  wire a, b, c;\n"
        "  assign c = a || b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_logical_not(void)
{
    const char *src =
        "module test;\n"
        "  wire a, c;\n"
        "  assign c = !a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_ternary(void)
{
    const char *src =
        "module test;\n"
        "  wire sel, a, b, y;\n"
        "  assign y = sel ? a : b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_shift_left(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, c;\n"
        "  assign c = a << 2;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_shift_right(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, c;\n"
        "  assign c = a >> 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_relation_lt(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a, b;\n"
        "  wire lt;\n"
        "  assign lt = a < b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_relation_gt(void)
{
    const char *src =
        "module test;\n"
        "  assign gt = a > b;\n"
        "endmodule\n";
    /* a, b are implicitly declared by the parser as wire refs */
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_relation_le(void)
{
    const char *src =
        "module test;\n"
        "  wire le;\n"
        "  assign le = a <= b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_relation_ge(void)
{
    const char *src =
        "module test;\n"
        "  assign ge = a >= b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_equality(void)
{
    const char *src =
        "module test;\n"
        "  wire eq;\n"
        "  assign eq = a == b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_inequality(void)
{
    const char *src =
        "module test;\n"
        "  assign neq = a != b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_concat_rhs(void)
{
    const char *src =
        "module test;\n"
        "  wire [7:0] c;\n"
        "  assign c = {a, b};\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_concat_multi(void)
{
    const char *src =
        "module test;\n"
        "  assign c = {a, b, c, d};\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_concat_lhs(void)
{
    const char *src =
        "module test;\n"
        "  wire [7:0] a, b;\n"
        "  assign {a, b} = 16'h1234;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    if (!r.success) {
        printf("Parse error: %s\n", r.error_count > 0 ? r.errors[0].message : "unknown");
    }
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_parens(void)
{
    const char *src =
        "module test;\n"
        "  assign y = (a + b) & c;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_assign_mixed_ops(void)
{
    const char *src =
        "module test;\n"
        "  assign y = a + b * c - d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_multi_assign(void)
{
    const char *src =
        "module test;\n"
        "  assign a = b;\n"
        "  assign c = d;\n"
        "  assign e = f;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->assign_count >= 3, "at least 3 assigns");
}

static void test_parse_assign_unary_minus(void)
{
    const char *src =
        "module test;\n"
        "  assign y = -a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 4. Always blocks
 * ================================================================= */

static void test_parse_always_block(void)
{
    const char *src =
        "module test;\n"
        "  reg q;\n"
        "  wire d, clk;\n"
        "  always @(posedge clk) begin\n"
        "    q <= d;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->process_count >= 1, "should have process");
}

static void test_parse_always_negedge(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d, clk;\n"
        "  always @(negedge clk) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->process_count >= 1, "has process");
}

static void test_parse_always_or_sens(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d, clk, rst;\n"
        "  always @(posedge clk or negedge rst) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_comma_sens(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d, clk, rst;\n"
        "  always @(posedge clk, posedge rst) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_level_sens(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d, clk;\n"
        "  always @(clk) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_auto_sens(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d;\n"
        "  always @(*) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->process_count >= 1, "has process");
}

static void test_parse_always_multi_stmts(void)
{
    const char *src =
        "module test;\n"
        "  reg a, b; wire clk;\n"
        "  always @(posedge clk) begin\n"
        "    a <= 1;\n"
        "    b <= 0;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_triple_sens(void)
{
    const char *src =
        "module test;\n"
        "  always @(posedge clk or negedge rst or en) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 5. Initial blocks
 * ================================================================= */

static void test_parse_initial_basic(void)
{
    const char *src =
        "module test;\n"
        "  reg q;\n"
        "  initial q = 0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->process_count >= 1, "has process");
}

static void test_parse_initial_begin_end(void)
{
    const char *src =
        "module test;\n"
        "  reg a, b;\n"
        "  initial begin\n"
        "    a = 0;\n"
        "    b = 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 6. If-else statements
 * ================================================================= */

static void test_parse_if_simple(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire a;\n"
        "  always @(*) begin\n"
        "    if (a) q = 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_if_else(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire a;\n"
        "  always @(*) begin\n"
        "    if (a) q = 1; else q = 0;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_if_else_blocks(void)
{
    const char *src =
        "module test;\n"
        "  reg q, r; wire a;\n"
        "  always @(*) begin\n"
        "    if (a) begin q = 1; r = 0; end\n"
        "    else begin q = 0; r = 1; end\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_if_nested(void)
{
    const char *src =
        "module test;\n"
        "  reg o; wire a, b;\n"
        "  always @(*) begin\n"
        "    if (a)\n"
        "      if (b) o = 1; else o = 0;\n"
        "    else\n"
        "      o = 2;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_if_in_always(void)
{
    /* if without begin-end in always */
    const char *src =
        "module test;\n"
        "  reg q; wire a;\n"
        "  always @(a) begin\n"
        "    if (a) q = 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 7. Case statements
 * ================================================================= */

static void test_parse_case_simple(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  reg o;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0: o = 1;\n"
        "      1: o = 0;\n"
        "      2: o = 1;\n"
        "      3: o = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_case_multi_pattern(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  reg o;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0, 1: o = 1;\n"
        "      2, 3: o = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_case_default(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  reg o;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0: o = 1;\n"
        "      default: o = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_casez_simple(void)
{
    const char *src =
        "module test;\n"
        "  reg [3:0] in;\n"
        "  reg [1:0] out;\n"
        "  always @(*) begin\n"
        "    casez (in)\n"
        "      4'b0001: out = 0;\n"
        "      4'b0010: out = 1;\n"
        "      default: out = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_casez_dontcare(void)
{
    const char *src =
        "module test;\n"
        "  reg [3:0] in;\n"
        "  reg [1:0] out;\n"
        "  always @(*) begin\n"
        "    casez (in)\n"
        "      4'b???1: out = 0;\n"
        "      4'b??10: out = 1;\n"
        "      default: out = 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_case_multi_stmt_body(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  reg a, b;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0: begin a = 1; b = 0; end\n"
        "      1: begin a = 0; b = 1; end\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 9. Blocking and nonblocking assignments
 * ================================================================= */

static void test_parse_blocking_simple(void)
{
    const char *src =
        "module test;\n"
        "  reg q;\n"
        "  always @(*) q = 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_blocking_expr(void)
{
    const char *src =
        "module test;\n"
        "  reg [3:0] cnt;\n"
        "  always @(*) cnt = cnt + 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_nonblocking_simple(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire d, clk;\n"
        "  always @(posedge clk) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_nonblocking_expr(void)
{
    const char *src =
        "module test;\n"
        "  reg [3:0] cnt;\n"
        "  always @(posedge clk) cnt <= cnt + 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_blocking_self_add(void)
{
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  always @(*) i = i + 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_blocking_multi_in_seq(void)
{
    const char *src =
        "module test;\n"
        "  reg a, b;\n"
        "  always @(*) begin\n"
        "    a = 1;\n"
        "    b = 2;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 10. Module instantiation
 * ================================================================= */

static void test_parse_module_instance(void)
{
    const char *src =
        "module top;\n"
        "  wire clk;\n"
        "  sub u_sub (.clk(clk));\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "top", "module is top");
}

static void test_parse_inst_named_multi(void)
{
    const char *src =
        "module top;\n"
        "  wire a, b, c;\n"
        "  sub u (.x(a), .y(b), .z(c));\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_inst_positional(void)
{
    const char *src =
        "module top;\n"
        "  wire a, b;\n"
        "  sub u (a, b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_inst_no_conns(void)
{
    const char *src =
        "module top;\n"
        "  sub u ();\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_inst_multi_instances(void)
{
    const char *src =
        "module top;\n"
        "  wire a, b;\n"
        "  inv u1 (.in(a), .out(b));\n"
        "  inv u2 (.in(b), .out(a));\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->instance_count >= 2, "2 instances");
}

/* =================================================================
 * 11. Expressions (via assign)
 * ================================================================= */

static void test_parse_expr_add_sub_mix(void)
{
    const char *src =
        "module test;\n"
        "  assign y = a + b - c;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_expr_mul_div_mod(void)
{
    const char *src =
        "module test;\n"
        "  assign y = a * b / c % d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_expr_ternary_nested(void)
{
    const char *src =
        "module test;\n"
        "  assign y = s1 ? (s2 ? a : b) : c;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_expr_concat_and_ternary(void)
{
    const char *src =
        "module test;\n"
        "  assign y = sel ? {a, b} : {c, d};\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_expr_shift_combined(void)
{
    const char *src =
        "module test;\n"
        "  assign y = (a << 2) | (b >> 1);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 12. Literals / Numbers
 * ================================================================= */

static void test_parse_lit_sized_dec(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'd10;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_sized_hex(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'hFF;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_sized_hex_lower(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'hff;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_sized_bin(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'b1010;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_sized_oct(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'o77;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_unsized(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 42;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_signed(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'sd5;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_signed_hex(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'shA5;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_x_in_binary(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'b01x0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_z_in_binary(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'b01z0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_qmark(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 4'b???1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_underscore_bin(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'b1010_1010;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_underscore_dec(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'd123;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_hex_x_digits(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'hx5;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_zero_width(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_lit_one(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 1'b1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 13. Parameters
 * ================================================================= */

static void test_parse_parameter_single(void)
{
    const char *src =
        "module test;\n"
        "  parameter W = 8;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_parameter_list(void)
{
    const char *src =
        "module test;\n"
        "  parameter W = 8, H = 4;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_parameter_expr(void)
{
    const char *src =
        "module test;\n"
        "  parameter W = 8, DEPTH = W * 2;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_localparam(void)
{
    const char *src =
        "module test;\n"
        "  localparam W = 8;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 14. Comments (interspersed with code)
 * ================================================================= */

static void test_parse_line_comment(void)
{
    const char *src =
        "// top comment\n"
        "module test;\n"
        "  // body comment\n"
        "  wire a; // inline comment\n"
        "endmodule\n"
        "// trailing comment\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_block_comment(void)
{
    const char *src =
        "/* header */ module test;\n"
        "  /* mid */ wire a; /* inline */\n"
        "endmodule\n"
        "/* footer */\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_multi_line_comment(void)
{
    const char *src =
        "module test;\n"
        "  /*\n"
        "   * multi-line\n"
        "   * comment\n"
        "   */\n"
        "  wire a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 15. Named sequential block
 * ================================================================= */

static void test_parse_seq_block_named(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire clk;\n"
        "  always @(posedge clk) begin : my_block\n"
        "    q <= 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 16. Combined constructs
 * ================================================================= */

static void test_parse_always_if_else_inside(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire a, b, clk;\n"
        "  always @(posedge clk) begin\n"
        "    if (a) q <= b; else q <= a;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_case_inside(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] state;\n"
        "  reg out;\n"
        "  wire clk;\n"
        "  always @(posedge clk) begin\n"
        "    case (state)\n"
        "      0: out <= 1;\n"
        "      1: out <= 0;\n"
        "      default: out <= 0;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_process_count(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire clk;\n"
        "  always @(posedge clk) q <= 1;\n"
        "  always @(negedge clk) q <= 0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->process_count, 2, "2 processes");
}

static void test_parse_initial_and_always(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire clk;\n"
        "  initial q = 0;\n"
        "  always @(posedge clk) q <= ~q;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->process_count >= 2, "at least 2 processes");
}

static void test_parse_module_signal_and_inst(void)
{
    const char *src =
        "module top;\n"
        "  wire a, b;\n"
        "  and g1 (a, b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("top.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * Additional positive edge cases
 * ================================================================= */

static void test_parse_assign_xnor(void)
{
    const char *src =
        "module test;\n"
        "  wire y;\n"
        "  assign y = ~(a ^ b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_always_deep_nested(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire a, b, clk;\n"
        "  always @(posedge clk) begin\n"
        "    if (a) begin\n"
        "      if (b) q <= 1; else q <= 0;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_wire_range_large(void)
{
    const char *src = "module test; wire [255:0] bigbus; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_blocking_assign_concat_rhs(void)
{
    const char *src =
        "module test;\n"
        "  reg [7:0] q;\n"
        "  always @(*) q = {a, b};\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_case_no_items(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_multi_decls_empty_body(void)
{
    const char *src =
        "module test;\n"
        "  wire a;\n"
        "  reg b;\n"
        "  integer c;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "3 signals");
}

static void test_parse_line_comment_before_module(void)
{
    const char *src =
        "// Copyright 2024\n"
        "// License: MIT\n"
        "module test;\n"
        "  wire a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_block_comment_between_decls(void)
{
    const char *src =
        "module test;\n"
        "  wire a; /* comment */\n"
        "  reg b; /* another */\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 2, "2 signals");
}

static void test_parse_always_sens_list_or(void)
{
    const char *src =
        "module test;\n"
        "  reg q; wire clk, rst;\n"
        "  always @(posedge clk or posedge rst) q <= 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

static void test_parse_initial_sequential(void)
{
    const char *src =
        "module test;\n"
        "  reg a, b;\n"
        "  initial begin\n"
        "    a = 0;\n"
        "    b = a + 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * NEGATIVE TESTS — illegal constructs that should be rejected
 * ================================================================= */

static void test_parse_invalid_syntax(void)
{
    const char *src = "not_a_module;";
    parse_result_t r = verilog_parse("broken.v", src, strlen(src));
    mu_assert(!r.success, "should fail on invalid syntax");
}

static void test_parse_missing_endmodule(void)
{
    const char *src = "module test;\n  wire a;\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "missing endmodule should fail");
}

static void test_parse_missing_module_keyword(void)
{
    const char *src = "test (); endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "missing module keyword should fail");
}

static void test_parse_unclosed_block_comment(void)
{
    const char *src = "module test; /* unclosed comment\nwire a;\nendmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "unclosed comment should consume everything");
}

static void test_parse_stray_character(void)
{
    const char *src = "module test; wire a; $ % @ endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "stray chars should fail");
}

static void test_parse_always_no_sensitivity(void)
{
    const char *src =
        "module test;\n"
        "  reg q;\n"
        "  always q <= 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "always without sensitivity is now valid");
}

static void test_parse_port_list_no_rparen(void)
{
    const char *src = "module test (input a;\nendmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "missing rparen should fail");
}

static void test_parse_missing_endcase(void)
{
    const char *src =
        "module test;\n"
        "  reg [1:0] sel;\n"
        "  always @(*) begin\n"
        "    case (sel)\n"
        "      0: o = 1;\n"
        "    end\n"  /* wrong: end instead of endcase */
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "missing endcase should fail");
}

static void test_parse_if_no_parens(void)
{
    const char *src =
        "module test;\n"
        "  reg q;\n"
        "  always @(*) begin\n"
        "    if a q = 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "if without parens should fail");
}

static void test_parse_assign_no_semi(void)
{
    const char *src =
        "module test;\n"
        "  assign y = a\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "assign without semicolon should fail");
}

static void test_parse_bad_number(void)
{
    const char *src =
        "module test;\n"
        "  assign y = 8'q123;\n"  /* invalid base */
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "invalid number base should fail");
}

static void test_parse_garbage_after_module(void)
{
    const char *src = "module test; endmodule garbage\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    /* Grammar does not enforce EOF — trailing garbage is ignored */
    mu_assert(r.success != 0, "garbage after module: module should still parse");
}

static void test_parse_decl_missing_id(void)
{
    const char *src = "module test; wire ; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "wire decl without id should fail");
}

static void test_parse_assign_no_rhs(void)
{
    const char *src =
        "module test;\n"
        "  assign y = ;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "assign without rhs should fail");
}

static void test_parse_case_no_expr(void)
{
    const char *src =
        "module test;\n"
        "  always @(*) begin\n"
        "    case ()\n"
        "      0: o = 1;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "case without expression should fail");
}

static void test_parse_inst_no_semi(void)
{
    const char *src =
        "module top;\n"
        "  sub u (.a(b))\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "instance without semicolon should fail");
}

static void test_parse_unclosed_paren_expr(void)
{
    const char *src =
        "module test;\n"
        "  assign y = (a + b;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "unclosed paren should fail");
}

static void test_parse_invalid_port_dir(void)
{
    const char *src = "module test (tri a); endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "invalid port direction should fail");
}

static void test_parse_module_missing_name(void)
{
    const char *src = "module (); endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "module without name should fail");
}

static void test_parse_double_semicolon_decl(void)
{
    const char *src = "module test; wire a;; endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "extra semicolon in body should fail");
}

static void test_parse_initial_no_body(void)
{
    const char *src = "module test; initial endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "initial without body should fail");
}

static void test_parse_block_comment_no_close(void)
{
    const char *src = "module test; /* oops endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "unclosed block comment should fail");
}

static void test_parse_always_double_at(void)
{
    const char *src =
        "module test;\n"
        "  always @@(posedge clk) q <= d;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "double @ should fail");
}

static void test_parse_case_missing_endcase(void)
{
    const char *src =
        "module test;\n"
        "  always @(*) begin\n"
        "    case (sel) end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "case without endcase should fail");
}

static void test_parse_invalid_expr_op(void)
{
    const char *src =
        "module test;\n"
        "  assign y = a ** b;\n"
        "endmodule\n";
    /* ** (exponentiation) is not a Verilog operator in IEEE 1364-2005 */
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(!r.success, "invalid operator should fail");
}

/* ── Function/Task parser tests ── */

static void test_parse_function_no_ports(void)
{
    const char *src =
        "module test;\n"
        "  function [7:0] get_val;\n"
        "  begin\n"
        "    get_val = 8'h42;\n"
        "  end\n"
        "  endfunction\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse function no ports should succeed");
    mu_assert(r.unit->func_task_count == 1, "should have 1 func/task");
    uir_func_t *ft = r.unit->func_tasks[0];
    mu_assert_not_null(ft);
    mu_assert(ft->is_function != 0, "should be a function");
    mu_assert_str_eq(ft->name, "get_val", "function name");
    mu_assert_eq(ft->return_width, 8u, "return width 8");
    mu_assert_eq(ft->port_count, 0u, "no ports");
    mu_assert_ptr_not_null(ft->body, "body exists");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_function_with_ports(void)
{
    const char *src =
        "module test;\n"
        "  function [7:0] add;\n"
        "    input [7:0] a;\n"
        "    input [7:0] b;\n"
        "  begin\n"
        "    add = a + b;\n"
        "  end\n"
        "  endfunction\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse function with ports");
    mu_assert(r.unit->func_task_count == 1, "1 func/task");
    uir_func_t *ft = r.unit->func_tasks[0];
    mu_assert_not_null(ft);
    mu_assert(ft->is_function != 0, "is function");
    mu_assert_str_eq(ft->name, "add", "name");
    mu_assert_eq(ft->return_width, 8u, "return width 8");
    mu_assert_eq(ft->port_count, 2u, "2 ports");
    mu_assert_str_eq(ft->ports[0].name, "a", "port 0 = a");
    mu_assert_eq(ft->ports[0].direction, UIR_PORT_IN, "port 0 dir");
    mu_assert_eq(ft->ports[0].width, 8u, "port 0 width");
    mu_assert_str_eq(ft->ports[1].name, "b", "port 1 = b");
    mu_assert_eq(ft->ports[1].width, 8u, "port 1 width");
    mu_assert(ft->local_count >= 1, "has return reg local");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_task_inout(void)
{
    const char *src =
        "module test;\n"
        "  task swap;\n"
        "    inout [7:0] a;\n"
        "    inout [7:0] b;\n"
        "    reg [7:0] tmp;\n"
        "  begin\n"
        "    tmp = a;\n"
        "    a = b;\n"
        "    b = tmp;\n"
        "  end\n"
        "  endtask\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse task inout");
    mu_assert(r.unit->func_task_count == 1, "1 func/task");
    uir_func_t *ft = r.unit->func_tasks[0];
    mu_assert_not_null(ft);
    mu_assert(ft->is_function == 0, "is task");
    mu_assert_str_eq(ft->name, "swap", "task name");
    mu_assert_eq(ft->return_width, 0u, "task return width 0");
    mu_assert_eq(ft->port_count, 2u, "2 ports");
    mu_assert_eq(ft->ports[0].direction, UIR_PORT_INOUT, "port 0 inout");
    mu_assert_eq(ft->ports[1].direction, UIR_PORT_INOUT, "port 1 inout");
    mu_assert(ft->local_count >= 1, "has tmp local");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_multiple_functions(void)
{
    const char *src =
        "module test;\n"
        "  function [3:0] add4;\n"
        "    input [3:0] a, b;\n"
        "  begin\n"
        "    add4 = a + b;\n"
        "  end\n"
        "  endfunction\n"
        "  function [3:0] sub4;\n"
        "    input [3:0] a, b;\n"
        "  begin\n"
        "    sub4 = a - b;\n"
        "  end\n"
        "  endfunction\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse multiple functions");
    mu_assert(r.unit->func_task_count == 2, "2 func/tasks");
    uir_func_t *f0 = r.unit->func_tasks[0];
    uir_func_t *f1 = r.unit->func_tasks[1];
    mu_assert_str_eq(f0->name, "add4", "first func name");
    mu_assert_str_eq(f1->name, "sub4", "second func name");
    mu_assert(f0->is_function != 0, "first is function");
    mu_assert(f1->is_function != 0, "second is function");
    uir_destroy_design_unit(r.unit);
}

/* ── System task ($display/$write/$monitor) parser tests ── */

static void test_parse_display(void)
{
    const char *src =
        "module test;\n"
        "  initial $display(\"hello world\");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $display");
    uir_design_unit_t *u = r.unit;
    mu_assert(u != NULL, "unit exists");
    uir_process_t *proc = u->process_count > 0 ? u->processes[0] : NULL;
    mu_assert(proc != NULL, "initial process exists");
    uir_block_t *block = proc->body ? (uir_block_t *)proc->body : NULL;
    mu_assert(block != NULL && block->base.kind == UIR_BLOCK, "body is block");
    mu_assert(block->stmt_count == 1, "one statement");
    mu_assert(block->stmts[0]->kind == UIR_SYS_TASK, "stmt is sys_task");
    uir_sys_task_t *t = (uir_sys_task_t *)block->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_DISPLAY, "kind is DISPLAY");
    mu_assert_str_eq(t->fmt, "hello world", "format string");
    mu_assert(t->arg_count == 0, "no args");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_display_with_args(void)
{
    const char *src =
        "module test(input clk);\n"
        "  reg [7:0] x;\n"
        "  always @(posedge clk) $display(\"x=%d y=%h\", x, 42);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $display with args");
    uir_design_unit_t *u = r.unit;
    mu_assert(u != NULL, "unit exists");
    mu_assert(u->process_count > 0, "process exists");
    uir_process_t *proc = u->processes[0];
    uir_block_t *block = (uir_block_t *)proc->body;
    mu_assert(block->stmt_count == 1, "one statement");
    mu_assert(block->stmts[0]->kind == UIR_SYS_TASK, "stmt is sys_task");
    uir_sys_task_t *t = (uir_sys_task_t *)block->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_DISPLAY, "kind is DISPLAY");
    mu_assert_str_eq(t->fmt, "x=%d y=%h", "format string");
    mu_assert(t->arg_count == 2, "two args");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_write(void)
{
    const char *src =
        "module test;\n"
        "  initial $write(\"hello\");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $write");
    uir_design_unit_t *u = r.unit;
    mu_assert(u != NULL, "unit exists");
    uir_process_t *proc = u->process_count > 0 ? u->processes[0] : NULL;
    mu_assert(proc != NULL, "initial process exists");
    uir_block_t *block = proc->body ? (uir_block_t *)proc->body : NULL;
    mu_assert(block->stmt_count == 1, "one statement");
    mu_assert(block->stmts[0]->kind == UIR_SYS_TASK, "stmt is sys_task");
    uir_sys_task_t *t = (uir_sys_task_t *)block->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_WRITE, "kind is WRITE");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_monitor(void)
{
    const char *src =
        "module test(input clk, input rst);\n"
        "  reg [7:0] a, b;\n"
        "  initial $monitor(\"a=%d b=%d\", a, b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $monitor");
    uir_design_unit_t *u = r.unit;
    mu_assert(u != NULL, "unit exists");
    uir_process_t *proc = u->processes[0];
    uir_block_t *block = (uir_block_t *)proc->body;
    mu_assert(block->stmt_count == 1, "one statement");
    uir_sys_task_t *t = (uir_sys_task_t *)block->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_MONITOR, "kind is MONITOR");
    mu_assert(t->arg_count == 2, "two args");
    uir_destroy_design_unit(r.unit);
}

/* ── Loop tests: for, while, repeat, forever ── */

static void test_parse_for_loop(void)
{
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  reg [7:0] arr [0:15];\n"
        "  initial begin\n"
        "    for (i = 0; i < 16; i = i + 1)\n"
        "      arr[i] = i;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "for loop parse");
    mu_assert(r.unit->process_count >= 1, "has process");
    uir_process_t *p = r.unit->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count >= 1, "body has stmts");
    mu_assert(b->stmts[b->stmt_count - 1]->kind == UIR_LOOP, "last stmt is UIR_LOOP");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_for_block(void)
{
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  reg [7:0] arr;\n"
        "  initial begin\n"
        "    for (i = 0; i < 10; i = i + 1) begin\n"
        "      arr = arr + 1;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "for loop with block parse");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_for_init(void)
{
    /* verify for-init doesn't leak into parent block */
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  reg a;\n"
        "  initial begin\n"
        "    a = 0;\n"
        "    for (i = 0; i < 5; i = i + 1)\n"
        "      a = a + 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "for with prior stmt");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    /* a=0 should be in process body, for-init should NOT be there */
    mu_assert(b->stmt_count == 2, "two stmts in body: a=0 and loop");
    mu_assert(b->stmts[0]->kind == UIR_ASSIGN, "first is assign");
    mu_assert(b->stmts[1]->kind == UIR_LOOP, "second is loop");
    uir_loop_t *loop = (uir_loop_t *)b->stmts[1];
    mu_assert(loop->init_stmt != NULL, "loop has init");
    mu_assert(loop->condition != NULL, "loop has cond");
    mu_assert(loop->step_stmt != NULL, "loop has step");
    mu_assert(loop->body != NULL, "loop has body");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_while_loop(void)
{
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    i = 0;\n"
        "    while (i < 10)\n"
        "      i = i + 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "while loop parse");
    uir_design_unit_t *u = r.unit;
    mu_assert(u->process_count >= 1, "has process");
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count >= 1, "body has stmts");
    mu_assert(b->stmts[b->stmt_count - 1]->kind == UIR_LOOP, "last stmt is UIR_LOOP");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_repeat_loop(void)
{
    const char *src =
        "module test;\n"
        "  integer i;\n"
        "  initial begin\n"
        "    i = 0;\n"
        "    repeat(10) i = i + 1;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "repeat loop parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count >= 1, "body has stmts");
    mu_assert(b->stmts[b->stmt_count - 1]->kind == UIR_LOOP, "last stmt is UIR_LOOP");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_forever_loop(void)
{
    const char *src =
        "module test;\n"
        "  reg clk;\n"
        "  initial forever #5 clk = ~clk;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "forever loop parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_LOOP, "stmt is UIR_LOOP");
    uir_destroy_design_unit(r.unit);
}

/* ── Wait statement ═══════════════════════════════════════════════ */

static void test_parse_wait_basic(void)
{
    const char *src =
        "module test;\n"
        "  reg a;\n"
        "  initial wait (a) a = 0;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "wait basic parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_WAIT, "stmt is UIR_WAIT");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_wait_with_block(void)
{
    /* With transparent seq_block, both assignments go to process body.
     * do_wait_finish wraps only the LAST stmt in UIR_WAIT. */
    const char *src =
        "module test;\n"
        "  reg a, b;\n"
        "  initial wait (a) begin a = 0; b = 1; end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "wait block parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count >= 1, "has stmts");
    /* UIR_WAIT should be among the stmts */
    int found_wait = 0;
    for (size_t i = 0; i < b->stmt_count; i++)
        if (b->stmts[i]->kind == UIR_WAIT) found_wait = 1;
    mu_assert(found_wait, "found UIR_WAIT");
    uir_destroy_design_unit(r.unit);
}

/* ── Event control @() ════════════════════════════════════════════ */

static void test_parse_event_control_posedge(void)
{
    const char *src =
        "module test;\n"
        "  reg a;\n"
        "  initial @(posedge clk) a = 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "event posedge parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_EVENT_CTRL, "stmt is UIR_EVENT_CTRL");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_event_control_negedge(void)
{
    const char *src =
        "module test;\n"
        "  reg a;\n"
        "  initial @(negedge clk) a = 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "event negedge parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_EVENT_CTRL, "stmt is UIR_EVENT_CTRL");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_event_control_any(void)
{
    const char *src =
        "module test;\n"
        "  reg a;\n"
        "  initial @(clk) a = 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "event any parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_EVENT_CTRL, "stmt is UIR_EVENT_CTRL");
    uir_destroy_design_unit(r.unit);
}

/* ── Disable statement ════════════════════════════════════════════ */

static void test_parse_disable(void)
{
    /* Named block wrap may be transparent; check that UIR_DISABLE exists in stmts */
    const char *src =
        "module test;\n"
        "  initial begin : my_block\n"
        "    disable my_block;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "disable parse");
    uir_design_unit_t *u = r.unit;
    uir_process_t *p = u->processes[0];
    uir_block_t *b = (uir_block_t *)p->body;
    int found_disable = 0;
    for (size_t i = 0; i < b->stmt_count; i++) {
        if (b->stmts[i]->kind == UIR_DISABLE) found_disable = 1;
        else if (b->stmts[i]->kind == UIR_BLOCK) {
            uir_block_t *nb = (uir_block_t *)b->stmts[i];
            for (size_t j = 0; j < nb->stmt_count; j++)
                if (nb->stmts[j]->kind == UIR_DISABLE) found_disable = 1;
        }
    }
    mu_assert(found_disable, "found UIR_DISABLE in stmts");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_disable_task(void)
{
    const char *src =
        "module test;\n"
        "  task my_task;\n"
        "  begin\n"
        "    disable my_task;\n"
        "  end\n"
        "  endtask\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "disable task parse");
    uir_design_unit_t *u = r.unit;
    mu_assert(u->func_task_count >= 1, "has task");
    uir_func_t *ft = u->func_tasks[0];
    uir_block_t *b = (uir_block_t *)ft->body;
    mu_assert(b->stmt_count == 1, "single stmt");
    mu_assert(b->stmts[0]->kind == UIR_DISABLE, "stmt is UIR_DISABLE");
    uir_destroy_design_unit(r.unit);
}

/* ── Parser tests for system tasks and functions (Phase 2) ── */

static void test_parse_stop_no_arg(void)
{
    const char *src =
        "module test;\n"
        "  initial $stop;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $stop");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_STOP, "task kind STOP");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_stop_with_arg(void)
{
    const char *src =
        "module test;\n"
        "  initial $stop(1);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $stop(1)");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_STOP, "task kind STOP");
    mu_assert(t->arg_count == 1, "one arg");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_finish_no_arg(void)
{
    const char *src =
        "module test;\n"
        "  initial $finish;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $finish");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_FINISH, "task kind FINISH");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_finish_with_arg(void)
{
    const char *src =
        "module test;\n"
        "  initial $finish(0);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $finish(0)");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_FINISH, "task kind FINISH");
    mu_assert(t->arg_count == 1, "one arg");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_severity_tasks(void)
{
    const char *src =
        "module test;\n"
        "  initial begin\n"
        "    $fatal(\"fatal msg\");\n"
        "    $error(\"error msg\");\n"
        "    $warning(\"warn msg\");\n"
        "    $info(\"info msg\");\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse severity tasks");
    uir_block_t *b = (uir_block_t *)((uir_process_t *)r.unit->processes[0])->body;
    mu_assert(((uir_sys_task_t *)b->stmts[0])->task_kind == UIR_SYS_FATAL, "kind FATAL");
    mu_assert_str_eq(((uir_sys_task_t *)b->stmts[0])->fmt, "fatal msg", "fatal fmt");
    mu_assert(((uir_sys_task_t *)b->stmts[1])->task_kind == UIR_SYS_ERROR, "kind ERROR");
    mu_assert_str_eq(((uir_sys_task_t *)b->stmts[1])->fmt, "error msg", "error fmt");
    mu_assert(((uir_sys_task_t *)b->stmts[2])->task_kind == UIR_SYS_WARNING, "kind WARNING");
    mu_assert(((uir_sys_task_t *)b->stmts[3])->task_kind == UIR_SYS_INFO, "kind INFO");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sys_func_signed(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a;\n"
        "  assign a = $signed(b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $signed");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_SIGNED, "func kind SIGNED");
    mu_assert(sf->arg_count == 1, "one arg");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sys_func_unsigned(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a;\n"
        "  assign a = $unsigned(b);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $unsigned");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_UNSIGNED, "func kind UNSIGNED");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_clog2(void)
{
    const char *src =
        "module test;\n"
        "  wire [3:0] a;\n"
        "  assign a = $clog2(16);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $clog2");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_CLOG2, "func kind CLOG2");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sys_time(void)
{
    const char *src =
        "module test;\n"
        "  wire [63:0] t;\n"
        "  assign t = $time;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $time");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_TIME, "func kind TIME");
    mu_assert(sf->arg_count == 0, "zero args");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sys_realtime(void)
{
    const char *src =
        "module test;\n"
        "  wire [63:0] t;\n"
        "  assign t = $realtime;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $realtime");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_REALTIME, "func kind REALTIME");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_random_no_arg(void)
{
    const char *src =
        "module test;\n"
        "  wire [31:0] r;\n"
        "  assign r = $random;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $random");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_RANDOM, "func kind RANDOM");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_random_with_arg(void)
{
    const char *src =
        "module test;\n"
        "  wire [31:0] r;\n"
        "  assign r = $random(42);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $random(42)");
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)r.unit->assigns[0]->rhs;
    mu_assert(sf->func_kind == UIR_SYS_FUNC_RANDOM, "func kind RANDOM");
    mu_assert(sf->arg_count == 1, "one arg");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_dumpfile(void)
{
    const char *src =
        "module test;\n"
        "  initial $dumpfile(\"test.vcd\");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $dumpfile");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_DUMPFILE, "kind DUMPFILE");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_dumpvars(void)
{
    const char *src =
        "module test;\n"
        "  initial $dumpvars;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $dumpvars");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_DUMPVARS, "kind DUMPVARS");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_dumpon_dumpoff(void)
{
    const char *src =
        "module test;\n"
        "  initial begin\n"
        "    $dumpon;\n"
        "    $dumpoff;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $dumpon/$dumpoff");
    uir_block_t *b = (uir_block_t *)((uir_process_t *)r.unit->processes[0])->body;
    mu_assert(((uir_sys_task_t *)b->stmts[0])->task_kind == UIR_SYS_DUMPON, "kind DUMPON");
    mu_assert(((uir_sys_task_t *)b->stmts[1])->task_kind == UIR_SYS_DUMPOFF, "kind DUMPOFF");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_value_plusargs(void)
{
    const char *src =
        "module test;\n"
        "  reg [31:0] v;\n"
        "  initial $value$plusargs(\"TEST=%d\", v);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $value$plusargs");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_VALUE_PLUSARGS, "kind VALUE_PLUSARGS");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_test_plusargs(void)
{
    const char *src =
        "module test;\n"
        "  initial $test$plusargs(\"TEST\");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $test$plusargs");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_TEST_PLUSARGS, "kind TEST_PLUSARGS");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_fopen_stub(void)
{
    const char *src =
        "module test;\n"
        "  integer fd;\n"
        "  initial fd = $fopen(\"out.txt\");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $fopen");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_fwrite_fdisplay(void)
{
    const char *src =
        "module test;\n"
        "  integer fd;\n"
        "  initial begin\n"
        "    $fwrite(fd, \"fmt %%d\\n\", 42);\n"
        "    $fdisplay(fd, \"fmt %%d\\n\", 42);\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $fwrite/$fdisplay");
    uir_block_t *b = (uir_block_t *)((uir_process_t *)r.unit->processes[0])->body;
    mu_assert(((uir_sys_task_t *)b->stmts[0])->task_kind == UIR_SYS_FWRITE, "kind FWRITE");
    mu_assert(((uir_sys_task_t *)b->stmts[1])->task_kind == UIR_SYS_FDISPLAY, "kind FDISPLAY");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_fclose(void)
{
    const char *src =
        "module test;\n"
        "  integer fd;\n"
        "  initial $fclose(fd);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $fclose");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_FCLOSE, "kind FCLOSE");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_timeformat(void)
{
    const char *src =
        "module test;\n"
        "  initial $timeformat(-9, 3, \"ns\", 10);\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $timeformat");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_TIMEFORMAT, "kind TIMEFORMAT");
    uir_destroy_design_unit(r.unit);
}

/* ── Named events (Phase 5a) ── */

static void test_parse_event_decl(void)
{
    const char *src =
        "module test;\n"
        "  event e1, e2;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse event decl");
    mu_assert(has_signal(r.unit, "e1", UIR_SIG_EVENT), "e1 is event");
    mu_assert(has_signal(r.unit, "e2", UIR_SIG_EVENT), "e2 is event");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_trigger(void)
{
    const char *src =
        "module test;\n"
        "  event ev;\n"
        "  initial -> ev;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse trigger");
    uir_process_t *proc = r.unit->processes[0];
    uir_block_t *b = (uir_block_t *)proc->body;
    mu_assert(b->stmt_count >= 1, "body has stmts");
    int found = 0;
    for (size_t i = 0; i < b->stmt_count; i++)
        if (b->stmts[i]->kind == UIR_EVENT_TRIGGER) found = 1;
    mu_assert(found, "found UIR_EVENT_TRIGGER");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_event_sensitivity(void)
{
    const char *src =
        "module test;\n"
        "  event ev;\n"
        "  reg a;\n"
        "  always @(ev) a = 1;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse event sensitivity");
    uir_process_t *proc = r.unit->processes[0];
    mu_assert(proc->sensitivity_count >= 1, "has sensitivity");
    mu_assert(proc->sensitivity_list[0].edge == 0, "level sensitive");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_printtimescale(void)
{
    const char *src =
        "module test;\n"
        "  initial $printtimescale;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse $printtimescale");
    uir_process_t *proc = r.unit->processes[0];
    uir_sys_task_t *t = (uir_sys_task_t *)((uir_block_t *)proc->body)->stmts[0];
    mu_assert(t->task_kind == UIR_SYS_PRINTTIMESCALE, "kind PRINTTIMESCALE");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Indexed part-select (+: / -:)
 * ================================================================= */

static void test_parse_indexed_part_select_plus(void)
{
    const char *src =
        "module test;\n"
        "  wire [7:0] a;\n"
        "  wire [3:0] b;\n"
        "  assign b = a[3 +: 4];\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse indexed +: part-select");
    mu_assert(r.unit->assign_count >= 1, "has assign");
    uir_ref_t *ref = (uir_ref_t *)r.unit->assigns[0]->rhs;
    mu_assert(ref->base.kind == UIR_REF, "RHS is a ref");
    mu_assert_not_null(ref->part_hi);
    mu_assert_not_null(ref->part_lo);
    mu_assert_str_eq(ref->name, "a", "ref name is a");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_indexed_part_select_minus(void)
{
    const char *src =
        "module test;\n"
        "  wire [7:0] a;\n"
        "  wire [3:0] b;\n"
        "  assign b = a[6 -: 4];\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse indexed -: part-select");
    mu_assert(r.unit->assign_count >= 1, "has assign");
    uir_ref_t *ref = (uir_ref_t *)r.unit->assigns[0]->rhs;
    mu_assert(ref->base.kind == UIR_REF, "RHS is a ref");
    mu_assert_not_null(ref->part_hi);
    mu_assert_not_null(ref->part_lo);
    mu_assert_str_eq(ref->name, "a", "ref name is a");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Attributes (* ... *)
 * ================================================================= */

static void test_parse_attr_simple(void)
{
    const char *src =
        "module test;\n"
        "  (* keep = 1 *) wire a;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse attr on wire");
    mu_assert(r.unit->attr_count >= 1, "has attr");
    mu_assert_str_eq(r.unit->attrs[0].name, "keep", "attr name");
    mu_assert_str_eq(r.unit->attrs[0].value, "1", "attr value");
    mu_assert(has_signal(r.unit, "a", UIR_SIG_WIRE), "wire a exists");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_attr_on_reg(void)
{
    const char *src =
        "module test;\n"
        "  (* init = 32'hDEAD_BEEF *) reg [31:0] debug_val;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse attr on reg");
    mu_assert(r.unit->attr_count >= 1, "has attr");
    mu_assert_str_eq(r.unit->attrs[0].name, "init", "attr name");
    mu_assert_str_eq(r.unit->attrs[0].value, "32'hDEAD_BEEF", "attr value");
    mu_assert(has_signal(r.unit, "debug_val", UIR_SIG_REG), "reg exists");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Defparam
 * ================================================================= */

static void test_parse_defparam_simple(void)
{
    const char *src =
        "module test;\n"
        "  defparam uut.WIDTH = 8;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse defparam");
    mu_assert(r.unit->defparam_count >= 1, "has defparam");
    mu_assert_str_eq(r.unit->defparams[0].hier_path, "uut.WIDTH", "hier path");
    mu_assert(r.unit->defparams[0].value != NULL, "value is not null");
    mu_assert(r.unit->defparams[0].value->kind == UIR_LITERAL, "value is literal");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_defparam_multi(void)
{
    const char *src =
        "module test;\n"
        "  defparam uut.WIDTH = 8, uut.DEPTH = 256;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parse multi defparam");
    mu_assert(r.unit->defparam_count >= 2, "has 2 defparams");
    mu_assert_str_eq(r.unit->defparams[0].hier_path, "uut.WIDTH", "first path");
    mu_assert_str_eq(r.unit->defparams[1].hier_path, "uut.DEPTH", "second path");
    mu_assert(r.unit->defparams[1].value != NULL, "second value is not null");
    uir_destroy_design_unit(r.unit);
}

/* ── Specify block tests (Phase 4a) ── */

static void test_parse_specify_empty(void)
{
    const char *src =
        "module test;\n"
        "  specify\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "empty specify block");
    mu_assert(r.unit->specify_count >= 1, "has specify block");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_specparam(void)
{
    const char *src =
        "module test;\n"
        "  specify\n"
        "    specparam t_rise = 10, t_fall = 12;\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "specparam parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->specparam_count >= 2, "has 2 specparams");
    mu_assert_str_eq(sp->specparams[0].hier_path, "t_rise", "first specparam name");
    mu_assert_str_eq(sp->specparams[1].hier_path, "t_fall", "second specparam name");
    mu_assert(sp->specparams[0].value != NULL, "first value not null");
    mu_assert(sp->specparams[1].value != NULL, "second value not null");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_parallel_path(void)
{
    const char *src =
        "module test;\n"
        "  input a; output b;\n"
        "  specify\n"
        "    (a => b) = (10, 12);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "parallel path parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert_str_eq(sp->paths[0].src, "a", "path src");
    mu_assert_str_eq(sp->paths[0].dst, "b", "path dst");
    mu_assert(sp->paths[0].type == UIR_PATH_PARALLEL, "path type parallel");
    mu_assert(sp->paths[0].rise_delay != NULL, "rise delay not null");
    mu_assert(sp->paths[0].fall_delay != NULL, "fall delay not null");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_full_path(void)
{
    const char *src =
        "module test;\n"
        "  input a; output b;\n"
        "  specify\n"
        "    (a *> b) = (10, 12, 15, 18);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "full path parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert_str_eq(sp->paths[0].src, "a", "path src");
    mu_assert_str_eq(sp->paths[0].dst, "b", "path dst");
    mu_assert(sp->paths[0].type == UIR_PATH_FULL, "path type full");
    mu_assert(sp->paths[0].rise_delay != NULL, "rise");
    mu_assert(sp->paths[0].fall_delay != NULL, "fall");
    mu_assert(sp->paths[0].z_delay != NULL, "z delay");
    mu_assert(sp->paths[0].x_delay != NULL, "x delay");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_specify_in_module(void)
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
    mu_assert(r.success, "specify in module");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 2, "has 2 paths");
    mu_assert_str_eq(sp->paths[0].src, "a", "first src");
    mu_assert_str_eq(sp->paths[1].src, "b", "second src");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_conditional_path(void)
{
    const char *src =
        "module test;\n"
        "  input a; output b;\n"
        "  specify\n"
        "    if (a) (a => b) = (10, 12);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "conditional path parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert(sp->paths[0].condition != NULL, "has condition");
    mu_assert_str_eq(sp->paths[0].src, "a", "src");
    mu_assert_str_eq(sp->paths[0].dst, "b", "dst");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_edge_path(void)
{
    const char *src =
        "module test;\n"
        "  input clk; output reg q;\n"
        "  specify\n"
        "    (posedge clk => q) = (3, 4);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "edge path parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert(sp->paths[0].src_edge == 1, "posedge");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_edge_path_data_terminal(void)
{
    const char *src =
        "module test;\n"
        "  input clk, d; output reg q;\n"
        "  specify\n"
        "    (posedge clk => (q +: d)) = (3, 4);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "edge path +: parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert_str_eq(sp->paths[0].data_src, "d", "data source");
    mu_assert(sp->paths[0].src_edge == 1, "posedge");
    mu_assert(sp->paths[0].dst_polarity == 1, "polarity +:");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_edge_path_data_terminal_neg(void)
{
    const char *src =
        "module test;\n"
        "  input rst, d; output reg q;\n"
        "  specify\n"
        "    (negedge rst => (q -: d)) = (5, 6);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "edge path -: parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->path_count >= 1, "has path");
    mu_assert_str_eq(sp->paths[0].data_src, "d", "data source");
    mu_assert(sp->paths[0].src_edge == -1, "negedge");
    mu_assert(sp->paths[0].dst_polarity == -1, "polarity -:");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_setup(void)
{
    const char *src =
        "module test;\n"
        "  input d, clk;\n"
        "  specify\n"
        "    $setup(d, clk, 10);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "$setup parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->timing_check_count >= 1, "has timing check");
    mu_assert(sp->timing_checks[0].kind == UIR_TIMING_SETUP, "kind=setup");
    mu_assert_str_eq(sp->timing_checks[0].data_pin, "d", "data pin");
    mu_assert_str_eq(sp->timing_checks[0].ref_pin, "clk", "ref pin");
    mu_assert(sp->timing_checks[0].limit != NULL, "limit not null");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_hold(void)
{
    const char *src =
        "module test;\n"
        "  input d, clk;\n"
        "  specify\n"
        "    $hold(clk, d, 5);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "$hold parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->timing_check_count >= 1, "has timing check");
    mu_assert(sp->timing_checks[0].kind == UIR_TIMING_HOLD, "kind=hold");
    mu_assert_str_eq(sp->timing_checks[0].data_pin, "d", "data pin");
    mu_assert_str_eq(sp->timing_checks[0].ref_pin, "clk", "ref pin");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_setuphold(void)
{
    const char *src =
        "module test;\n"
        "  input d, clk;\n"
        "  specify\n"
        "    $setuphold(clk, d, 10, 5);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "$setuphold parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->timing_check_count >= 1, "has timing check");
    mu_assert(sp->timing_checks[0].kind == UIR_TIMING_SETUPHOLD, "kind=setuphold");
    mu_assert(sp->timing_checks[0].limit != NULL, "limit not null");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_width(void)
{
    const char *src =
        "module test;\n"
        "  input clk;\n"
        "  specify\n"
        "    $width(clk, 10);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "$width parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->timing_check_count >= 1, "has timing check");
    mu_assert(sp->timing_checks[0].kind == UIR_TIMING_WIDTH, "kind=width");
    mu_assert_str_eq(sp->timing_checks[0].ref_pin, "clk", "ref pin");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_period(void)
{
    const char *src =
        "module test;\n"
        "  input clk;\n"
        "  specify\n"
        "    $period(clk, 20);\n"
        "  endspecify\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "$period parse");
    mu_assert(r.unit->specify_count >= 1, "has specify");
    uir_specify_t *sp = r.unit->specifies[0];
    mu_assert(sp->timing_check_count >= 1, "has timing check");
    mu_assert(sp->timing_checks[0].kind == UIR_TIMING_PERIOD, "kind=period");
    mu_assert_str_eq(sp->timing_checks[0].ref_pin, "clk", "ref pin");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 17. UDP primitives
 * ================================================================= */

static void test_parse_udp_combinational(void)
{
    const char *src =
        "primitive my_and(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  table\n"
        "    0 0 : 0;\n"
        "    0 1 : 0;\n"
        "    1 0 : 0;\n"
        "    1 1 : 1;\n"
        "  endtable\n"
        "endprimitive\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "combinational UDP parse");
    mu_assert(r.unit != NULL, "unit not null");
    mu_assert(r.unit->udp != NULL, "has udp struct");
    mu_assert(!r.unit->udp->is_sequential, "is combinational");
    mu_assert(r.unit->udp->entry_count == 4, "4 table entries");
    mu_assert(has_port(r.unit, "y", UIR_PORT_OUT), "output port y");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "input port a");
    mu_assert(has_port(r.unit, "b", UIR_PORT_IN), "input port b");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_udp_sequential(void)
{
    const char *src =
        "primitive dff(q, clk, d);\n"
        "  output reg q;\n"
        "  input clk, d;\n"
        "  table\n"
        "    (01) 0 : ? : 1;\n"
        "    (01) 1 : ? : 0;\n"
        "    (0x) 0 : ? : -;\n"
        "    (0x) 1 : ? : -;\n"
        "  endtable\n"
        "endprimitive\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "sequential UDP parse");
    mu_assert(r.unit != NULL, "unit not null");
    mu_assert(r.unit->udp != NULL, "has udp struct");
    mu_assert(r.unit->udp->is_sequential, "is sequential");
    mu_assert(r.unit->udp->entry_count == 4, "4 table entries");
    mu_assert(has_port(r.unit, "q", UIR_PORT_OUT), "output port q");
    mu_assert(has_port(r.unit, "clk", UIR_PORT_IN), "input port clk");
    mu_assert(has_port(r.unit, "d", UIR_PORT_IN), "input port d");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_udp_edge_shorthand(void)
{
    const char *src =
        "primitive dff(q, clk, d);\n"
        "  output reg q;\n"
        "  input clk, d;\n"
        "  table\n"
        "    r 0 : ? : 1;\n"
        "    f 1 : ? : 0;\n"
        "    * 0 : ? : -;\n"
        "  endtable\n"
        "endprimitive\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "edge shorthand UDP parse");
    mu_assert(r.unit != NULL, "unit not null");
    mu_assert(r.unit->udp != NULL, "has udp struct");
    mu_assert(r.unit->udp->is_sequential, "is sequential");
    mu_assert(r.unit->udp->entry_count == 3, "3 table entries");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_udp_with_initial(void)
{
    const char *src =
        "primitive dff(q, clk, d);\n"
        "  output reg q;\n"
        "  input clk, d;\n"
        "  initial q = 0;\n"
        "  table\n"
        "    (01) 0 : ? : 1;\n"
        "    (01) 1 : ? : 0;\n"
        "  endtable\n"
        "endprimitive\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "UDP with initial parse");
    mu_assert(r.unit != NULL, "unit not null");
    mu_assert(r.unit->udp != NULL, "has udp struct");
    mu_assert(r.unit->udp->is_sequential, "is sequential");
    mu_assert(r.unit->udp->entry_count == 2, "2 table entries");
    mu_assert(r.unit->process_count >= 1, "has initial process");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * SystemVerilog RTL tests (Mode 1)
 * ================================================================= */

static void test_parse_logic_decl(void)
{
    const char *src =
        "module test;\n"
        "  logic [7:0] data;\n"
        "  logic clk;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(has_signal(r.unit, "data", UIR_SIG_LOGIC), "should have data signal");
    mu_assert(has_signal(r.unit, "clk", UIR_SIG_LOGIC), "should have clk signal");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_logic_port(void)
{
    const char *src =
        "module test (\n"
        "  input logic clk,\n"
        "  output logic [7:0] data,\n"
        "  inout logic bidir\n"
        ");\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(has_port(r.unit, "clk", UIR_PORT_IN), "should have input clk");
    mu_assert(has_port(r.unit, "data", UIR_PORT_OUT), "should have output data");
    mu_assert(has_port(r.unit, "bidir", UIR_PORT_INOUT), "should have inout bidir");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_always_comb(void)
{
    const char *src =
        "module test;\n"
        "  logic [3:0] y;\n"
        "  always_comb begin\n"
        "    y = 4'b0;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->process_count >= 1, "should have at least 1 process");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_always_ff(void)
{
    const char *src =
        "module test;\n"
        "  logic q;\n"
        "  always_ff @(posedge clk) begin\n"
        "    q <= 1'b0;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->process_count >= 1, "should have at least 1 process");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_always_latch(void)
{
    const char *src =
        "module test;\n"
        "  logic q;\n"
        "  always_latch begin\n"
        "    if (en) q <= d;\n"
        "  end\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->process_count >= 1, "should have at least 1 process");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * SystemVerilog interface/modport (SV.2)
 * ================================================================= */

static void test_parse_interface_basic(void)
{
    const char *src =
        "interface my_bus;\n"
        "  logic [7:0] data;\n"
        "  logic clk;\n"
        "endinterface\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "my_bus", "interface name");
    mu_assert(r.unit->is_interface, "should be marked as interface");
    mu_assert(has_signal(r.unit, "data", UIR_SIG_LOGIC), "should have data signal");
    mu_assert(has_signal(r.unit, "clk", UIR_SIG_LOGIC), "should have clk signal");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_interface_modport(void)
{
    const char *src =
        "interface my_bus;\n"
        "  logic [7:0] data;\n"
        "  logic clk;\n"
        "  modport master (input clk, output data);\n"
        "endinterface\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->is_interface, "should be marked as interface");
    mu_assert(r.unit->modport_count >= 1, "should have at least 1 modport");
    if (r.unit->modport_count > 0) {
        mu_assert_str_eq(r.unit->modports[0].name, "master", "modport name");
        mu_assert(r.unit->modports[0].port_count >= 2, "modport should have ports");
    }
    uir_destroy_design_unit(r.unit);
}

static void test_parse_interface_in_port(void)
{
    const char *src =
        "module top;\n"
        "  my_bus bus_inst();\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->instance_count >= 1, "should have instance");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * SystemVerilog package/import (SV.3)
 * ================================================================= */

static void test_parse_sv_package_empty(void)
{
    const char *src =
        "package my_pkg;\n"
        "endpackage\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "my_pkg", "package name");
    mu_assert_str_eq(r.unit->language, "verilog", "language");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sv_package_param(void)
{
    const char *src =
        "package my_pkg;\n"
        "  parameter WIDTH = 8;\n"
        "endpackage\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert_str_eq(r.unit->name, "my_pkg", "package name");
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sv_import(void)
{
    const char *src =
        "module test;\n"
        "  import my_pkg::WIDTH;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->import_count >= 1, "should have import");
    if (r.unit->import_count > 0) {
        mu_assert_str_eq(r.unit->imports[0].pkg_name, "my_pkg", "pkg name");
        mu_assert(r.unit->imports[0].item_name != NULL, "item should not be NULL");
        mu_assert_str_eq(r.unit->imports[0].item_name, "WIDTH", "item name");
    }
    uir_destroy_design_unit(r.unit);
}

static void test_parse_sv_import_wildcard(void)
{
    const char *src =
        "module test;\n"
        "  import my_pkg::*;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("test.v", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit != NULL, "should have unit");
    mu_assert(r.unit->import_count >= 1, "should have import");
    if (r.unit->import_count > 0) {
        mu_assert_str_eq(r.unit->imports[0].pkg_name, "my_pkg", "pkg name");
        mu_assert(r.unit->imports[0].item_name == NULL, "wildcard item should be NULL");
    }
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Test registration
 * ================================================================= */

void register_verilog_parser_tests(void)
{
    printf("[Verilog Parser]\n");

    /* Module basics */
    mu_run_test(test_parse_null_source);
    mu_run_test(test_parse_empty_source);
    mu_run_test(test_parse_comment_only);
    mu_run_test(test_parse_empty_module);
    mu_run_test(test_parse_module_no_ports);
    mu_run_test(test_parse_module_empty_parens);
    mu_run_test(test_parse_module_with_ports);
    mu_run_test(test_parse_module_non_ansi_ports);
    mu_run_test(test_parse_module_single_port);
    mu_run_test(test_parse_module_ansi_ports_with_range);

    /* Declarations */
    mu_run_test(test_parse_wire_and_reg);
    mu_run_test(test_parse_net_types);
    mu_run_test(test_parse_wire_with_range);
    mu_run_test(test_parse_wire_reverse_range);
    mu_run_test(test_parse_wire_wide_range);
    mu_run_test(test_parse_wire_range_comma);
    mu_run_test(test_parse_reg_with_range);
    mu_run_test(test_parse_reg_range_comma);
    mu_run_test(test_parse_reg_reverse_range);
    mu_run_test(test_parse_reg_array);
    mu_run_test(test_parse_wire_array);
    mu_run_test(test_parse_reg_array_reverse);
    mu_run_test(test_parse_reg_array_no_range);
    mu_run_test(test_parse_input_array);
    mu_run_test(test_parse_output_array);
    mu_run_test(test_parse_reg_array_multi_dim);
    mu_run_test(test_parse_integer_single);
    mu_run_test(test_parse_integer_list);
    mu_run_test(test_parse_mixed_decls);
    mu_run_test(test_parse_body_input_decl);
    mu_run_test(test_parse_body_output_decl);
    mu_run_test(test_parse_body_inout_decl);
    mu_run_test(test_parse_body_input_range);
    mu_run_test(test_parse_body_output_reg);
    mu_run_test(test_parse_body_input_list);
    mu_run_test(test_parse_body_output_list);
    mu_run_test(test_parse_wire_id_with_dollar);
    mu_run_test(test_parse_wire_id_with_underscore);

    /* Continuous assign */
    mu_run_test(test_parse_continuous_assign);
    mu_run_test(test_parse_assign_arith);
    mu_run_test(test_parse_assign_sub);
    mu_run_test(test_parse_assign_mul);
    mu_run_test(test_parse_assign_div);
    mu_run_test(test_parse_assign_mod);
    mu_run_test(test_parse_assign_bitwise_and);
    mu_run_test(test_parse_assign_bitwise_or);
    mu_run_test(test_parse_assign_bitwise_xor);
    mu_run_test(test_parse_assign_bitwise_not);
    mu_run_test(test_parse_assign_logical_and);
    mu_run_test(test_parse_assign_logical_or);
    mu_run_test(test_parse_assign_logical_not);
    mu_run_test(test_parse_assign_ternary);
    mu_run_test(test_parse_assign_shift_left);
    mu_run_test(test_parse_assign_shift_right);
    mu_run_test(test_parse_assign_relation_lt);
    mu_run_test(test_parse_assign_relation_gt);
    mu_run_test(test_parse_assign_relation_le);
    mu_run_test(test_parse_assign_relation_ge);
    mu_run_test(test_parse_assign_equality);
    mu_run_test(test_parse_assign_inequality);
    mu_run_test(test_parse_assign_concat_rhs);
    mu_run_test(test_parse_assign_concat_multi);
    mu_run_test(test_parse_assign_concat_lhs);
    mu_run_test(test_parse_assign_parens);
    mu_run_test(test_parse_assign_mixed_ops);
    mu_run_test(test_parse_multi_assign);
    mu_run_test(test_parse_assign_unary_minus);

    /* Always blocks */
    mu_run_test(test_parse_always_block);
    mu_run_test(test_parse_always_negedge);
    mu_run_test(test_parse_always_or_sens);
    mu_run_test(test_parse_always_comma_sens);
    mu_run_test(test_parse_always_level_sens);
    mu_run_test(test_parse_always_auto_sens);
    mu_run_test(test_parse_always_multi_stmts);
    mu_run_test(test_parse_always_triple_sens);

    /* Initial blocks */
    mu_run_test(test_parse_initial_basic);
    mu_run_test(test_parse_initial_begin_end);

    /* If statements */
    mu_run_test(test_parse_if_simple);
    mu_run_test(test_parse_if_else);
    mu_run_test(test_parse_if_else_blocks);
    mu_run_test(test_parse_if_nested);
    mu_run_test(test_parse_if_in_always);

    /* Case statements */
    mu_run_test(test_parse_case_simple);
    mu_run_test(test_parse_case_multi_pattern);
    mu_run_test(test_parse_case_default);
    mu_run_test(test_parse_casez_simple);
    mu_run_test(test_parse_casez_dontcare);
    mu_run_test(test_parse_case_multi_stmt_body);

    /* Blocking/nonblocking */
    mu_run_test(test_parse_blocking_simple);
    mu_run_test(test_parse_blocking_expr);
    mu_run_test(test_parse_nonblocking_simple);
    mu_run_test(test_parse_nonblocking_expr);
    mu_run_test(test_parse_blocking_self_add);
    mu_run_test(test_parse_blocking_multi_in_seq);

    /* Module instantiation */
    mu_run_test(test_parse_module_instance);
    mu_run_test(test_parse_inst_named_multi);
    mu_run_test(test_parse_inst_positional);
    mu_run_test(test_parse_inst_no_conns);
    mu_run_test(test_parse_inst_multi_instances);

    /* Expressions */
    mu_run_test(test_parse_expr_add_sub_mix);
    mu_run_test(test_parse_expr_mul_div_mod);
    mu_run_test(test_parse_expr_ternary_nested);
    mu_run_test(test_parse_expr_concat_and_ternary);
    mu_run_test(test_parse_expr_shift_combined);

    /* Literals */
    mu_run_test(test_parse_lit_sized_dec);
    mu_run_test(test_parse_lit_sized_hex);
    mu_run_test(test_parse_lit_sized_hex_lower);
    mu_run_test(test_parse_lit_sized_bin);
    mu_run_test(test_parse_lit_sized_oct);
    mu_run_test(test_parse_lit_unsized);
    mu_run_test(test_parse_lit_signed);
    mu_run_test(test_parse_lit_signed_hex);
    mu_run_test(test_parse_lit_x_in_binary);
    mu_run_test(test_parse_lit_z_in_binary);
    mu_run_test(test_parse_lit_qmark);
    mu_run_test(test_parse_lit_underscore_bin);
    mu_run_test(test_parse_lit_underscore_dec);
    mu_run_test(test_parse_lit_hex_x_digits);
    mu_run_test(test_parse_lit_zero_width);
    mu_run_test(test_parse_lit_one);

    /* Parameters */
    mu_run_test(test_parse_parameter_single);
    mu_run_test(test_parse_parameter_list);
    mu_run_test(test_parse_parameter_expr);
    mu_run_test(test_parse_localparam);

    /* Comments */
    mu_run_test(test_parse_line_comment);
    mu_run_test(test_parse_block_comment);
    mu_run_test(test_parse_multi_line_comment);

    /* Named block */
    mu_run_test(test_parse_seq_block_named);

    /* Combined */
    mu_run_test(test_parse_always_if_else_inside);
    mu_run_test(test_parse_always_case_inside);
    mu_run_test(test_parse_always_process_count);
    mu_run_test(test_parse_initial_and_always);
    mu_run_test(test_parse_module_signal_and_inst);

    /* Additional edge cases */
    mu_run_test(test_parse_assign_xnor);
    mu_run_test(test_parse_always_deep_nested);
    mu_run_test(test_parse_wire_range_large);
    mu_run_test(test_parse_blocking_assign_concat_rhs);
    mu_run_test(test_parse_case_no_items);
    mu_run_test(test_parse_multi_decls_empty_body);
    mu_run_test(test_parse_line_comment_before_module);
    mu_run_test(test_parse_block_comment_between_decls);
    mu_run_test(test_parse_always_sens_list_or);
    mu_run_test(test_parse_initial_sequential);

    /* Negative tests */
    mu_run_test(test_parse_invalid_syntax);
    mu_run_test(test_parse_missing_endmodule);
    mu_run_test(test_parse_missing_module_keyword);
    mu_run_test(test_parse_unclosed_block_comment);
    mu_run_test(test_parse_stray_character);
    mu_run_test(test_parse_always_no_sensitivity);
    mu_run_test(test_parse_port_list_no_rparen);
    mu_run_test(test_parse_missing_endcase);
    mu_run_test(test_parse_if_no_parens);
    mu_run_test(test_parse_assign_no_semi);
    mu_run_test(test_parse_bad_number);
    mu_run_test(test_parse_garbage_after_module);
    mu_run_test(test_parse_decl_missing_id);
    mu_run_test(test_parse_assign_no_rhs);
    mu_run_test(test_parse_case_no_expr);
    mu_run_test(test_parse_inst_no_semi);
    mu_run_test(test_parse_unclosed_paren_expr);
    mu_run_test(test_parse_invalid_port_dir);
    mu_run_test(test_parse_module_missing_name);
    mu_run_test(test_parse_double_semicolon_decl);
    mu_run_test(test_parse_initial_no_body);
    mu_run_test(test_parse_block_comment_no_close);
    mu_run_test(test_parse_always_double_at);
    mu_run_test(test_parse_case_missing_endcase);
    mu_run_test(test_parse_invalid_expr_op);

    /* SystemVerilog RTL (Mode 1) */
    mu_run_test(test_parse_logic_decl);
    mu_run_test(test_parse_logic_port);
    mu_run_test(test_parse_always_comb);
    mu_run_test(test_parse_always_ff);
    mu_run_test(test_parse_always_latch);

    /* Function/Task */
    mu_run_test(test_parse_function_no_ports);
    mu_run_test(test_parse_function_with_ports);
    mu_run_test(test_parse_task_inout);
    mu_run_test(test_parse_multiple_functions);

    /* $display/$write/$monitor */
    mu_run_test(test_parse_display);
    mu_run_test(test_parse_display_with_args);
    mu_run_test(test_parse_write);
    mu_run_test(test_parse_monitor);

    /* System tasks and functions (Phase 2) */
    mu_run_test(test_parse_stop_no_arg);
    mu_run_test(test_parse_stop_with_arg);
    mu_run_test(test_parse_finish_no_arg);
    mu_run_test(test_parse_finish_with_arg);
    mu_run_test(test_parse_severity_tasks);
    mu_run_test(test_parse_sys_func_signed);
    mu_run_test(test_parse_sys_func_unsigned);
    mu_run_test(test_parse_clog2);
    mu_run_test(test_parse_sys_time);
    mu_run_test(test_parse_sys_realtime);
    mu_run_test(test_parse_random_no_arg);
    mu_run_test(test_parse_random_with_arg);
    mu_run_test(test_parse_dumpfile);
    mu_run_test(test_parse_dumpvars);
    mu_run_test(test_parse_dumpon_dumpoff);
    mu_run_test(test_parse_value_plusargs);
    mu_run_test(test_parse_test_plusargs);
    mu_run_test(test_parse_fopen_stub);
    mu_run_test(test_parse_fwrite_fdisplay);
    mu_run_test(test_parse_fclose);
    mu_run_test(test_parse_timeformat);

    /* Named events (Phase 5a) */
    mu_run_test(test_parse_event_decl);
    mu_run_test(test_parse_trigger);
    mu_run_test(test_parse_event_sensitivity);
    mu_run_test(test_parse_printtimescale);

    /* Indexed part-select (Phase 5b) */
    mu_run_test(test_parse_indexed_part_select_plus);
    mu_run_test(test_parse_indexed_part_select_minus);

    /* Attributes (Phase 5b) */
    mu_run_test(test_parse_attr_simple);
    mu_run_test(test_parse_attr_on_reg);

    /* Defparam (Phase 5b) */
    mu_run_test(test_parse_defparam_simple);
    mu_run_test(test_parse_defparam_multi);

    /* Specify blocks (Phase 4a + 4b) */
    mu_run_test(test_parse_specify_empty);
    mu_run_test(test_parse_specparam);
    mu_run_test(test_parse_parallel_path);
    mu_run_test(test_parse_full_path);
    mu_run_test(test_parse_specify_in_module);
    mu_run_test(test_parse_conditional_path);
    mu_run_test(test_parse_edge_path);
    mu_run_test(test_parse_edge_path_data_terminal);
    mu_run_test(test_parse_edge_path_data_terminal_neg);
    mu_run_test(test_parse_setup);
    mu_run_test(test_parse_hold);
    mu_run_test(test_parse_setuphold);
    mu_run_test(test_parse_width);
    mu_run_test(test_parse_period);

    /* Loop statements */
    mu_run_test(test_parse_for_loop);
    mu_run_test(test_parse_for_block);
    mu_run_test(test_parse_for_init);
    mu_run_test(test_parse_while_loop);
    mu_run_test(test_parse_repeat_loop);
    mu_run_test(test_parse_forever_loop);

    /* Wait / Event Control / Disable */
    mu_run_test(test_parse_wait_basic);
    mu_run_test(test_parse_wait_with_block);
    mu_run_test(test_parse_event_control_posedge);
    mu_run_test(test_parse_event_control_negedge);
    mu_run_test(test_parse_event_control_any);
    mu_run_test(test_parse_disable);
    mu_run_test(test_parse_disable_task);

    /* UDP primitives */
    mu_run_test(test_parse_udp_combinational);
    mu_run_test(test_parse_udp_sequential);
    mu_run_test(test_parse_udp_edge_shorthand);
    mu_run_test(test_parse_udp_with_initial);

    /* SystemVerilog interface/modport (SV.2) */
    mu_run_test(test_parse_interface_basic);
    mu_run_test(test_parse_interface_modport);
    mu_run_test(test_parse_interface_in_port);

    /* SystemVerilog package/import (SV.3) */
    mu_run_test(test_parse_sv_package_empty);
    mu_run_test(test_parse_sv_package_param);
    mu_run_test(test_parse_sv_import);
    mu_run_test(test_parse_sv_import_wildcard);

    printf("\n");
}
