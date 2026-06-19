/* Tests for Verilog generate blocks — parse, elaborate, and verify expansion.
 * Covers generate for, generate if, and bare generate...endgenerate.
 */

#include "minunit.h"
#include "libqsim/verilog_parser.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir.h"
#include <string.h>
#include <stdio.h>

/* =================================================================
 * 1. Generate for — parsing
 * ================================================================= */

static void test_generate_for_parse(void)
{
    const char *src =
        "module gen_test;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 4; i = i + 1) begin : gen\n"
        "    wire w;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_for.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);
    mu_assert_str_eq(r.unit->name, "gen_test", "module name");

    /* Should have a generate node */
    mu_assert(r.unit->generate_count > 0, "should have generate nodes");
    mu_assert_not_null(r.unit->generates[0]);
    mu_assert(r.unit->generates[0]->gen_type == UIR_GEN_LOOP,
              "should be GEN_LOOP");
    mu_assert_str_eq(r.unit->generates[0]->label, "gen", "label");
    mu_assert_str_eq(r.unit->generates[0]->genvar_name, "i", "genvar name");
    mu_assert(r.unit->generates[0]->body_item_count > 0, "body items present");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 2. Generate for — elaboration with signals
 * ================================================================= */

static void test_generate_for_signal_elab(void)
{
    const char *src =
        "module gen_sig;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 3; i = i + 1) begin : gen\n"
        "    wire [7:0] w;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_sig.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");
    mu_assert_not_null(r.unit);

    /* Elaborate */
    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");
    mu_assert_eq(elab->diag_count, 0, "no diagnostics");

    /* After elaboration, generate nodes should be expanded.
     * The unit should have 3 signals: gen[0].w, gen[1].w, gen[2].w */
    int found_0 = 0, found_1 = 0, found_2 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) found_0 = 1;
        if (strcmp(name, "gen[1].w") == 0) found_1 = 1;
        if (strcmp(name, "gen[2].w") == 0) found_2 = 1;
    }
    mu_assert(found_0, "gen[0].w exists");
    mu_assert(found_1, "gen[1].w exists");
    mu_assert(found_2, "gen[2].w exists");

    /* generate_count should be 0 (cleared after expansion) */
    mu_assert_eq(r.unit->generate_count, 0, "generates cleared");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 3. Generate for — elaboration with module instances
 * ================================================================= */

static void test_generate_for_instance_elab(void)
{
    const char *src =
        "module leaf(input clk, input rst);\n"
        "  reg q;\n"
        "  always @(posedge clk) q <= rst ? 1'b0 : ~q;\n"
        "endmodule\n";
    parse_result_t leaf_r = verilog_parse("leaf.v", src, strlen(src));
    mu_assert(leaf_r.success, "leaf parse");
    uir_design_unit_t *leaf = leaf_r.unit;

    const char *top_src =
        "module gen_inst(input clk, input rst);\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 4; i = i + 1) begin : gen\n"
        "    leaf u_leaf(.clk(clk), .rst(rst));\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t top_r = verilog_parse("gen_inst.v", top_src, strlen(top_src));
    mu_assert(top_r.success, "top parse");

    uir_design_unit_t *units[] = {leaf, top_r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");

    /* After expansion, top should have 4 instances: gen[0].u_leaf .. gen[3].u_leaf */
    int found[4] = {0, 0, 0, 0};
    char name_buf[64];
    for (size_t i = 0; i < top_r.unit->instance_count; i++) {
        const char *iname = top_r.unit->instances[i]->instance_name;
        for (int j = 0; j < 4; j++) {
            snprintf(name_buf, sizeof(name_buf), "gen[%d].u_leaf", j);
            if (strcmp(iname, name_buf) == 0)
                found[j] = 1;
        }
    }
    mu_assert(found[0], "gen[0].u_leaf exists");
    mu_assert(found[1], "gen[1].u_leaf exists");
    mu_assert(found[2], "gen[2].u_leaf exists");
    mu_assert(found[3], "gen[3].u_leaf exists");

    /* Each instance should be bound to leaf */
    for (size_t i = 0; i < top_r.unit->instance_count; i++) {
        mu_assert_not_null(top_r.unit->instances[i]->bound_to);
        mu_assert(top_r.unit->instances[i]->bound_to == leaf,
                  "bound to leaf module");
    }

    uir_elab_result_free(elab);
    uir_destroy_design_unit(top_r.unit);
    uir_destroy_design_unit(leaf);
}

/* =================================================================
 * 4. Generate for — with always block
 * ================================================================= */

static void test_generate_for_always_elab(void)
{
    const char *src =
        "module gen_always;\n"
        "  input clk;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    reg [7:0] cnt;\n"
        "    always @(posedge clk) cnt <= cnt + 1;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_always.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");

    /* Should have signals gen[0].cnt and gen[1].cnt */
    int found_0 = 0, found_1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].cnt") == 0) found_0 = 1;
        if (strcmp(name, "gen[1].cnt") == 0) found_1 = 1;
    }
    mu_assert(found_0, "gen[0].cnt exists");
    mu_assert(found_1, "gen[1].cnt exists");

    /* Should have 2 processes (one per iteration) */
    mu_assert(r.unit->process_count >= 2, "at least 2 processes");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 5. Generate for — empty body
 * ================================================================= */

