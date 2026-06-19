#include "libqsim/verilog_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* Test A: two modules */
    const char *src_a =
        "module adder(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  assign y = a & b;\n"
        "endmodule\n"
        "module top(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  adder u1(.y(y), .a(a), .b(b));\n"
        "endmodule\n";

    printf("=== Test A: two modules ===\n");
    parse_result_t pr = verilog_parse("two_mod.v", src_a, strlen(src_a));
    printf("success=%d unit_count=%zu error_count=%d\n",
           pr.success, pr.unit_count, pr.error_count);
    if (pr.error_count > 0) {
        for (int i = 0; i < pr.error_count; i++)
            printf("  error: %s\n", pr.errors[i].message);
    }
    if (pr.success && pr.units) {
        for (size_t i = 0; i < pr.unit_count; i++)
            printf("  unit[%zu] = %s\n", i, pr.units[i]->name);
        for (size_t i = 0; i < pr.unit_count; i++)
            uir_destroy_design_unit(pr.units[i]);
        free(pr.units);
    }

    /* Test B: primitive only */
    const char *src_b =
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

    printf("\n=== Test B: primitive only ===\n");
    pr = verilog_parse("prim.v", src_b, strlen(src_b));
    printf("success=%d unit_count=%zu error_count=%d\n",
           pr.success, pr.unit_count, pr.error_count);
    if (pr.error_count > 0) {
        for (int i = 0; i < pr.error_count; i++)
            printf("  error: %s\n", pr.errors[i].message);
    }
    if (pr.success && pr.units) {
        for (size_t i = 0; i < pr.unit_count; i++)
            printf("  unit[%zu] = %s\n", i, pr.units[i]->name);
        for (size_t i = 0; i < pr.unit_count; i++)
            uir_destroy_design_unit(pr.units[i]);
        free(pr.units);
    }

    /* Test C: module only with same instantiation pattern */
    const char *src_c =
        "module top(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  adder u1(.y(y), .a(a), .b(b));\n"
        "endmodule\n";

    printf("\n=== Test C: module only ===\n");
    pr = verilog_parse("top.v", src_c, strlen(src_c));
    printf("success=%d unit_count=%zu error_count=%d\n",
           pr.success, pr.unit_count, pr.error_count);
    if (pr.error_count > 0) {
        for (int i = 0; i < pr.error_count; i++)
            printf("  error: %s\n", pr.errors[i].message);
    }
    if (pr.success && pr.units) {
        for (size_t i = 0; i < pr.unit_count; i++)
            printf("  unit[%zu] = %s\n", i, pr.units[i]->name);
        for (size_t i = 0; i < pr.unit_count; i++)
            uir_destroy_design_unit(pr.units[i]);
        free(pr.units);
    }

    /* Test D: primitive + module (the failing case) */
    const char *src_d =
        "primitive my_and(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  table\n"
        "    0 0 : 0;\n"
        "    0 1 : 0;\n"
        "    1 0 : 0;\n"
        "    1 1 : 1;\n"
        "  endtable\n"
        "endprimitive\n"
        "module top(y, a, b);\n"
        "  output y;\n"
        "  input a, b;\n"
        "  my_and u1(.y(y), .a(a), .b(b));\n"
        "endmodule\n";

    printf("\n=== Test D: primitive + module ===\n");
    pr = verilog_parse("combined.v", src_d, strlen(src_d));
    printf("success=%d unit_count=%zu error_count=%d\n",
           pr.success, pr.unit_count, pr.error_count);
    if (pr.error_count > 0) {
        for (int i = 0; i < pr.error_count; i++)
            printf("  error: %s\n", pr.errors[i].message);
    }
    if (pr.success && pr.units) {
        for (size_t i = 0; i < pr.unit_count; i++)
            printf("  unit[%zu] = %s\n", i, pr.units[i]->name);
        for (size_t i = 0; i < pr.unit_count; i++)
            uir_destroy_design_unit(pr.units[i]);
        free(pr.units);
    }

    return 0;
}
