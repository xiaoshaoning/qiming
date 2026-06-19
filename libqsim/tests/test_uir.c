#include "libqsim/uir.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

static void test_create_design_unit(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    mu_assert_not_null(unit);
    mu_assert_str_eq(unit->name, "top", "name");
    mu_assert_str_eq(unit->language, "verilog", "language");
    mu_assert_eq(unit->port_count, 0, "0 ports");
    mu_assert_eq(unit->signal_count, 0, "0 signals");
    mu_assert(unit->node_count >= 1, "has self node");
    uir_destroy_design_unit(unit);
}

static void test_add_port(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_port_t *p = uir_add_port(unit, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    mu_assert_not_null(p);
    mu_assert_str_eq(p->name, "clk", "port name");
    mu_assert(p->direction == UIR_PORT_IN, "direction input");
    mu_assert_eq(p->width, 1, "width 1");
    mu_assert_eq(unit->port_count, 1, "1 port in unit");

    uir_port_t *p2 = uir_add_port(unit, "data", UIR_PORT_IN, 7, 0, UIR_SIG_WIRE);
    mu_assert_eq(p2->width, 8, "vector width 8");
    mu_assert(p2->is_vector, "is vector");
    mu_assert_eq(unit->port_count, 2, "2 ports in unit");

    uir_destroy_design_unit(unit);
}

static void test_add_signal(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_signal_t *s = uir_add_signal(unit, "count", UIR_SIG_REG, 8, 0);
    mu_assert_not_null(s);
    mu_assert_str_eq(s->name, "count", "signal name");
    mu_assert(s->sig_type == UIR_SIG_REG, "type reg");
    mu_assert_eq(s->width, 8, "width 8");
    mu_assert_eq(unit->signal_count, 1, "1 signal");
    uir_destroy_design_unit(unit);
}

static void test_add_process(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_process_t *proc = uir_add_process(unit, UIR_PROC_ALWAYS);
    mu_assert_not_null(proc);
    mu_assert(proc->proc_kind == UIR_PROC_ALWAYS, "always");
    mu_assert_eq(unit->process_count, 1, "1 process");
    uir_destroy_design_unit(unit);
}

static void test_add_instance(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_instance_t *inst = uir_add_instance(unit, "u1", "counter");
    mu_assert_not_null(inst);
    mu_assert_str_eq(inst->instance_name, "u1", "inst name");
    mu_assert_str_eq(inst->module_name, "counter", "module name");
    mu_assert_eq(unit->instance_count, 1, "1 instance");
    uir_destroy_design_unit(unit);
}

static void test_expr_binary(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);

    qsim_bit_vector_t *va = qsim_bit_vector_from_state(4, QSIM_1);
    qsim_bit_vector_t *vb = qsim_bit_vector_from_state(4, QSIM_0);
    uir_literal_t *a = uir_make_literal(unit, va, loc);
    uir_literal_t *b = uir_make_literal(unit, vb, loc);
    mu_assert_not_null(a);
    mu_assert_not_null(b);

    uir_expr_t *e = uir_make_binary(unit, UIR_OP_ADD, (uir_node_t *)a, (uir_node_t *)b, loc);
    mu_assert_not_null(e);
    mu_assert(e->is_binary, "is binary");
    mu_assert(e->op.bin_op == UIR_OP_ADD, "op add");

    size_t fi_count;
    uir_node_t **fi = uir_get_fan_in((uir_node_t *)e, &fi_count);
    mu_assert_eq(fi_count, 2, "2 fan-in");

    size_t fo_count;
    uir_node_t **fo = uir_get_fan_out((uir_node_t *)a, &fo_count);
    mu_assert_eq(fo_count, 1, "1 fan-out");
    mu_assert(fo[0] == (uir_node_t *)e, "fan-out to expr");

    uir_destroy_design_unit(unit);
}

static void test_make_literal(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    qsim_bit_vector_t *v = qsim_bit_vector_from_str("'b1010");
    uir_literal_t *lit = uir_make_literal(unit, v, loc);
    mu_assert_not_null(lit);
    mu_assert_eq(lit->width, 4, "width");
    uir_destroy_design_unit(unit);
}

static void test_find_signal(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_signal_t *s = uir_add_signal(unit, "clk", UIR_SIG_WIRE, 1, 0);
    mu_assert_not_null(s);

    uir_node_t *found = uir_find_signal(unit, "clk");
    mu_assert_not_null(found);
    mu_assert(found->kind == UIR_SIGNAL, "found is signal");

    found = uir_find_signal(unit, "nonexistent");
    mu_assert_null(found);

    uir_destroy_design_unit(unit);
}

static void test_trace_drivers(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);

    qsim_bit_vector_t *va = qsim_bit_vector_from_state(1, QSIM_1);
    uir_literal_t *lit = uir_make_literal(unit, va, loc);
    uir_signal_t *sig = uir_add_signal(unit, "out", UIR_SIG_WIRE, 1, 0);

    uir_add_fan_in((uir_node_t *)sig, (uir_node_t *)lit);
    uir_add_fan_out((uir_node_t *)lit, (uir_node_t *)sig);

    size_t count;
    uir_node_t **drivers = uir_trace_drivers((uir_node_t *)sig, &count, 2);
    mu_assert(count >= 1, "drivers found");
    free(drivers);

    uir_destroy_design_unit(unit);
}

static void test_to_json(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_add_port(unit, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_signal(unit, "count", UIR_SIG_REG, 8, 0);

    char *json = uir_to_json(unit);
    mu_assert_not_null(json);
    mu_assert(strstr(json, "design_unit") != NULL, "has design_unit");
    mu_assert(strstr(json, "top") != NULL, "has name");
    mu_assert(strstr(json, "count") != NULL, "has signal");
    free(json);

    uir_destroy_design_unit(unit);
}

static void walk_counter(uir_node_t *node, void *ctx)
{
    (void)node;
    (*(int *)ctx)++;
}

static void test_walk(void)
{
    uir_loc_t loc = {"test.v", 1, 1};
    uir_design_unit_t *unit = uir_create_design_unit("top", "verilog", loc);
    uir_add_port(unit, "clk", UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    uir_add_signal(unit, "q", UIR_SIG_REG, 1, 0);
    uir_add_process(unit, UIR_PROC_ALWAYS);

    int count = 0;
    uir_walk(unit, walk_counter, &count);
    mu_assert(count > 0, "walk visits nodes");

    uir_destroy_design_unit(unit);
}

void register_uir_tests(void)
{
    printf("[UIR]\n");
    mu_run_test(test_create_design_unit);
    mu_run_test(test_add_port);
    mu_run_test(test_add_signal);
    mu_run_test(test_add_process);
    mu_run_test(test_add_instance);
    mu_run_test(test_expr_binary);
    mu_run_test(test_make_literal);
    mu_run_test(test_find_signal);
    mu_run_test(test_trace_drivers);
    mu_run_test(test_to_json);
    mu_run_test(test_walk);
    printf("\n");
}