static void test_generate_for_empty_body(void)
{
    const char *src =
        "module gen_empty;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 4; i = i + 1) begin : gen\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_empty.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");
    mu_assert(r.unit->generate_count > 0, "has generate");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");
    mu_assert_eq(elab->diag_count, 0, "no diagnostics");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 6. Generate bare (generate...endgenerate with body items)
 * ================================================================= */

static void test_generate_bare(void)
{
    const char *src =
        "module gen_bare;\n"
        "  generate\n"
        "    wire [3:0] gwire;\n"
        "  endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_bare.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");
    mu_assert_not_null(r.unit);

    /* Bare generate items go directly to module, not via generate node */
    mu_assert(r.unit->signal_count > 0, "should have signal");
    int found = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "gwire") == 0) found = 1;
    }
    mu_assert(found, "gwire signal exists");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 7. Genvar declaration parsing
 * ================================================================= */

static void test_genvar_decl(void)
{
    const char *src =
        "module genvar_decl;\n"
        "  genvar i, j, k;\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("genvar_decl.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");
    mu_assert_not_null(r.unit);

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 8. Generate for — genvar outside (out-of-block genvar)
 * ================================================================= */

static void test_generate_for_genvar_outside(void)
{
    const char *src =
        "module gen_outside;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    wire w;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_outside.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");

    int f0 = 0, f1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "gen[0].w") == 0) f0 = 1;
        if (strcmp(r.unit->signals[i]->name, "gen[1].w") == 0) f1 = 1;
    }
    mu_assert(f0, "gen[0].w");
    mu_assert(f1, "gen[1].w");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 9. Generate for — wider iteration range (step > 1)
 * ================================================================= */

static void test_generate_for_wide_range(void)
{
    const char *src =
        "module gen_wide;\n"
        "  genvar i;\n"
        "  generate for (i = 2; i < 6; i = i + 2) begin : g\n"
        "    wire [31:0] data;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_wide.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");

    int f2 = 0, f4 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "g[2].data") == 0) f2 = 1;
        if (strcmp(r.unit->signals[i]->name, "g[4].data") == 0) f4 = 1;
    }
    mu_assert(f2, "g[2].data exists");
    mu_assert(f4, "g[4].data exists");
    mu_assert_eq(r.unit->signal_count, 2, "exactly 2 signals");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 10. Generate for — descending range
 * ================================================================= */

static void test_generate_for_descending(void)
{
    const char *src =
        "module gen_desc;\n"
        "  genvar i;\n"
        "  generate for (i = 3; i > 0; i = i - 1) begin : g\n"
        "    wire x;\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_desc.v", src, strlen(src));
    mu_assert(r.success, "parse succeed");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab succeed");

    int f3 = 0, f2 = 0, f1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "g[3].x") == 0) f3 = 1;
        if (strcmp(r.unit->signals[i]->name, "g[2].x") == 0) f2 = 1;
        if (strcmp(r.unit->signals[i]->name, "g[1].x") == 0) f1 = 1;
    }
    mu_assert(f3, "g[3].x exists");
    mu_assert(f2, "g[2].x exists");
    mu_assert(f1, "g[1].x exists");
    mu_assert_eq(r.unit->signal_count, 3, "exactly 3 signals");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 11. Generate case — simple matching
 * ================================================================= */

static void test_generate_case_simple(void)
{
    const char *src =
        "module gen_case_simple;\n"
        "  generate case (1)\n"
        "    0: begin : a\n"
        "      wire w;\n"
        "    end\n"
        "    1: begin : b\n"
        "      wire x;\n"
        "    end\n"
        "  endcase endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_simple.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    /* Parse-time case: matching items go directly into the module
     * with raw signal names (no prefix — that happens during elaboration). */
    int found_w = 0, found_x = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "w") == 0) found_w = 1;
        if (strcmp(name, "x") == 0) found_x = 1;
    }
    mu_assert(!found_w, "branch 0 (wire w) should be suppressed");
    mu_assert(found_x, "branch 1 (wire x) should be emitted");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 12. Generate case — default
 * ================================================================= */

