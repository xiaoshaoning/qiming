#include "libqsim/elaboration.h"
#include "libqsim/uir.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

static void test_elab_empty_units(void)
{
    uir_elab_result_t *r = uir_elaborate(NULL, 0);
    mu_assert_not_null(r);
    mu_assert(r->success, "empty elaborate succeeds");
    uir_elab_result_free(r);
}

static void test_elab_null_units_list(void)
{
    uir_elab_result_t *r = uir_elaborate(NULL, 5);
    mu_assert_not_null(r);
    mu_assert(r->success, "null list succeeds");
    uir_elab_result_free(r);
}

static void test_elab_instance_binding(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    /* Create counter module */
    uir_design_unit_t *counter = uir_create_design_unit("counter", "verilog", loc);
    uir_add_port(counter, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_port(counter, "rst", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_signal(counter, "count", UIR_SIG_REG, 8, 0);

    /* Create top module with instance of counter */
    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(top, "u1", "counter");
    inst->connections = calloc(1, sizeof(uir_port_connection_t));
    inst->connection_count = 1;
    inst->connections[0].formal_name = "clk";

    uir_design_unit_t *units[] = {counter, top};
    uir_elab_result_t *r = uir_elaborate(units, 2);
    mu_assert_not_null(r);
    mu_assert(r->success, "binding should succeed");

    /* Verify instance got bound */
    mu_assert_not_null(inst->bound_to);
    mu_assert(inst->bound_to == counter, "bound to counter module");

    uir_elab_result_free(r);
    /* uir_destroy_design_unit handles freeing connections array */
    uir_destroy_design_unit(counter);
    uir_destroy_design_unit(top);
}

static void test_elab_port_validation_ok(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    uir_design_unit_t *counter = uir_create_design_unit("counter", "verilog", loc);
    uir_add_port(counter, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_port(counter, "rst", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);

    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(top, "u1", "counter");
    inst->connections = calloc(2, sizeof(uir_port_connection_t));
    inst->connection_count = 2;
    inst->connections[0].formal_name = "clk";
    inst->connections[1].formal_name = "rst";

    uir_design_unit_t *units[] = {counter, top};
    uir_elab_result_t *r = uir_elaborate(units, 2);
    mu_assert_not_null(r);
    mu_assert(r->success, "valid ports should succeed");
    mu_assert_eq(r->diag_count, 0, "no diagnostics");

    uir_elab_result_free(r);
    uir_destroy_design_unit(counter);
    uir_destroy_design_unit(top);
}

static void test_elab_port_validation_error(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    uir_design_unit_t *counter = uir_create_design_unit("counter", "verilog", loc);
    uir_add_port(counter, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_port(counter, "rst", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);

    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(top, "u1", "counter");
    inst->connections = calloc(1, sizeof(uir_port_connection_t));
    inst->connection_count = 1;
    inst->connections[0].formal_name = "nonexistent_port";

    uir_design_unit_t *units[] = {counter, top};
    uir_elab_result_t *r = uir_elaborate(units, 2);
    mu_assert_not_null(r);
    mu_assert(!r->success, "bad port should fail");
    mu_assert(r->diag_count > 0, "should have diagnostic");
    mu_assert(strstr(r->diagnostics[0], "nonexistent_port") != NULL,
              "diagnostic mentions bad port");

    uir_elab_result_free(r);
    uir_destroy_design_unit(counter);
    uir_destroy_design_unit(top);
}

static void test_elab_instance_not_found(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(top, "u1", "nonexistent_module");

    uir_design_unit_t *units[] = {top};
    uir_elab_result_t *r = uir_elaborate(units, 1);
    mu_assert_not_null(r);
    mu_assert(!r->success, "missing module should fail");
    mu_assert(r->diag_count > 0, "should have diagnostic");
    mu_assert(strstr(r->diagnostics[0], "nonexistent_module") != NULL,
              "diagnostic mentions module");
    mu_assert_null(inst->bound_to);

    uir_elab_result_free(r);
    uir_destroy_design_unit(top);
}

static void test_elab_hier_signal_lookup(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    /* Create counter with a signal */
    uir_design_unit_t *counter = uir_create_design_unit("counter", "verilog", loc);
    uir_signal_t *count_sig = uir_add_signal(counter, "count", UIR_SIG_REG, 8, 0);

    /* Create top with instance of counter */
    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(top, "u1", "counter");

    /* Bind manually (skip elaborate for this test) */
    inst->bound_to = counter;
    uir_add_child(top, counter);

    /* Look up hierarchical signal */
    uir_node_t *found = uir_find_signal_hier(top, "u1.count");
    mu_assert_not_null(found);
    mu_assert(found->kind == UIR_SIGNAL, "found is a signal");
    mu_assert(found == (uir_node_t *)count_sig, "correct signal");

    /* Non-existent path */
    found = uir_find_signal_hier(top, "u1.nonexistent");
    mu_assert_null(found);

    /* Non-existent instance */
    found = uir_find_signal_hier(top, "bad.count");
    mu_assert_null(found);

    /* Simple (non-hierarchical) lookup */
    uir_add_signal(top, "clk", UIR_SIG_WIRE, 1, 0);
    found = uir_find_signal_hier(top, "clk");
    mu_assert_not_null(found);
    mu_assert(found->kind == UIR_SIGNAL, "simple lookup finds signal");

    /* NULL safety */
    found = uir_find_signal_hier(NULL, "foo");
    mu_assert_null(found);
    found = uir_find_signal_hier(top, NULL);
    mu_assert_null(found);

    uir_destroy_design_unit(top);
    uir_destroy_design_unit(counter);
}

static void test_elab_result_free_null(void)
{
    uir_elab_result_free(NULL);
}

static void test_elab_multiple_instances(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    /* Create leaf module */
    uir_design_unit_t *leaf = uir_create_design_unit("leaf", "verilog", loc);
    uir_add_signal(leaf, "q", UIR_SIG_WIRE, 1, 0);

    /* Create middle module with two leaf instances */
    uir_design_unit_t *middle = uir_create_design_unit("middle", "verilog", loc);
    uir_instance_t *inst_a = uir_add_instance(middle, "a", "leaf");
    uir_instance_t *inst_b = uir_add_instance(middle, "b", "leaf");

    /* Create top with middle instance */
    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *top_inst = uir_add_instance(top, "m", "middle");

    uir_design_unit_t *units[] = {leaf, middle, top};
    uir_elab_result_t *r = uir_elaborate(units, 3);
    mu_assert_not_null(r);
    mu_assert(r->success, "multi-level binding succeeds");

    /* Check all bindings */
    mu_assert(inst_a->bound_to == leaf, "middle.a binds to leaf");
    mu_assert(inst_b->bound_to == leaf, "middle.b binds to leaf");
    mu_assert(top_inst->bound_to == middle, "top.m binds to middle");

    /* Hierarchical lookup through two levels */
    uir_node_t *found = uir_find_signal_hier(top, "m.a.q");
    mu_assert_not_null(found);
    mu_assert(found->kind == UIR_SIGNAL, "2-level hier lookup");
    mu_assert(found == (uir_node_t *)uir_find_signal(leaf, "q"),
              "points to leaf.q");

    uir_elab_result_free(r);
    uir_destroy_design_unit(top);
    uir_destroy_design_unit(middle);
    uir_destroy_design_unit(leaf);
}

static void test_elab_recovery_instance_not_found(void)
{
    uir_loc_t loc = {"test.v", 1, 1};

    /* Create a known module that could be suggested */
    uir_design_unit_t *counter = uir_create_design_unit("counter", "verilog", loc);
    uir_design_unit_t *adder = uir_create_design_unit("adder", "verilog", loc);

    /* Top instantiates a mispelled module */
    uir_design_unit_t *top = uir_create_design_unit("top", "verilog", loc);
    uir_add_instance(top, "u1", "countr"); /* typo: should be 'counter' */

    uir_design_unit_t *units[] = {counter, adder, top};
    uir_elab_result_t *r = uir_elaborate(units, 3);
    mu_assert_not_null(r);
    mu_assert(!r->success, "typo module should fail");
    mu_assert(r->diag_count > 0, "should have diagnostic");
    mu_assert(r->recoveries != NULL, "recoveries array present");
    mu_assert(r->recoveries[0] != NULL, "recovery hints for typo");
    mu_assert(r->recoveries[0]->suggestion_count >= 1, "at least 1 suggestion");
    mu_assert(strcmp(r->recoveries[0]->suggestions[0], "counter") == 0 ||
              strcmp(r->recoveries[0]->suggestions[0], "adder") == 0,
              "suggests existing module");

    uir_elab_result_free(r);
    uir_destroy_design_unit(counter);
    uir_destroy_design_unit(adder);
    uir_destroy_design_unit(top);
}

void register_elaboration_tests(void)
{
    printf("[Elaboration]\n");
    mu_run_test(test_elab_empty_units);
    mu_run_test(test_elab_null_units_list);
    mu_run_test(test_elab_instance_binding);
    mu_run_test(test_elab_port_validation_ok);
    mu_run_test(test_elab_port_validation_error);
    mu_run_test(test_elab_instance_not_found);
    mu_run_test(test_elab_hier_signal_lookup);
    mu_run_test(test_elab_result_free_null);
    mu_run_test(test_elab_multiple_instances);
    mu_run_test(test_elab_recovery_instance_not_found);
    printf("\n");
}