static void test_generate_case_default(void)
{
    const char *src =
        "module gen_case_default;\n"
        "  generate case (2)\n"
        "    0: begin : a\n"
        "      wire w;\n"
        "    end\n"
        "    default: begin : d\n"
        "      wire y;\n"
        "    end\n"
        "  endcase endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_default.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    int found_w = 0, found_y = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "w") == 0) found_w = 1;
        if (strcmp(name, "y") == 0) found_y = 1;
    }
    mu_assert(!found_w, "branch 0 (wire w) should be suppressed");
    mu_assert(found_y, "default branch (wire y) should be emitted");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 13. Generate case — first match wins
 * ================================================================= */

static void test_generate_case_first_match(void)
{
    const char *src =
        "module gen_case_first;\n"
        "  generate case (0)\n"
        "    0: begin : a\n"
        "      wire w;\n"
        "    end\n"
        "    0: begin : b\n"
        "      wire x;\n"
        "    end\n"
        "  endcase endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_first.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    /* Only first matching branch emitted */
    int found_w = 0, found_x = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "w") == 0) found_w = 1;
        if (strcmp(name, "x") == 0) found_x = 1;
    }
    mu_assert(found_w, "first matching branch (wire w) should be emitted");
    mu_assert(!found_x, "second matching branch (wire x) should be suppressed");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 14. Generate case — multiple comma-separated patterns
 * ================================================================= */

static void test_generate_case_multiple_patterns(void)
{
    const char *src =
        "module gen_case_multi;\n"
        "  generate case (1)\n"
        "    1, 2: begin : m\n"
        "      wire w;\n"
        "    end\n"
        "    3: begin : n\n"
        "      wire x;\n"
        "    end\n"
        "  endcase endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_multi.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    int found_w = 0, found_x = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "w") == 0) found_w = 1;
        if (strcmp(name, "x") == 0) found_x = 1;
    }
    mu_assert(found_w, "branch matching 1 (wire w) should be emitted");
    mu_assert(!found_x, "non-matching branch (wire x) should be suppressed");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 15. Generate case — no match (all suppressed)
 * ================================================================= */

static void test_generate_case_no_match(void)
{
    const char *src =
        "module gen_case_nomatch;\n"
        "  generate case (5)\n"
        "    0: begin : a\n"
        "      wire w;\n"
        "    end\n"
        "    1: begin : b\n"
        "      wire x;\n"
        "    end\n"
        "  endcase endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_nomatch.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    /* No branch matches — nothing from generate should appear */
    int found = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "w") == 0 || strcmp(name, "x") == 0)
            found = 1;
    }
    mu_assert(!found, "no branch should be emitted");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 16. Nested generate — bare generate inside generate for
 * ================================================================= */

static void test_generate_nested_bare(void)
{
    const char *src =
        "module gen_nested_bare;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    generate\n"
        "      wire w;\n"
        "    endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_nested_bare.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);
    mu_assert(r.unit->generate_count > 0, "should have generate nodes");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab should succeed");
    mu_assert_eq(elab->diag_count, 0, "no diagnostics");

    /* Should have gen[0].w and gen[1].w */
    int f0 = 0, f1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) f0 = 1;
        if (strcmp(name, "gen[1].w") == 0) f1 = 1;
    }
    mu_assert(f0, "gen[0].w exists");
    mu_assert(f1, "gen[1].w exists");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 17. Nested generate — generate if inside generate for
 * ================================================================= */

static void test_generate_nested_if(void)
{
    const char *src =
        "module gen_nested_if;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    generate if (1) begin : inner\n"
        "      wire w;\n"
        "    end endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_nested_if.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab should succeed");

    /* gen[0].w and gen[1].w should exist (inner label restored via stack) */
    int f0 = 0, f1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) f0 = 1;
        if (strcmp(name, "gen[1].w") == 0) f1 = 1;
    }
    mu_assert(f0, "gen[0].w exists");
    mu_assert(f1, "gen[1].w exists");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 18. Nested generate — generate for inside generate for (parsing)
 * ================================================================= */

static void test_generate_nested_for_parse(void)
{
    const char *src =
        "module gen_nested_for;\n"
        "  genvar i, j;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : outer\n"
        "    generate for (j = 0; j < 2; j = j + 1) begin : inner\n"
        "      wire w;\n"
        "    end endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_nested_for.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);
    mu_assert(r.unit->generate_count > 0, "should have generate nodes");

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 19. Generate for with genvar-dependent generate-if
 * ================================================================= */

static void test_generate_for_genvar_if(void)
{
    const char *src =
        "module gen_for_genvar_if;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    generate if (i == 0) begin : inner\n"
        "      wire w;\n"
        "    end endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_for_genvar_if.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab should succeed");

    /* gen[0].w should exist (i==0 true for iteration 0) */
    /* gen[1].w should NOT exist (i==0 false for iteration 1 with no else) */
    int f0 = 0, f1 = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) f0 = 1;
        if (strcmp(name, "gen[1].w") == 0) f1 = 1;
    }
    mu_assert(f0, "gen[0].w should exist");
    mu_assert(!f1, "gen[1].w should NOT exist");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 20. Generate for with genvar-dependent generate-if-else
 * ================================================================= */

static void test_generate_for_genvar_if_else(void)
{
    const char *src =
        "module gen_for_genvar_if_else;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 2; i = i + 1) begin : gen\n"
        "    generate if (i == 0) begin : a\n"
        "      wire w;\n"
        "    end else begin : b\n"
        "      wire x;\n"
        "    end endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_for_genvar_if_else.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab should succeed");

    /* gen[0].w from TRUE branch, gen[1].x from ELSE branch */
    int f0w = 0, f1x = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) f0w = 1;
        if (strcmp(name, "gen[1].x") == 0) f1x = 1;
    }
    mu_assert(f0w, "gen[0].w should exist (TRUE branch)");
    mu_assert(f1x, "gen[1].x should exist (ELSE branch)");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 21. Generate case inside generate-for (template-mode elaboration)
 * ================================================================= */

static void test_generate_case_in_for(void)
{
    const char *src =
        "module gen_case_in_for;\n"
        "  genvar i;\n"
        "  generate for (i = 0; i < 3; i = i + 1) begin : gen\n"
        "    generate case (i)\n"
        "      0: begin : a\n"
        "        wire w;\n"
        "      end\n"
        "      1: begin : b\n"
        "        wire x;\n"
        "      end\n"
        "      default: begin : c\n"
        "        wire y;\n"
        "      end\n"
        "    endcase endgenerate\n"
        "  end endgenerate\n"
        "endmodule\n";
    parse_result_t r = verilog_parse("gen_case_in_for.v", src, strlen(src));
    mu_assert(r.success, "parse should succeed");
    mu_assert_not_null(r.unit);
    mu_assert(r.unit->generate_count > 0, "has generate nodes");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert_not_null(elab);
    mu_assert(elab->success, "elab should succeed");
    mu_assert_eq(elab->diag_count, 0, "no diagnostics");

    /* Iteration 0 (i==0): match branch 0 -> gen[0].w */
    /* Iteration 1 (i==1): match branch 1 -> gen[1].x */
    /* Iteration 2 (i==2): default -> gen[2].y */
    int f0w = 0, f1x = 0, f2y = 0;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        const char *name = r.unit->signals[i]->name;
        if (strcmp(name, "gen[0].w") == 0) f0w = 1;
        if (strcmp(name, "gen[1].x") == 0) f1x = 1;
        if (strcmp(name, "gen[2].y") == 0) f2y = 1;
    }
    mu_assert(f0w, "gen[0].w exists (i==0 -> branch 0)");
    mu_assert(f1x, "gen[1].x exists (i==1 -> branch 1)");
    mu_assert(f2y, "gen[2].y exists (i==2 -> default)");

    uir_elab_result_free(elab);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Register
 * ================================================================= */

void register_generate_tests(void)
{
    printf("[Generate]\n");
    mu_run_test(test_generate_for_parse);
    mu_run_test(test_generate_for_signal_elab);
    mu_run_test(test_generate_for_instance_elab);
    mu_run_test(test_generate_for_always_elab);
    mu_run_test(test_generate_for_empty_body);
    mu_run_test(test_generate_bare);
    mu_run_test(test_genvar_decl);
    mu_run_test(test_generate_for_genvar_outside);
    mu_run_test(test_generate_for_wide_range);
    mu_run_test(test_generate_for_descending);
    mu_run_test(test_generate_case_simple);
    mu_run_test(test_generate_case_default);
    mu_run_test(test_generate_case_first_match);
    mu_run_test(test_generate_case_multiple_patterns);
    mu_run_test(test_generate_case_no_match);
    mu_run_test(test_generate_nested_bare);
    mu_run_test(test_generate_nested_if);
    mu_run_test(test_generate_nested_for_parse);
    mu_run_test(test_generate_for_genvar_if);
    mu_run_test(test_generate_for_genvar_if_else);

    /* Generate case inside generate-for (Phase 5b template-mode) */
    mu_run_test(test_generate_case_in_for);
    printf("\n");
}
