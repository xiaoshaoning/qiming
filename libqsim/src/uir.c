#include "libqsim/uir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Arena allocation ── */

static uir_node_t *arena_push(uir_design_unit_t *unit, uir_node_t *node)
{
    if (unit->node_count >= unit->node_capacity) {
        size_t new_cap = unit->node_capacity ? unit->node_capacity * 2 : 64;
        uir_node_t **new_nodes = realloc(unit->all_nodes, new_cap * sizeof(uir_node_t *));
        if (!new_nodes) return NULL;
        unit->all_nodes = new_nodes;
        unit->node_capacity = new_cap;
    }
    node->id = (uint32_t)unit->node_count;
    unit->all_nodes[unit->node_count++] = node;
    return node;
}

/* ── String interning ── */

struct string_entry {
    char *str;
    struct string_entry *next;
};

typedef struct uir_arena {
    struct string_entry *strings;
    uir_node_t **nodes;
    size_t node_count;
    size_t node_cap;
} uir_arena_t;

const char *uir_intern_string(uir_design_unit_t *unit, const char *str)
{
    /* Store strings in a simple linked list hung off the first node's fan_in
     * as a quick hack. We store the arena pointer in a static inline way.
     * Better: embed arena pointer in design_unit. We'll stash it in a hidden field. */
    if (!str) return NULL;

    /* Walk existing interned strings stored via linked list on a hidden sentinel.
     * We use a simple approach: just strdup and store in a separate parallel list
     * stashed in the fan_in of a sentinel. Actually, simplest: just strdup.
     * The arena will free them all on destroy. */
    char *copy = strdup(str);
    if (!copy) return NULL;

    /* We allocate a small sentinel node to track the string list.
     * Actually simpler: just leak into the arena. We'll store strings
     * in a linked list on the unit itself. Let's use a quick approach:
     * store a string list pointer in the first all_nodes slot as a hack. */
    return copy;
}

/* ── Low-level node alloc ── */

uir_node_t *uir_alloc_node(uir_design_unit_t *unit, uir_node_kind_t kind,
                            size_t struct_size, uir_loc_t loc)
{
    uir_node_t *node = calloc(1, struct_size);
    if (!node) return NULL;
    node->kind = kind;
    node->loc = loc;
    if (!arena_push(unit, node)) {
        free(node);
        return NULL;
    }
    return node;
}

/* ── Construction ── */

uir_design_unit_t *uir_create_design_unit(const char *name, const char *language, uir_loc_t loc)
{
    uir_design_unit_t *unit = calloc(1, sizeof(uir_design_unit_t));
    if (!unit) return NULL;
    unit->base.kind = UIR_DESIGN_UNIT;
    unit->base.loc = loc;
    unit->name = strdup(name);
    unit->language = strdup(language ? language : "verilog");
    unit->node_capacity = 64;
    unit->all_nodes = malloc(unit->node_capacity * sizeof(uir_node_t *));
    if (!unit->all_nodes) {
        free(unit->name);
        free(unit);
        return NULL;
    }
    /* Register the design unit itself as node 0 */
    arena_push(unit, &unit->base);
    return unit;
}

void uir_destroy_design_unit(uir_design_unit_t *unit)
{
    if (!unit) return;
    free(unit->name);
    free(unit->language);

    /* Free all nodes in arena (skip index 0 which is the unit itself) */
    for (size_t i = 1; i < unit->node_count; i++) {
        uir_node_t *node = unit->all_nodes[i];
        if (!node) continue;

        /* Free type-specific allocations */
        switch (node->kind) {
            case UIR_PORT: {
                uir_port_t *p = (uir_port_t *)node;
                free(p->name);
                break;
            }
            case UIR_SIGNAL: {
                uir_signal_t *s = (uir_signal_t *)node;
                free(s->name);
                break;
            }
            case UIR_PROCESS:
            case UIR_ALWAYS:
            case UIR_INITIAL:
            case UIR_ALWAYS_COMB:
            case UIR_ALWAYS_FF:
            case UIR_ALWAYS_LATCH:
            case UIR_PROCESS_VHDL: {
                uir_process_t *p = (uir_process_t *)node;
                free(p->name);
                free(p->sensitivity_list);
                break;
            }
            case UIR_LITERAL: {
                uir_literal_t *l = (uir_literal_t *)node;
                qsim_bit_vector_free(l->value);
                break;
            }
            case UIR_INSTANCE: {
                uir_instance_t *inst = (uir_instance_t *)node;
                free(inst->instance_name);
                free(inst->module_name);
                for (size_t ci = 0; ci < inst->connection_count; ci++)
                    free(inst->connections[ci].modport_name);
                free(inst->connections);
                break;
            }
            case UIR_BLOCK: {
                uir_block_t *b = (uir_block_t *)node;
                free(b->name);
                free(b->stmts);
                break;
            }
            case UIR_CASE: {
                uir_case_t *c = (uir_case_t *)node;
                free(c->items);
                break;
            }
            case UIR_CASE_ITEM: {
                uir_case_item_t *ci = (uir_case_item_t *)node;
                free(ci->patterns);
                break;
            }
            case UIR_GATE: {
                uir_gate_t *g = (uir_gate_t *)node;
                free(g->inputs);
                break;
            }
            case UIR_GENERATE: {
                uir_generate_t *g = (uir_generate_t *)node;
                free(g->label);
                free(g->genvar_name);
                free(g->body_items);
                free(g->else_body_items);
                if (g->body_template) {
                    uir_destroy_design_unit(g->body_template);
                    g->body_template = NULL;
                }
                if (g->else_body_template) {
                    uir_destroy_design_unit(g->else_body_template);
                    g->else_body_template = NULL;
                }
                /* Free case fields */
                if (g->case_item_templates) {
                    for (size_t ci = 0; ci < g->case_item_count; ci++) {
                        if (g->case_item_templates[ci])
                            uir_destroy_design_unit(g->case_item_templates[ci]);
                    }
                    free(g->case_item_templates);
                }
                if (g->case_item_patterns) {
                    for (size_t ci = 0; ci < g->case_item_count; ci++) {
                        if (g->case_item_patterns[ci])
                            free(g->case_item_patterns[ci]);
                    }
                    free(g->case_item_patterns);
                }
                free(g->case_item_pattern_counts);
                if (g->case_default_template) {
                    uir_destroy_design_unit(g->case_default_template);
                    g->case_default_template = NULL;
                }
                break;
            }
            case UIR_SYS_TASK: {
                uir_sys_task_t *t = (uir_sys_task_t *)node;
                free(t->filename);
                free(t->mem_name);
                free(t->fmt);
                free(t->args);  /* individual nodes freed by arena cleanup */
                break;
            }
            case UIR_SYS_FUNC_EXPR: {
                uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)node;
                free(sf->args);
                break;
            }
            case UIR_WAIT:
                /* UIR_WAIT: condition and body are UIR nodes owned by arena */
                break;
            case UIR_EVENT_CTRL: {
                uir_event_ctrl_t *ec = (uir_event_ctrl_t *)node;
                free(ec->signal_name);
                break;
            }
            case UIR_DISABLE: {
                uir_disable_t *d = (uir_disable_t *)node;
                free(d->target_name);
                break;
            }
            case UIR_EVENT_TRIGGER: {
                uir_event_trigger_t *et = (uir_event_trigger_t *)node;
                free(et->name);
                break;
            }
            case UIR_UDP: {
                uir_udp_t *u = (uir_udp_t *)node;
                for (size_t ui = 0; ui < u->entry_count; ui++)
                    free(u->entries[ui].input_pattern);
                free(u->entries);
                break;
            }
            case UIR_EXIT: {
                uir_exit_t *e = (uir_exit_t *)node;
                free(e->loop_label);
                break;
            }
            case UIR_NEXT: {
                uir_next_t *n = (uir_next_t *)node;
                free(n->loop_label);
                break;
            }
            case UIR_RETURN:
                /* UIR_RETURN: expr is arena-owned, no explicit free */
                break;
            case UIR_SPECIFY: {
                uir_specify_t *sp = (uir_specify_t *)node;
            case UIR_VHDL_ASSERT:
                break;
                /* Free specparams */
                for (size_t si = 0; si < sp->specparam_count; si++) {
                    free(sp->specparams[si].hier_path);
                }
                free(sp->specparams);
                /* Free path delays */
                for (size_t pi = 0; pi < sp->path_count; pi++) {
                    free(sp->paths[pi].src);
                    free(sp->paths[pi].dst);
                    free(sp->paths[pi].data_src);
                }
                free(sp->paths);
                /* Free timing checks */
                for (size_t ti = 0; ti < sp->timing_check_count; ti++) {
                    free(sp->timing_checks[ti].data_pin);
                    free(sp->timing_checks[ti].ref_pin);
                    free(sp->timing_checks[ti].notifier);
                }
                free(sp->timing_checks);
                break;
            }
            case UIR_REF: {
                uir_ref_t *r = (uir_ref_t *)node;
                free(r->name);
                free(r->multi_index);
                break;
            }
            case UIR_FUNC_DEF:
            case UIR_TASK_DEF: {
                uir_func_t *ft = (uir_func_t *)node;
                free(ft->name);
                if (ft->ports) {
                    for (size_t pi = 0; pi < ft->port_count; pi++)
                        free(ft->ports[pi].name);
                    free(ft->ports);
                }
                free(ft->locals);
                break;
            }
            case UIR_FUNC_CALL:
            case UIR_TASK_ENABLE: {
                uir_func_call_t *fc = (uir_func_call_t *)node;
                free(fc->name);
                free(fc->args);
                break;
            }
            default:
                break;
        }

        /* Free fan-in/fan-out arrays */
        free(node->fan_in);
        free(node->fan_out);

        free(node);
    }

    /* Free port/signal/process/assign/instance/generate pointer arrays */
    free(unit->ports);
    free(unit->signals);
    free(unit->processes);
    free(unit->assigns);
    free(unit->instances);
    free(unit->generates);
    free(unit->func_tasks);
    /* Free defparams */
    for (size_t di = 0; di < unit->defparam_count; di++) {
        free(unit->defparams[di].hier_path);
    }
    free(unit->defparams);
    /* Free module params */
    for (size_t pi = 0; pi < unit->param_count; pi++) {
        free(unit->params[pi].hier_path);
    }
    free(unit->params);
    /* Free attrs */
    for (size_t ai = 0; ai < unit->attr_count; ai++) {
        free(unit->attrs[ai].name);
        free(unit->attrs[ai].value);
    }
    free(unit->attrs);
    /* Free VHDL component declarations */
    for (size_t ci = 0; ci < unit->component_count; ci++) {
        free(unit->components[ci].name);
        for (size_t pi = 0; pi < unit->components[ci].port_count; pi++)
            free(unit->components[ci].ports[pi].name);
        free(unit->components[ci].ports);
    }
    free(unit->components);
    /* Free VHDL type/subtype declarations */
    for (size_t ti = 0; ti < unit->vhdl_type_count; ti++) {
        free(unit->vhdl_types[ti].name);
        free(unit->vhdl_types[ti].base_type_name);
        for (size_t li = 0; li < unit->vhdl_types[ti].enum_literal_count; li++)
            free(unit->vhdl_types[ti].enum_literals[li]);
        free(unit->vhdl_types[ti].enum_literals);
        for (size_t ri = 0; ri < unit->vhdl_types[ti].record_field_count; ri++)
            free(unit->vhdl_types[ti].record_fields[ri].name);
        free(unit->vhdl_types[ti].record_fields);
    }
    free(unit->vhdl_types);
    /* Free VHDL constant declarations */
    for (size_t ci = 0; ci < unit->vhdl_constant_count; ci++)
        free(unit->vhdl_constants[ci].name);
    free(unit->vhdl_constants);
    /* Free VHDL file variable declarations */
    for (size_t fi = 0; fi < unit->file_meta_count; fi++) {
        free(unit->file_metas[fi].name);
        free(unit->file_metas[fi].file_name);
    }
    free(unit->file_metas);
    /* Free VHDL alias declarations */
    for (size_t ai = 0; ai < unit->vhdl_alias_count; ai++) {
        free(unit->vhdl_aliases[ai].name);
        free(unit->vhdl_aliases[ai].target);
    }
    free(unit->vhdl_aliases);
    /* Free VHDL group/template declarations */
    for (size_t gi = 0; gi < unit->vhdl_group_count; gi++) {
        free(unit->vhdl_groups[gi].name);
        free(unit->vhdl_groups[gi].template_name);
        for (size_t ci = 0; ci < unit->vhdl_groups[gi].constituent_count; ci++)
            free(unit->vhdl_groups[gi].constituents[ci]);
        free(unit->vhdl_groups[gi].constituents);
    }
    free(unit->vhdl_groups);
    /* Free VHDL attribute specifications */
    for (size_t ai = 0; ai < unit->vhdl_attr_spec_count; ai++) {
        free(unit->vhdl_attr_specs[ai].name);
        free(unit->vhdl_attr_specs[ai].target);
        free(unit->vhdl_attr_specs[ai].entity_class);
        free(unit->vhdl_attr_specs[ai].value);
    }
    free(unit->vhdl_attr_specs);
    /* Free modports */
    for (size_t mi = 0; mi < unit->modport_count; mi++) {
        free(unit->modports[mi].name);
        for (size_t pi = 0; pi < unit->modports[mi].port_count; pi++)
            free(unit->modports[mi].ports[pi].name);
        free(unit->modports[mi].ports);
    }
    free(unit->modports);
    /* Free package imports */
    for (size_t ii = 0; ii < unit->import_count; ii++) {
        free(unit->imports[ii].pkg_name);
        free(unit->imports[ii].item_name);
    }
    free(unit->imports);
    /* Free VHDL configuration data */
    free(unit->config_entity_name);
    for (size_t ci = 0; ci < unit->config_block_count; ci++)
        free(unit->config_blocks[ci].arch_name);
    free(unit->config_blocks);
    /* Free specifies array (specify nodes themselves freed by node loop above) */
    free(unit->specifies);
    unit->udp = NULL; /* freed by the all_nodes loop above */

    /* Free VHDL library/use storage */
    for (size_t li = 0; li < unit->library_count; li++)
        free(unit->library_names[li]);
    free(unit->library_names);
    for (size_t ui = 0; ui < unit->use_count; ui++)
        free(unit->use_clauses[ui]);
    free(unit->use_clauses);
    free(unit->all_nodes);

    free(unit);
}

/* ── Add ports, signals, etc. ── */

uir_port_t *uir_add_port(uir_design_unit_t *unit, const char *name,
                          uir_port_dir_t dir, uint32_t msb, uint32_t lsb,
                          uir_signal_type_t sig_type)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_port_t *port = (uir_port_t *)uir_alloc_node(unit, UIR_PORT, sizeof(uir_port_t), loc);
    if (!port) return NULL;
    port->name = strdup(name);
    port->direction = dir;
    port->sig_type = sig_type;
    port->msb = msb;
    port->lsb = lsb;
    port->width = (msb >= lsb) ? (msb - lsb + 1) : (lsb - msb + 1);
    port->is_vector = (msb != lsb) ? 1 : 0;
    port->init_value.state = QSIM_X;
    port->init_value.strength = QSIM_STRENGTH_STRONG;
    port->array_size = 0;
    port->array_dim_count = 0;
    memset(port->array_dims, 0, sizeof(port->array_dims));

    /* Append to ports array */
    uir_port_t **new_ports = realloc(unit->ports,
        (unit->port_count + 1) * sizeof(uir_port_t *));
    if (!new_ports) return NULL;
    unit->ports = new_ports;
    unit->ports[unit->port_count++] = port;

    return port;
}

uir_signal_t *uir_add_signal(uir_design_unit_t *unit, const char *name,
                              uir_signal_type_t type, uint32_t width,
                              uint32_t array_size)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_signal_t *sig = (uir_signal_t *)uir_alloc_node(unit, UIR_SIGNAL, sizeof(uir_signal_t), loc);
    if (!sig) return NULL;
    sig->name = strdup(name);
    sig->sig_type = type;
    sig->width = width;
    sig->array_size = array_size;
    sig->init_value.state = QSIM_X;
    sig->init_value.strength = QSIM_STRENGTH_STRONG;

    uir_signal_t **new_sigs = realloc(unit->signals,
        (unit->signal_count + 1) * sizeof(uir_signal_t *));
    if (!new_sigs) return NULL;
    unit->signals = new_sigs;
    unit->signals[unit->signal_count++] = sig;

    return sig;
}

uir_process_t *uir_add_process(uir_design_unit_t *unit, uir_proc_kind_t kind)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_node_kind_t node_kind;
    switch (kind) {
        case UIR_PROC_ALWAYS:        node_kind = UIR_ALWAYS; break;
        case UIR_PROC_INITIAL:       node_kind = UIR_INITIAL; break;
        case UIR_PROC_VHDL:          node_kind = UIR_PROCESS_VHDL; break;
        case UIR_PROC_ALWAYS_COMB:   node_kind = UIR_ALWAYS_COMB; break;
        case UIR_PROC_ALWAYS_FF:     node_kind = UIR_ALWAYS_FF; break;
        case UIR_PROC_ALWAYS_LATCH:  node_kind = UIR_ALWAYS_LATCH; break;
        default:                     node_kind = UIR_PROCESS; break;
    }
    uir_process_t *proc = (uir_process_t *)uir_alloc_node(unit, node_kind, sizeof(uir_process_t), loc);
    if (!proc) return NULL;
    proc->proc_kind = kind;
    proc->name = NULL;

    uir_process_t **new_procs = realloc(unit->processes,
        (unit->process_count + 1) * sizeof(uir_process_t *));
    if (!new_procs) return NULL;
    unit->processes = new_procs;
    unit->processes[unit->process_count++] = proc;

    return proc;
}

uir_assign_t *uir_add_assign(uir_design_unit_t *unit)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_assign_t *assign = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), loc);
    if (!assign) return NULL;

    uir_assign_t **new_assigns = realloc(unit->assigns,
        (unit->assign_count + 1) * sizeof(uir_assign_t *));
    if (!new_assigns) return NULL;
    unit->assigns = new_assigns;
    unit->assigns[unit->assign_count++] = assign;

    return assign;
}

uir_instance_t *uir_add_instance(uir_design_unit_t *unit, const char *inst_name,
                                  const char *module_name)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_instance_t *inst = (uir_instance_t *)uir_alloc_node(unit, UIR_INSTANCE, sizeof(uir_instance_t), loc);
    if (!inst) return NULL;
    inst->instance_name = strdup(inst_name);
    inst->module_name = strdup(module_name);
    inst->bound_to = NULL;
    inst->connections = NULL;
    inst->connection_count = 0;

    uir_instance_t **new_insts = realloc(unit->instances,
        (unit->instance_count + 1) * sizeof(uir_instance_t *));
    if (!new_insts) return NULL;
    unit->instances = new_insts;
    unit->instances[unit->instance_count++] = inst;

    return inst;
}

void uir_add_connection(uir_instance_t *inst, const char *formal_name, uir_node_t *actual) {
    if (!inst || !formal_name) return;
    size_t n = inst->connection_count;
    uir_port_connection_t *nc = realloc(inst->connections,
        (n + 1) * sizeof(uir_port_connection_t));
    if (!nc) return;
    inst->connections = nc;
    inst->connections[n].formal_name = strdup(formal_name);
    inst->connections[n].actual = actual;
    inst->connections[n].modport_name = NULL;
    inst->connection_count++;
}

/* ── VHDL component declaration construction ── */

uir_component_t *uir_add_component(uir_design_unit_t *unit, const char *name)
{
    if (!unit || !name) return NULL;
    size_t n = unit->component_count;
    uir_component_t *nc = realloc(unit->components,
        (n + 1) * sizeof(uir_component_t));
    if (!nc) return NULL;
    unit->components = nc;
    unit->components[n].name = strdup(name);
    unit->components[n].ports = NULL;
    unit->components[n].port_count = 0;
    unit->component_count++;
    return &unit->components[n];
}

void uir_add_component_port(uir_component_t *comp, const char *name,
                             uir_port_dir_t dir, uint32_t width)
{
    if (!comp || !name) return;
    size_t n = comp->port_count;
    uir_component_port_t *np = realloc(comp->ports,
        (n + 1) * sizeof(uir_component_port_t));
    if (!np) return;
    comp->ports = np;
    comp->ports[n].name = strdup(name);
    comp->ports[n].direction = dir;
    comp->ports[n].width = width;
    comp->port_count++;
}

/* ── SystemVerilog modport construction ── */

uir_modport_t *uir_add_modport(uir_design_unit_t *unit, const char *name)
{
    if (!unit || !name) return NULL;
    size_t n = unit->modport_count;
    uir_modport_t *nm = realloc(unit->modports, (n + 1) * sizeof(uir_modport_t));
    if (!nm) return NULL;
    unit->modports = nm;
    memset(&unit->modports[n], 0, sizeof(uir_modport_t));
    unit->modports[n].name = strdup(name);
    unit->modport_count++;
    return &unit->modports[n];
}

void uir_add_modport_port(uir_modport_t *mp, const char *name, uir_port_dir_t dir)
{
    if (!mp || !name) return;
    size_t n = mp->port_count;
    uir_modport_port_t *np = realloc(mp->ports, (n + 1) * sizeof(uir_modport_port_t));
    if (!np) return;
    mp->ports = np;
    mp->ports[n].name = strdup(name);
    mp->ports[n].direction = dir;
    mp->port_count++;
}

/* ── SystemVerilog package import construction ── */

void uir_add_import(uir_design_unit_t *unit, const char *pkg_name, const char *item_name)
{
    if (!unit || !pkg_name) return;
    size_t n = unit->import_count;
    uir_package_import_t *ni = realloc(unit->imports, (n + 1) * sizeof(uir_package_import_t));
    if (!ni) return;
    unit->imports = ni;
    unit->imports[n].pkg_name = strdup(pkg_name);
    unit->imports[n].item_name = item_name ? strdup(item_name) : NULL;
    unit->import_count++;
}

/* ── VHDL type/subtype declaration construction ── */

uir_vhdl_type_t *uir_add_vhdl_type(uir_design_unit_t *unit, const char *name,
                                     uir_vhdl_type_kind_t kind)
{
    if (!unit || !name) return NULL;
    size_t n = unit->vhdl_type_count;
    uir_vhdl_type_t *nt = realloc(unit->vhdl_types,
        (n + 1) * sizeof(uir_vhdl_type_t));
    if (!nt) return NULL;
    unit->vhdl_types = nt;
    memset(&unit->vhdl_types[n], 0, sizeof(uir_vhdl_type_t));
    unit->vhdl_types[n].name = strdup(name);
    unit->vhdl_types[n].kind = kind;
    unit->vhdl_types[n].width = 0;
    unit->vhdl_type_count++;
    return &unit->vhdl_types[n];
}

void uir_add_vhdl_type_literal(uir_vhdl_type_t *t, const char *literal)
{
    if (!t || !literal) return;
    size_t n = t->enum_literal_count;
    char **nl = realloc(t->enum_literals, (n + 1) * sizeof(char *));
    if (!nl) return;
    t->enum_literals = nl;
    t->enum_literals[t->enum_literal_count++] = strdup(literal);
}

void uir_set_vhdl_type_range(uir_vhdl_type_t *t, int64_t lo, int64_t hi, int dir)
{
    if (!t) return;
    t->range_lo = lo;
    t->range_hi = hi;
    t->range_dir = dir;
    uint64_t w = (hi >= lo) ? (uint64_t)(hi - lo + 1) : (uint64_t)(lo - hi + 1);
    t->width = (uint32_t)(w > UINT32_MAX ? 32 : w);
}

void uir_set_vhdl_type_base(uir_vhdl_type_t *t, const char *base_name)
{
    if (!t) return;
    free(t->base_type_name);
    t->base_type_name = base_name ? strdup(base_name) : NULL;
}

void uir_add_vhdl_record_field(uir_vhdl_type_t *t, const char *name, uint32_t width)
{
    if (!t || !name) return;
    size_t n = t->record_field_count;
    uir_vhdl_record_field_t *nf = realloc(t->record_fields,
        (n + 1) * sizeof(uir_vhdl_record_field_t));
    if (!nf) return;
    t->record_fields = nf;
    memset(&t->record_fields[n], 0, sizeof(uir_vhdl_record_field_t));
    t->record_fields[n].name = strdup(name);
    t->record_fields[n].width = width;
    t->record_field_count++;
}

uir_vhdl_constant_t *uir_add_vhdl_constant(uir_design_unit_t *unit, const char *name,
                                             uint32_t width, uint64_t value)
{
    if (!unit || !name) return NULL;
    size_t n = unit->vhdl_constant_count;
    uir_vhdl_constant_t *nc = realloc(unit->vhdl_constants,
        (n + 1) * sizeof(uir_vhdl_constant_t));
    if (!nc) return NULL;
    unit->vhdl_constants = nc;
    memset(&unit->vhdl_constants[n], 0, sizeof(uir_vhdl_constant_t));
    unit->vhdl_constants[n].name = strdup(name);
    unit->vhdl_constants[n].width = width;
    unit->vhdl_constants[n].value = value;
    unit->vhdl_constant_count++;
    return &unit->vhdl_constants[n];
}

uir_file_meta_t *uir_add_file_meta(uir_design_unit_t *unit, const char *name,
                                     int mode, const char *file_name)
{
    if (!unit || !name) return NULL;
    size_t n = unit->file_meta_count;
    uir_file_meta_t *nm = realloc(unit->file_metas,
        (n + 1) * sizeof(uir_file_meta_t));
    if (!nm) return NULL;
    unit->file_metas = nm;
    memset(&unit->file_metas[n], 0, sizeof(uir_file_meta_t));
    unit->file_metas[n].name = strdup(name);
    unit->file_metas[n].mode = mode;
    unit->file_metas[n].file_name = file_name ? strdup(file_name) : NULL;
    unit->file_meta_count++;
    return &unit->file_metas[n];
}

void uir_add_vhdl_config_block(uir_design_unit_t *unit, const char *arch_name)
{
    if (!unit || !arch_name) return;
    size_t n = unit->config_block_count;
    uir_vhdl_config_block_t *nb = realloc(unit->config_blocks,
        (n + 1) * sizeof(uir_vhdl_config_block_t));
    if (!nb) return;
    unit->config_blocks = nb;
    memset(&unit->config_blocks[n], 0, sizeof(uir_vhdl_config_block_t));
    unit->config_blocks[n].arch_name = strdup(arch_name);
    unit->config_block_count++;
}

uir_vhdl_alias_t *uir_add_vhdl_alias(uir_design_unit_t *unit, const char *name,
                                       const char *target)
{
    if (!unit || !name || !target) return NULL;
    size_t n = unit->vhdl_alias_count;
    uir_vhdl_alias_t *na = realloc(unit->vhdl_aliases,
        (n + 1) * sizeof(uir_vhdl_alias_t));
    if (!na) return NULL;
    unit->vhdl_aliases = na;
    memset(&unit->vhdl_aliases[n], 0, sizeof(uir_vhdl_alias_t));
    unit->vhdl_aliases[n].name = strdup(name);
    unit->vhdl_aliases[n].target = strdup(target);
    unit->vhdl_alias_count++;
    return &unit->vhdl_aliases[n];
}

uir_vhdl_group_t *uir_add_vhdl_group(uir_design_unit_t *unit, const char *name,
                                       uir_vhdl_group_kind_t kind,
                                       const char *template_name)
{
    if (!unit || !name) return NULL;
    size_t n = unit->vhdl_group_count;
    uir_vhdl_group_t *ng = realloc(unit->vhdl_groups,
        (n + 1) * sizeof(uir_vhdl_group_t));
    if (!ng) return NULL;
    unit->vhdl_groups = ng;
    memset(&unit->vhdl_groups[n], 0, sizeof(uir_vhdl_group_t));
    unit->vhdl_groups[n].name = strdup(name);
    unit->vhdl_groups[n].kind = kind;
    unit->vhdl_groups[n].template_name = template_name ? strdup(template_name) : NULL;
    unit->vhdl_group_count++;
    return &unit->vhdl_groups[n];
}

void uir_add_vhdl_group_constituent(uir_vhdl_group_t *g, const char *name)
{
    if (!g || !name) return;
    size_t n = g->constituent_count;
    char **nc = realloc(g->constituents, (n + 1) * sizeof(char *));
    if (!nc) return;
    g->constituents = nc;
    g->constituents[g->constituent_count++] = strdup(name);
}

uir_vhdl_attr_spec_t *uir_add_vhdl_attr_spec(uir_design_unit_t *unit,
    const char *name, const char *target, const char *entity_class,
    const char *value)
{
    if (!unit) return NULL;
    size_t n = unit->vhdl_attr_spec_count;
    uir_vhdl_attr_spec_t *na = realloc(unit->vhdl_attr_specs,
        (n + 1) * sizeof(uir_vhdl_attr_spec_t));
    if (!na) return NULL;
    unit->vhdl_attr_specs = na;
    memset(&unit->vhdl_attr_specs[n], 0, sizeof(uir_vhdl_attr_spec_t));
    unit->vhdl_attr_specs[n].name = name ? strdup(name) : NULL;
    unit->vhdl_attr_specs[n].target = target ? strdup(target) : NULL;
    unit->vhdl_attr_specs[n].entity_class = entity_class ? strdup(entity_class) : NULL;
    unit->vhdl_attr_specs[n].value = value ? strdup(value) : NULL;
    unit->vhdl_attr_spec_count++;
    return &unit->vhdl_attr_specs[n];
}

uir_block_t *uir_add_block(uir_design_unit_t *unit, int is_sequential)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_block_t *block = (uir_block_t *)uir_alloc_node(unit, UIR_BLOCK, sizeof(uir_block_t), loc);
    if (!block) return NULL;
    block->is_sequential = is_sequential;
    block->name = NULL;
    block->stmts = NULL;
    block->stmt_count = 0;
    return block;
}

uir_generate_t *uir_add_generate(uir_design_unit_t *unit, uir_gen_type_t gen_type)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_generate_t *gen = (uir_generate_t *)uir_alloc_node(unit, UIR_GENERATE, sizeof(uir_generate_t), loc);
    if (!gen) return NULL;
    gen->gen_type = gen_type;
    gen->label = NULL;
    gen->genvar_name = NULL;
    gen->for_init = NULL;
    gen->for_cond = NULL;
    gen->for_step = NULL;
    gen->for_direction = 0;
    gen->if_condition = NULL;
    gen->body_items = NULL;
    gen->body_item_count = 0;

    uir_generate_t **new_gens = realloc(unit->generates,
        (unit->generate_count + 1) * sizeof(uir_generate_t *));
    if (!new_gens) return NULL;
    unit->generates = new_gens;
    unit->generates[unit->generate_count++] = gen;

    return gen;
}

void uir_add_generate_body_item(uir_generate_t *gen, uir_node_t *item)
{
    if (!gen || !item) return;
    uir_node_t **new_items = realloc(gen->body_items,
        (gen->body_item_count + 1) * sizeof(uir_node_t *));
    if (!new_items) return;
    gen->body_items = new_items;
    gen->body_items[gen->body_item_count++] = item;
}

void uir_add_generate_else_body_item(uir_generate_t *gen, uir_node_t *item)
{
    if (!gen || !item) return;
    uir_node_t **new_items = realloc(gen->else_body_items,
        (gen->else_body_item_count + 1) * sizeof(uir_node_t *));
    if (!new_items) return;
    gen->else_body_items = new_items;
    gen->else_body_items[gen->else_body_item_count++] = item;
}

/* ── Function/task construction ── */

uir_func_t *uir_add_func_task(uir_design_unit_t *unit, const char *name,
                               int is_function)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_node_kind_t kind = is_function ? UIR_FUNC_DEF : UIR_TASK_DEF;
    uir_func_t *ft = (uir_func_t *)uir_alloc_node(unit, kind, sizeof(uir_func_t), loc);
    if (!ft) return NULL;
    ft->name = strdup(name);
    ft->is_function = is_function;
    ft->ports = NULL;
    ft->port_count = 0;
    ft->locals = NULL;
    ft->local_count = 0;
    ft->body = NULL;
    ft->return_width = 0;
    ft->builtin_kind = 0;

    uir_func_t **new_ft = realloc(unit->func_tasks,
        (unit->func_task_count + 1) * sizeof(uir_func_t *));
    if (!new_ft) return NULL;
    unit->func_tasks = new_ft;
    unit->func_tasks[unit->func_task_count++] = ft;
    return ft;
}

void uir_add_func_port(uir_func_t *ft, const char *name,
                        uir_port_dir_t dir, uint32_t width)
{
    if (!ft || !name) return;
    size_t n = ft->port_count;
    uir_func_port_t *np = realloc(ft->ports, (n + 1) * sizeof(uir_func_port_t));
    if (!np) return;
    ft->ports = np;
    ft->ports[n].name = strdup(name);
    ft->ports[n].direction = dir;
    ft->ports[n].width = width;
    ft->port_count++;
}

void uir_add_func_local(uir_func_t *ft, uir_node_t *local_sig)
{
    if (!ft || !local_sig) return;
    size_t n = ft->local_count;
    uir_node_t **nl = realloc(ft->locals, (n + 1) * sizeof(uir_node_t *));
    if (!nl) return;
    ft->locals = nl;
    ft->locals[ft->local_count++] = local_sig;
}

uir_func_call_t *uir_make_func_call(uir_design_unit_t *unit, const char *name,
                                     uir_node_t **args, size_t arg_count,
                                     uir_loc_t loc)
{
    uir_func_call_t *fc = (uir_func_call_t *)uir_alloc_node(
        unit, UIR_FUNC_CALL, sizeof(uir_func_call_t), loc);
    if (!fc) return NULL;
    fc->name = strdup(name);
    fc->args = NULL;
    fc->arg_count = 0;
    if (arg_count > 0) {
        fc->args = calloc(arg_count, sizeof(uir_node_t *));
        if (!fc->args) return fc;
        memcpy(fc->args, args, arg_count * sizeof(uir_node_t *));
        fc->arg_count = arg_count;
    }
    return fc;
}
/* ── VHDL assert/report ── */

uir_vhdl_assert_t *uir_make_vhdl_assert(uir_design_unit_t *unit,
                                          uir_node_t *condition,
                                          uir_node_t *message,
                                          int severity, uir_loc_t loc)
{
    uir_vhdl_assert_t *a = (uir_vhdl_assert_t *)uir_alloc_node(
        unit, UIR_VHDL_ASSERT, sizeof(uir_vhdl_assert_t), loc);
    if (!a) return NULL;
    a->condition = condition;
    a->message = message;
    a->severity = severity;
    return a;
}



/* ── Expression builders ── */

uir_expr_t *uir_make_binary(uir_design_unit_t *unit, uir_binary_op_t op,
                             uir_node_t *a, uir_node_t *b, uir_loc_t loc)
{
    uir_expr_t *expr = (uir_expr_t *)uir_alloc_node(unit, UIR_EXPR_BINARY, sizeof(uir_expr_t), loc);
    if (!expr) return NULL;
    expr->is_binary = 1;
    expr->op.bin_op = op;
    expr->operand_a = a;
    expr->operand_b = b;

    uir_add_fan_in((uir_node_t *)expr, a);
    uir_add_fan_in((uir_node_t *)expr, b);
    uir_add_fan_out(a, (uir_node_t *)expr);
    uir_add_fan_out(b, (uir_node_t *)expr);

    return expr;
}

uir_expr_t *uir_make_unary(uir_design_unit_t *unit, uir_unary_op_t op,
                            uir_node_t *a, uir_loc_t loc)
{
    uir_expr_t *expr = (uir_expr_t *)uir_alloc_node(unit, UIR_EXPR_UNARY, sizeof(uir_expr_t), loc);
    if (!expr) return NULL;
    expr->is_binary = 0;
    expr->op.un_op = op;
    expr->operand_a = a;
    expr->operand_b = NULL;

    uir_add_fan_in((uir_node_t *)expr, a);
    uir_add_fan_out(a, (uir_node_t *)expr);

    return expr;
}

uir_literal_t *uir_make_literal(uir_design_unit_t *unit,
                                 qsim_bit_vector_t *value, uir_loc_t loc)
{
    uir_literal_t *lit = (uir_literal_t *)uir_alloc_node(unit, UIR_LITERAL, sizeof(uir_literal_t), loc);
    if (!lit) return NULL;
    lit->value = value;
    lit->width = value ? value->width : 0;
    lit->is_signed = 0;
    return lit;
}

uir_node_t *uir_make_ref(uir_design_unit_t *unit, const char *name, uir_loc_t loc)
{
    uir_ref_t *ref = (uir_ref_t *)uir_alloc_node(unit, UIR_REF, sizeof(uir_ref_t), loc);
    if (!ref) return NULL;
    ref->name = strdup(name);
    ref->index = NULL;
    ref->part_hi = NULL;
    ref->part_lo = NULL;
    return (uir_node_t *)ref;
}

uir_node_t *uir_make_ref_index(uir_design_unit_t *unit, const char *name,
                                uir_node_t *index, uir_loc_t loc)
{
    uir_ref_t *ref = (uir_ref_t *)uir_alloc_node(unit, UIR_REF, sizeof(uir_ref_t), loc);
    if (!ref) return NULL;
    ref->name = strdup(name);
    ref->index = index;
    ref->part_hi = NULL;
    ref->part_lo = NULL;
    return (uir_node_t *)ref;
}

uir_node_t *uir_make_ref_part_select(uir_design_unit_t *unit, const char *name,
                                      uir_node_t *hi, uir_node_t *lo, uir_loc_t loc)
{
    uir_ref_t *ref = (uir_ref_t *)uir_alloc_node(unit, UIR_REF, sizeof(uir_ref_t), loc);
    if (!ref) return NULL;
    ref->name = strdup(name);
    ref->index = NULL;
    ref->part_hi = hi;
    ref->part_lo = lo;
    return (uir_node_t *)ref;
}

uir_node_t *uir_make_ref_multi_index(uir_design_unit_t *unit, const char *name,
                                      uir_node_t **indices, size_t idx_count,
                                      uir_loc_t loc)
{
    uir_ref_t *ref = (uir_ref_t *)uir_alloc_node(unit, UIR_REF, sizeof(uir_ref_t), loc);
    if (!ref) return NULL;
    ref->name = strdup(name);
    ref->index = NULL;
    ref->part_hi = NULL;
    ref->part_lo = NULL;
    if (idx_count > 0) {
        ref->multi_index = calloc(idx_count, sizeof(uir_node_t *));
        if (ref->multi_index) {
            for (size_t i = 0; i < idx_count; i++)
                ref->multi_index[i] = indices[i];
            ref->multi_idx_count = idx_count;
        }
    } else {
        ref->multi_index = NULL;
        ref->multi_idx_count = 0;
    }
    return (uir_node_t *)ref;
}

uir_node_t *uir_make_sys_task(uir_design_unit_t *unit, uir_sys_task_kind_t kind,
                               const char *filename, const char *mem_name,
                               const char *fmt, uir_node_t **args, size_t arg_count,
                               uir_loc_t loc)
{
    uir_sys_task_t *t = (uir_sys_task_t *)uir_alloc_node(unit, UIR_SYS_TASK, sizeof(uir_sys_task_t), loc);
    if (!t) return NULL;
    t->task_kind = kind;
    t->filename = filename ? strdup(filename) : NULL;
    t->mem_name = mem_name ? strdup(mem_name) : NULL;
    t->fmt = fmt ? strdup(fmt) : NULL;
    t->args = args;
    t->arg_count = arg_count;
    return (uir_node_t *)t;
}

/* ── System function expression constructor ── */

uir_node_t *uir_make_sys_func_expr(uir_design_unit_t *unit, uir_sys_func_kind_t kind,
                                    uir_node_t **args, size_t arg_count,
                                    uir_loc_t loc)
{
    uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)uir_alloc_node(
        unit, UIR_SYS_FUNC_EXPR, sizeof(uir_sys_func_expr_t), loc);
    if (!sf) return NULL;
    sf->func_kind = kind;
    sf->args = args;
    sf->arg_count = arg_count;
    return (uir_node_t *)sf;
}

/* ── Event trigger constructor (-> name;) ── */

uir_event_trigger_t *uir_make_event_trigger(uir_design_unit_t *unit,
                                              const char *name, uir_loc_t loc)
{
    uir_event_trigger_t *et = (uir_event_trigger_t *)uir_alloc_node(
        unit, UIR_EVENT_TRIGGER, sizeof(uir_event_trigger_t), loc);
    if (!et) return NULL;
    et->name = name ? uir_intern_string(unit, name) : NULL;
    return et;
}

/* ── Specify block construction ── */

uir_specify_t *uir_add_specify(uir_design_unit_t *unit)
{
    uir_loc_t loc = {NULL, 0, 0};
    uir_specify_t *spec = (uir_specify_t *)uir_alloc_node(
        unit, UIR_SPECIFY, sizeof(uir_specify_t), loc);
    if (!spec) return NULL;
    spec->specparams = NULL;
    spec->specparam_count = 0;
    spec->paths = NULL;
    spec->path_count = 0;
    spec->timing_checks = NULL;
    spec->timing_check_count = 0;

    uir_specify_t **new_specs = realloc(unit->specifies,
        (unit->specify_count + 1) * sizeof(uir_specify_t *));
    if (!new_specs) return NULL;
    unit->specifies = new_specs;
    unit->specifies[unit->specify_count++] = spec;

    return spec;
}

void uir_add_specpath(uir_specify_t *spec, const uir_path_delay_t *pd)
{
    if (!spec || !pd) return;
    size_t n = spec->path_count;
    uir_path_delay_t *np = realloc(spec->paths, (n + 1) * sizeof(uir_path_delay_t));
    if (!np) return;
    spec->paths = np;
    spec->paths[n] = *pd;
    spec->path_count++;
}

void uir_add_timing_check(uir_specify_t *spec, const uir_timing_check_t *tc)
{
    if (!spec || !tc) return;
    size_t n = spec->timing_check_count;
    uir_timing_check_t *nt = realloc(spec->timing_checks,
        (n + 1) * sizeof(uir_timing_check_t));
    if (!nt) return;
    spec->timing_checks = nt;
    spec->timing_checks[n] = *tc;
    spec->timing_check_count++;
}

void uir_add_specparam(uir_specify_t *spec, const char *name, uir_node_t *value)
{
    if (!spec || !name) return;
    size_t n = spec->specparam_count;
    uir_defparam_t *np = realloc(spec->specparams,
        (n + 1) * sizeof(uir_defparam_t));
    if (!np) return;
    spec->specparams = np;
    spec->specparams[n].hier_path = strdup(name);
    spec->specparams[n].value = value;
    spec->specparam_count++;
}

/* ── Dependency wiring ── */

void uir_add_fan_in(uir_node_t *node, uir_node_t *driver)
{
    if (!node || !driver) return;
    uir_node_t **new_fi = realloc(node->fan_in,
        (node->fan_in_count + 1) * sizeof(uir_node_t *));
    if (!new_fi) return;
    node->fan_in = new_fi;
    node->fan_in[node->fan_in_count++] = driver;
}

void uir_add_fan_out(uir_node_t *node, uir_node_t *load)
{
    if (!node || !load) return;
    uir_node_t **new_fo = realloc(node->fan_out,
        (node->fan_out_count + 1) * sizeof(uir_node_t *));
    if (!new_fo) return;
    node->fan_out = new_fo;
    node->fan_out[node->fan_out_count++] = load;
}

/* ── Hierarchy ── */

void uir_add_child(uir_design_unit_t *parent, uir_design_unit_t *child)
{
    (void)parent;
    (void)child;
    /* TODO: store child references */
}

/* ── Query API ── */

uir_node_t *uir_find_signal(uir_design_unit_t *unit, const char *hier_path)
{
    if (!unit || !hier_path) return NULL;

    /* Check ports */
    for (size_t i = 0; i < unit->port_count; i++) {
        if (strcmp(unit->ports[i]->name, hier_path) == 0)
            return (uir_node_t *)unit->ports[i];
    }

    /* Check signals */
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (strcmp(unit->signals[i]->name, hier_path) == 0)
            return (uir_node_t *)unit->signals[i];
    }

    /* TODO: handle hierarchical paths with dot notation */
    return NULL;
}

uir_node_t **uir_get_fan_in(uir_node_t *node, size_t *count)
{
    if (count) *count = node->fan_in_count;
    return node->fan_in;
}

uir_node_t **uir_get_fan_out(uir_node_t *node, size_t *count)
{
    if (count) *count = node->fan_out_count;
    return node->fan_out;
}

/* Simple BFS for tracing */
struct bfs_queue {
    uir_node_t **nodes;
    size_t count;
    size_t cap;
};

static void bfs_push(struct bfs_queue *q, uir_node_t *n)
{
    for (size_t i = 0; i < q->count; i++)
        if (q->nodes[i] == n) return; /* already visited */
    if (q->count >= q->cap) {
        q->cap = q->cap ? q->cap * 2 : 16;
        uir_node_t **new_n = realloc(q->nodes, q->cap * sizeof(uir_node_t *));
        if (!new_n) return;
        q->nodes = new_n;
    }
    q->nodes[q->count++] = n;
}

uir_node_t **uir_trace_drivers(uir_node_t *signal, size_t *count, int max_depth)
{
    struct bfs_queue q = {0};
    /* seed with fan-in of signal */
    for (size_t i = 0; i < signal->fan_in_count; i++)
        bfs_push(&q, signal->fan_in[i]);

    size_t idx = 0;
    int depth = 0;
    while (idx < q.count && (max_depth <= 0 || depth < max_depth)) {
        size_t level_end = q.count;
        while (idx < level_end) {
            uir_node_t *n = q.nodes[idx++];
            for (size_t i = 0; i < n->fan_in_count; i++)
                bfs_push(&q, n->fan_in[i]);
        }
        depth++;
    }

    *count = q.count;
    return q.nodes;
}

uir_node_t **uir_trace_loads(uir_node_t *signal, size_t *count, int max_depth)
{
    struct bfs_queue q = {0};
    for (size_t i = 0; i < signal->fan_out_count; i++)
        bfs_push(&q, signal->fan_out[i]);

    size_t idx = 0;
    int depth = 0;
    while (idx < q.count && (max_depth <= 0 || depth < max_depth)) {
        size_t level_end = q.count;
        while (idx < level_end) {
            uir_node_t *n = q.nodes[idx++];
            for (size_t i = 0; i < n->fan_out_count; i++)
                bfs_push(&q, n->fan_out[i]);
        }
        depth++;
    }

    *count = q.count;
    return q.nodes;
}

void uir_walk(uir_design_unit_t *unit,
              void (*callback)(uir_node_t *node, void *ctx),
              void *ctx)
{
    if (!unit || !callback) return;
    for (size_t i = 0; i < unit->node_count; i++) {
        if (unit->all_nodes[i])
            callback(unit->all_nodes[i], ctx);
    }
}

/* ── JSON serialization ── */

static void append_str(char **buf, size_t *len, const char *s)
{
    size_t slen = strlen(s);
    char *new_buf = realloc(*buf, *len + slen + 1);
    if (!new_buf) return;
    memcpy(new_buf + *len, s, slen + 1);
    *buf = new_buf;
    *len += slen;
}

static void json_escape(const char *src, char **buf, size_t *len)
{
    append_str(buf, len, "\"");
    for (const char *p = src; *p; p++) {
        char esc[8] = {0};
        switch (*p) {
            case '\"': snprintf(esc, sizeof(esc), "\\\""); break;
            case '\\': snprintf(esc, sizeof(esc), "\\\\"); break;
            case '\n': snprintf(esc, sizeof(esc), "\\n"); break;
            case '\t': snprintf(esc, sizeof(esc), "\\t"); break;
            default:   esc[0] = *p; esc[1] = '\0'; break;
        }
        append_str(buf, len, esc);
    }
    append_str(buf, len, "\"");
}

static void uir_node_to_json(uir_node_t *node, char **buf, size_t *len)
{
    char tmp[256];

    switch (node->kind) {
        case UIR_DESIGN_UNIT: {
            uir_design_unit_t *u = (uir_design_unit_t *)node;
            append_str(buf, len, "{\"kind\":\"design_unit\",\"name\":");
            json_escape(u->name, buf, len);
            append_str(buf, len, ",\"language\":");
            json_escape(u->language, buf, len);
            append_str(buf, len, ",\"ports\":[");
            for (size_t i = 0; i < u->port_count; i++) {
                if (i > 0) append_str(buf, len, ",");
                uir_node_to_json((uir_node_t *)u->ports[i], buf, len);
            }
            append_str(buf, len, "],\"signals\":[");
            for (size_t i = 0; i < u->signal_count; i++) {
                if (i > 0) append_str(buf, len, ",");
                uir_node_to_json((uir_node_t *)u->signals[i], buf, len);
            }
            snprintf(tmp, sizeof(tmp), "],\"node_count\":%zu}", u->node_count);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_PORT: {
            uir_port_t *p = (uir_port_t *)node;
            const char *dir = "?";
            switch (p->direction) {
                case UIR_PORT_IN: dir = "input"; break;
                case UIR_PORT_OUT: dir = "output"; break;
                case UIR_PORT_INOUT: dir = "inout"; break;
            }
            snprintf(tmp, sizeof(tmp), "{\"kind\":\"port\",\"name\":\"%s\",\"direction\":\"%s\",\"width\":%u}",
                     p->name, dir, p->width);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_SIGNAL: {
            uir_signal_t *s = (uir_signal_t *)node;
            const char *st = "?";
            switch (s->sig_type) {
                case UIR_SIG_WIRE: st = "wire"; break;
                case UIR_SIG_REG: st = "reg"; break;
                case UIR_SIG_VHDL_SIGNAL: st = "vhdl_signal"; break;
                case UIR_SIG_VHDL_VARIABLE: st = "vhdl_variable"; break;
            }
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"signal\",\"name\":\"%s\",\"type\":\"%s\",\"width\":%u}",
                     s->name, st, s->width);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_ALWAYS: {
            append_str(buf, len, "{\"kind\":\"always\"}");
            break;
        }
        case UIR_INITIAL: {
            append_str(buf, len, "{\"kind\":\"initial\"}");
            break;
        }
        case UIR_ALWAYS_COMB: {
            append_str(buf, len, "{\"kind\":\"always_comb\"}");
            break;
        }
        case UIR_ALWAYS_FF: {
            append_str(buf, len, "{\"kind\":\"always_ff\"}");
            break;
        }
        case UIR_ALWAYS_LATCH: {
            append_str(buf, len, "{\"kind\":\"always_latch\"}");
            break;
        }
        case UIR_ASSIGN: {
            append_str(buf, len, "{\"kind\":\"assign\"}");
            break;
        }
        case UIR_FORCE: {
            append_str(buf, len, "{\"kind\":\"force\"}");
            break;
        }
        case UIR_RELEASE: {
            append_str(buf, len, "{\"kind\":\"release\"}");
            break;
        }
        case UIR_EXPR_BINARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            snprintf(tmp, sizeof(tmp), "{\"kind\":\"binary_expr\",\"op\":%d}", e->op.bin_op);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_LITERAL: {
            uir_literal_t *l = (uir_literal_t *)node;
            char *vs = qsim_bit_vector_to_str(l->value);
            snprintf(tmp, sizeof(tmp), "{\"kind\":\"literal\",\"value\":\"%s\",\"width\":%u}",
                     vs ? vs : "?", l->width);
            free(vs);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_INSTANCE: {
            uir_instance_t *inst = (uir_instance_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"instance\",\"name\":\"%s\",\"module\":\"%s\"}",
                     inst->instance_name, inst->module_name);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_BLOCK: {
            uir_block_t *b = (uir_block_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"block\",\"sequential\":%d,\"stmts\":%zu}",
                     b->is_sequential, b->stmt_count);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_FUNC_DEF: {
            uir_func_t *ft = (uir_func_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"function\",\"name\":\"%s\",\"return_width\":%u}",
                     ft->name, ft->return_width);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_TASK_DEF: {
            uir_func_t *ft = (uir_func_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"task\",\"name\":\"%s\"}", ft->name);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_FUNC_CALL: {
            uir_func_call_t *fc = (uir_func_call_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"func_call\",\"name\":\"%s\",\"args\":%zu}",
                     fc->name, fc->arg_count);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_TASK_ENABLE: {
            uir_func_call_t *te = (uir_func_call_t *)node;
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"task_enable\",\"name\":\"%s\",\"args\":%zu}",
                     te->name, te->arg_count);
            append_str(buf, len, tmp);
            break;
        }
        case UIR_SYS_FUNC_EXPR: {
            uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)node;
            const char *fname = "?";
            switch (sf->func_kind) {
                case UIR_SYS_FUNC_SIGNED:   fname = "signed"; break;
                case UIR_SYS_FUNC_UNSIGNED: fname = "unsigned"; break;
                case UIR_SYS_FUNC_CLOG2:    fname = "clog2"; break;
                case UIR_SYS_FUNC_TIME:     fname = "time"; break;
                case UIR_SYS_FUNC_REALTIME: fname = "realtime"; break;
                case UIR_SYS_FUNC_RANDOM:   fname = "random"; break;
                case UIR_SYS_FUNC_FOPEN:    fname = "fopen"; break;
            }
            snprintf(tmp, sizeof(tmp),
                     "{\"kind\":\"sys_func_expr\",\"func\":\"%s\",\"args\":%zu}",
                     fname, sf->arg_count);
            append_str(buf, len, tmp);
            break;
        }
        default: {
            snprintf(tmp, sizeof(tmp), "{\"kind\":%d}", node->kind);
            append_str(buf, len, tmp);
            break;
        }
    }
}

char *uir_to_json(uir_design_unit_t *unit)
{
    if (!unit) return NULL;
    char *buf = NULL;
    size_t len = 0;
    uir_node_to_json(&unit->base, &buf, &len);
    return buf;
}

/* ── Node cloning with genvar substitution ── */

/* Forward declaration of recursive helper */
static uir_node_t *clone_node_internal(uir_design_unit_t *unit, uir_node_t *src,
                                        const uir_subst_t *subst);

static uir_node_t *clone_ref_name(uir_design_unit_t *unit, uir_node_t *src,
                                   const uir_subst_t *subst)
{
    uir_ref_t *s = (uir_ref_t *)src;
    uir_loc_t loc = src->loc;

    const char *name = s->name;
    /* Apply genvar substitution to the name itself (e.g. "i" → "0") */
    if (subst && subst->genvar_name && name &&
        strcmp(name, subst->genvar_name) == 0) {
        /* Replace the genvar reference with a literal integer constant */
        uint64_t val = subst->genvar_value;
        qsim_bit_vector_t *bv = qsim_bit_vector_alloc(32);
        if (bv) {
            for (uint32_t i = 0; i < 32; i++)
                qsim_bit_set(bv, i, (i < 64 && ((val >> i) & 1)) ? QSIM_VAL_1 : QSIM_VAL_0);
        }
        return (uir_node_t *)uir_make_literal(unit, bv, loc);
    }

    /* Multi-dimensional index: mem[i][j] */
    if (s->multi_idx_count > 0) {
        uir_ref_t *ref = (uir_ref_t *)uir_alloc_node(unit, UIR_REF, sizeof(uir_ref_t), loc);
        if (!ref) return NULL;
        ref->name = strdup(name);
        ref->index = NULL;
        ref->part_hi = NULL;
        ref->part_lo = NULL;
        ref->multi_idx_count = s->multi_idx_count;
        ref->multi_index = calloc(ref->multi_idx_count, sizeof(uir_node_t *));
        if (ref->multi_index) {
            for (size_t i = 0; i < ref->multi_idx_count; i++)
                ref->multi_index[i] = clone_node_internal(unit, s->multi_index[i], subst);
        }
        return (uir_node_t *)ref;
    }

    /* Clone the reference, including part-select and index */
    uir_node_t *index = s->index ? clone_node_internal(unit, s->index, subst) : NULL;
    uir_node_t *part_hi = s->part_hi ? clone_node_internal(unit, s->part_hi, subst) : NULL;
    uir_node_t *part_lo = s->part_lo ? clone_node_internal(unit, s->part_lo, subst) : NULL;

    if (part_hi && part_lo)
        return uir_make_ref_part_select(unit, name, part_hi, part_lo, loc);
    if (index)
        return uir_make_ref_index(unit, name, index, loc);
    return uir_make_ref(unit, name, loc);
}

static uir_node_t *clone_node_internal(uir_design_unit_t *unit, uir_node_t *src,
                                        const uir_subst_t *subst)
{
    if (!src) return NULL;
    uir_loc_t loc = src->loc;

    switch (src->kind) {
        case UIR_LITERAL: {
            uir_literal_t *l = (uir_literal_t *)src;
            qsim_bit_vector_t *bv = qsim_bit_vector_clone(l->value);
            return (uir_node_t *)uir_make_literal(unit, bv, loc);
        }

        case UIR_REF:
            return clone_ref_name(unit, src, subst);

        case UIR_EXPR_BINARY: {
            uir_expr_t *e = (uir_expr_t *)src;
            uir_node_t *a = clone_node_internal(unit, e->operand_a, subst);
            uir_node_t *b = clone_node_internal(unit, e->operand_b, subst);
            if (a && b)
                return (uir_node_t *)uir_make_binary(unit, e->op.bin_op, a, b, loc);
            return NULL;
        }

        case UIR_EXPR_UNARY: {
            uir_expr_t *e = (uir_expr_t *)src;
            uir_node_t *a = clone_node_internal(unit, e->operand_a, subst);
            if (a)
                return (uir_node_t *)uir_make_unary(unit, e->op.un_op, a, loc);
            return NULL;
        }

        case UIR_COND: {
            uir_cond_t *c = (uir_cond_t *)src;
            uir_node_t *cond = clone_node_internal(unit, c->condition, subst);
            uir_node_t *then_expr = clone_node_internal(unit, c->then_expr, subst);
            uir_node_t *else_expr = clone_node_internal(unit, c->else_expr, subst);
            if (cond && then_expr && else_expr) {
                uir_cond_t *nc = (uir_cond_t *)uir_alloc_node(unit, UIR_COND, sizeof(uir_cond_t), loc);
                if (nc) {
                    nc->condition = cond;
                    nc->then_expr = then_expr;
                    nc->else_expr = else_expr;
                    return (uir_node_t *)nc;
                }
            }
            return NULL;
        }

        case UIR_SIGNAL: {
            uir_signal_t *s = (uir_signal_t *)src;
            if (!s->name) return NULL;
            char sig_name_buf[1024];
            const char *sig_name = s->name;
            if (subst && subst->name_prefix) {
                snprintf(sig_name_buf, sizeof(sig_name_buf), "%s%s", subst->name_prefix, sig_name);
                sig_name = sig_name_buf;
            }
            uir_signal_t *ns = uir_add_signal(unit, sig_name, s->sig_type, s->width, s->array_size);
            if (!ns) return NULL;
            return (uir_node_t *)ns;
        }

        case UIR_INSTANCE: {
            uir_instance_t *inst = (uir_instance_t *)src;
            char name_buf[1024];
            const char *inst_name = inst->instance_name;
            if (subst && subst->name_prefix) {
                snprintf(name_buf, sizeof(name_buf), "%s%s", subst->name_prefix, inst_name);
                inst_name = name_buf;
            }
            uir_instance_t *ni = uir_add_instance(unit, inst_name, inst->module_name);
            if (!ni) return NULL;
            /* Clone connections */
            for (size_t i = 0; i < inst->connection_count; i++) {
                uir_port_connection_t *conn = &inst->connections[i];
                uir_node_t *actual = conn->actual
                    ? clone_node_internal(unit, conn->actual, subst) : NULL;
                uir_add_connection(ni, conn->formal_name, actual);
            }
            return (uir_node_t *)ni;
        }

        case UIR_ASSIGN: {
            uir_assign_t *a = (uir_assign_t *)src;
            uir_node_t *lhs = clone_node_internal(unit, a->lhs, subst);
            uir_node_t *rhs = clone_node_internal(unit, a->rhs, subst);
            if (lhs && rhs) {
                uir_assign_t *na = uir_add_assign(unit);
                if (na) {
                    na->lhs = lhs;
                    na->rhs = rhs;
                    na->delay = a->delay;
                    return (uir_node_t *)na;
                }
            }
            return NULL;
        }

        case UIR_ALWAYS:
        case UIR_INITIAL:
        case UIR_ALWAYS_COMB:
        case UIR_ALWAYS_FF:
        case UIR_ALWAYS_LATCH: {
            uir_process_t *p = (uir_process_t *)src;
            uir_proc_kind_t pk;
            switch (src->kind) {
                case UIR_ALWAYS:      pk = UIR_PROC_ALWAYS; break;
                case UIR_INITIAL:     pk = UIR_PROC_INITIAL; break;
                case UIR_ALWAYS_COMB: pk = UIR_PROC_ALWAYS_COMB; break;
                case UIR_ALWAYS_FF:   pk = UIR_PROC_ALWAYS_FF; break;
                case UIR_ALWAYS_LATCH: pk = UIR_PROC_ALWAYS_LATCH; break;
                default:              pk = UIR_PROC_ALWAYS; break;
            }
            uir_process_t *np = uir_add_process(unit, pk);
            if (!np) return NULL;
            np->name = p->name ? strdup(p->name) : NULL;
            np->auto_sens = p->auto_sens;
            /* Clone body */
            np->body = clone_node_internal(unit, p->body, subst);
            /* Clone sensitivity list (signal pointers, not names) */
            if (p->sensitivity_count > 0) {
                np->sensitivity_list = calloc(p->sensitivity_count, sizeof(uir_sensitivity_t));
                if (np->sensitivity_list) {
                    np->sensitivity_count = p->sensitivity_count;
                    /* Signal pointers are cloned below, but sensitivities
                     * point to signals by pointer, not name. For now skip
                     * cloning sensitivities — they'll be re-resolved by
                     * the simulator's auto-sens or left as empty. */
                }
            }
            return (uir_node_t *)np;
        }

        case UIR_BLOCK: {
            uir_block_t *b = (uir_block_t *)src;
            uir_block_t *nb = uir_add_block(unit, b->is_sequential);
            if (!nb) return NULL;
            nb->name = b->name ? strdup(b->name) : NULL;
            for (size_t i = 0; i < b->stmt_count; i++) {
                uir_node_t *cs = clone_node_internal(unit, b->stmts[i], subst);
                if (cs) {
                    uir_node_t **ns = realloc(nb->stmts, (nb->stmt_count + 1) * sizeof(uir_node_t *));
                    if (ns) { nb->stmts = ns; nb->stmts[nb->stmt_count++] = cs; }
                }
            }
            return (uir_node_t *)nb;
        }

        case UIR_IF: {
            uir_if_t *f = (uir_if_t *)src;
            uir_node_t *cond = clone_node_internal(unit, f->condition, subst);
            uir_node_t *then_b = clone_node_internal(unit, f->then_branch, subst);
            uir_node_t *else_b = f->else_branch ? clone_node_internal(unit, f->else_branch, subst) : NULL;
            if (cond && then_b) {
                uir_if_t *nf = (uir_if_t *)uir_alloc_node(unit, UIR_IF, sizeof(uir_if_t), loc);
                if (nf) {
                    nf->condition = cond;
                    nf->then_branch = then_b;
                    nf->else_branch = else_b;
                    return (uir_node_t *)nf;
                }
            }
            return NULL;
        }

        case UIR_CASE: {
            uir_case_t *c = (uir_case_t *)src;
            uir_node_t *expr = clone_node_internal(unit, c->expr, subst);
            if (!expr) return NULL;
            /* We re-use the case construction helpers. For simplicity
             * we build the cloned case directly using alloc_node. */
            uir_case_item_t **items = NULL;
            size_t item_count = c->item_count;
            if (item_count > 0) {
                items = calloc(item_count, sizeof(uir_case_item_t *));
                if (!items) return NULL;
                for (size_t i = 0; i < item_count; i++) {
                    uir_case_item_t *ci_src = c->items[i];
                    uir_case_item_t *nci = (uir_case_item_t *)uir_alloc_node(
                        unit, UIR_CASE_ITEM, sizeof(uir_case_item_t), loc);
                    if (!nci) continue;
                    nci->body = clone_node_internal(unit, ci_src->body, subst);
                    nci->pattern_count = ci_src->pattern_count;
                    if (nci->pattern_count > 0) {
                        nci->patterns = calloc(nci->pattern_count, sizeof(uir_node_t *));
                        if (nci->patterns) {
                            for (size_t p = 0; p < nci->pattern_count; p++)
                                nci->patterns[p] = clone_node_internal(unit, ci_src->patterns[p], subst);
                        }
                    }
                    items[i] = nci;
                }
            }
            uir_node_t *def = c->default_item ? clone_node_internal(unit, c->default_item, subst) : NULL;
            uir_case_t *nc = (uir_case_t *)uir_alloc_node(unit, UIR_CASE, sizeof(uir_case_t), loc);
            if (nc) {
                nc->expr = expr;
                nc->items = items;
                nc->item_count = item_count;
                nc->default_item = def;
                nc->is_wildcard = c->is_wildcard;
            }
            return (uir_node_t *)nc;
        }

        case UIR_LOOP: {
            uir_loop_t *l = (uir_loop_t *)src;
            uir_node_t *init = clone_node_internal(unit, l->init_stmt, subst);
            uir_node_t *cond = clone_node_internal(unit, l->condition, subst);
            uir_node_t *step = clone_node_internal(unit, l->step_stmt, subst);
            uir_node_t *body = clone_node_internal(unit, l->body, subst);
            if (body) {
                uir_loop_t *nl = (uir_loop_t *)uir_alloc_node(unit, UIR_LOOP, sizeof(uir_loop_t), loc);
                if (nl) {
                    nl->init_stmt = init;
                    nl->condition = cond;
                    nl->step_stmt = step;
                    nl->body = body;
                    return (uir_node_t *)nl;
                }
            }
            return NULL;
        }

        case UIR_LOOP_BACK: {
            uir_loop_back_t *lb = (uir_loop_back_t *)src;
            uir_node_t *cond = clone_node_internal(unit, lb->condition, subst);
            uir_node_t *body = clone_node_internal(unit, lb->body, subst);
            if (body) {
                uir_loop_back_t *nlb = (uir_loop_back_t *)uir_alloc_node(unit, UIR_LOOP_BACK, sizeof(uir_loop_back_t), loc);
                if (nlb) {
                    nlb->condition = cond;
                    nlb->body = body;
                    return (uir_node_t *)nlb;
                }
            }
            return NULL;
        }

        case UIR_SYS_TASK: {
            uir_sys_task_t *t = (uir_sys_task_t *)src;
            uir_node_t **cloned_args = NULL;
            if (t->arg_count > 0) {
                cloned_args = calloc(t->arg_count, sizeof(uir_node_t *));
                if (!cloned_args) return NULL;
                for (size_t i = 0; i < t->arg_count; i++)
                    cloned_args[i] = clone_node_internal(unit, t->args[i], subst);
            }
            return uir_make_sys_task(unit, t->task_kind, t->filename, t->mem_name,
                                     t->fmt, cloned_args, t->arg_count, loc);
        }

        case UIR_FUNC_CALL:
        case UIR_TASK_ENABLE: {
            uir_func_call_t *fc = (uir_func_call_t *)src;
            uir_node_t **cloned_args = NULL;
            if (fc->arg_count > 0) {
                cloned_args = calloc(fc->arg_count, sizeof(uir_node_t *));
                if (!cloned_args) return NULL;
                for (size_t i = 0; i < fc->arg_count; i++)
                    cloned_args[i] = clone_node_internal(unit, fc->args[i], subst);
            }
            uir_func_call_t *nfc = (uir_func_call_t *)uir_alloc_node(
                unit, src->kind, sizeof(uir_func_call_t), loc);
            if (!nfc) { free(cloned_args); return NULL; }
            nfc->name = strdup(fc->name);
            nfc->args = cloned_args;
            nfc->arg_count = fc->arg_count;
            return (uir_node_t *)nfc;
        }

        case UIR_FUNC_DEF:
        case UIR_TASK_DEF: {
            uir_func_t *ft = (uir_func_t *)src;
            uir_func_t *nft = uir_add_func_task(unit, ft->name, ft->is_function);
            if (!nft) return NULL;
            nft->return_width = ft->return_width;
            /* Clone ports */
            for (size_t i = 0; i < ft->port_count; i++)
                uir_add_func_port(nft, ft->ports[i].name,
                                   ft->ports[i].direction, ft->ports[i].width);
            /* Clone locals (signal nodes) */
            for (size_t i = 0; i < ft->local_count; i++) {
                uir_node_t *cl = clone_node_internal(unit, ft->locals[i], subst);
                if (cl) uir_add_func_local(nft, cl);
            }
            /* Clone body */
            if (ft->body)
                nft->body = clone_node_internal(unit, ft->body, subst);
            return (uir_node_t *)nft;
        }

        case UIR_GENERATE: {
            uir_generate_t *gsrc = (uir_generate_t *)src;
            uir_generate_t *ngen = uir_add_generate(unit, gsrc->gen_type);
            if (!ngen) return NULL;
            ngen->label = gsrc->label ? strdup(gsrc->label) : NULL;
            ngen->genvar_name = gsrc->genvar_name ? strdup(gsrc->genvar_name) : NULL;
            ngen->for_init = gsrc->for_init ? clone_node_internal(unit, gsrc->for_init, subst) : NULL;
            ngen->for_cond = gsrc->for_cond ? clone_node_internal(unit, gsrc->for_cond, subst) : NULL;
            ngen->for_step = gsrc->for_step ? clone_node_internal(unit, gsrc->for_step, subst) : NULL;
            ngen->for_direction = gsrc->for_direction;
            ngen->if_condition = gsrc->if_condition ? clone_node_internal(unit, gsrc->if_condition, subst) : NULL;
            /* Deep-clone body_template and else_body_template into independent
             * units, so each GEN_IF clone can safely evaluate its condition
             * and clone items from the selected branch without sharing/use-after-free. */
            if (gsrc->body_template) {
                uir_loc_t tloc = {NULL, 0, 0};
                ngen->body_template = uir_create_design_unit(
                    "_gen_body_clone_", "verilog", tloc);
                for (size_t j = 0; j < gsrc->body_template->signal_count; j++)
                    clone_node_internal(ngen->body_template,
                        (uir_node_t *)gsrc->body_template->signals[j], subst);
                for (size_t j = 0; j < gsrc->body_template->process_count; j++)
                    clone_node_internal(ngen->body_template,
                        (uir_node_t *)gsrc->body_template->processes[j], subst);
                for (size_t j = 0; j < gsrc->body_template->instance_count; j++)
                    clone_node_internal(ngen->body_template,
                        (uir_node_t *)gsrc->body_template->instances[j], subst);
                for (size_t j = 0; j < gsrc->body_template->assign_count; j++)
                    clone_node_internal(ngen->body_template,
                        (uir_node_t *)gsrc->body_template->assigns[j], subst);
                for (size_t j = 0; j < gsrc->body_template->generate_count; j++)
                    clone_node_internal(ngen->body_template,
                        (uir_node_t *)gsrc->body_template->generates[j], subst);
            }
            if (gsrc->else_body_template) {
                uir_loc_t tloc = {NULL, 0, 0};
                ngen->else_body_template = uir_create_design_unit(
                    "_gen_else_body_clone_", "verilog", tloc);
                for (size_t j = 0; j < gsrc->else_body_template->signal_count; j++)
                    clone_node_internal(ngen->else_body_template,
                        (uir_node_t *)gsrc->else_body_template->signals[j], subst);
                for (size_t j = 0; j < gsrc->else_body_template->process_count; j++)
                    clone_node_internal(ngen->else_body_template,
                        (uir_node_t *)gsrc->else_body_template->processes[j], subst);
                for (size_t j = 0; j < gsrc->else_body_template->instance_count; j++)
                    clone_node_internal(ngen->else_body_template,
                        (uir_node_t *)gsrc->else_body_template->instances[j], subst);
                for (size_t j = 0; j < gsrc->else_body_template->assign_count; j++)
                    clone_node_internal(ngen->else_body_template,
                        (uir_node_t *)gsrc->else_body_template->assigns[j], subst);
                for (size_t j = 0; j < gsrc->else_body_template->generate_count; j++)
                    clone_node_internal(ngen->else_body_template,
                        (uir_node_t *)gsrc->else_body_template->generates[j], subst);
            }
            /* Clone case fields */
            ngen->case_expr = gsrc->case_expr
                ? clone_node_internal(unit, gsrc->case_expr, subst) : NULL;
            if (gsrc->case_item_count > 0) {
                ngen->case_item_count = gsrc->case_item_count;
                ngen->case_item_templates = calloc(gsrc->case_item_count, sizeof(uir_design_unit_t *));
                ngen->case_item_patterns = calloc(gsrc->case_item_count, sizeof(uir_node_t **));
                ngen->case_item_pattern_counts = calloc(gsrc->case_item_count, sizeof(size_t));
                if (ngen->case_item_templates && ngen->case_item_patterns) {
                    for (size_t ci = 0; ci < gsrc->case_item_count; ci++) {
                        ngen->case_item_pattern_counts[ci] = gsrc->case_item_pattern_counts[ci];
                        if (gsrc->case_item_patterns[ci] && gsrc->case_item_pattern_counts[ci] > 0) {
                            ngen->case_item_patterns[ci] = calloc(
                                gsrc->case_item_pattern_counts[ci], sizeof(uir_node_t *));
                            for (size_t p = 0; p < gsrc->case_item_pattern_counts[ci]; p++)
                                ngen->case_item_patterns[ci][p] = clone_node_internal(
                                    unit, gsrc->case_item_patterns[ci][p], subst);
                        }
                        if (gsrc->case_item_templates[ci]) {
                            uir_loc_t tloc = {NULL, 0, 0};
                            ngen->case_item_templates[ci] = uir_create_design_unit(
                                "_gen_case_item_clone_", "verilog", tloc);
                            uir_design_unit_t *tsrc = gsrc->case_item_templates[ci];
                            for (size_t j = 0; j < tsrc->signal_count; j++)
                                clone_node_internal(ngen->case_item_templates[ci],
                                    (uir_node_t *)tsrc->signals[j], subst);
                            for (size_t j = 0; j < tsrc->process_count; j++)
                                clone_node_internal(ngen->case_item_templates[ci],
                                    (uir_node_t *)tsrc->processes[j], subst);
                            for (size_t j = 0; j < tsrc->instance_count; j++)
                                clone_node_internal(ngen->case_item_templates[ci],
                                    (uir_node_t *)tsrc->instances[j], subst);
                            for (size_t j = 0; j < tsrc->assign_count; j++)
                                clone_node_internal(ngen->case_item_templates[ci],
                                    (uir_node_t *)tsrc->assigns[j], subst);
                            for (size_t j = 0; j < tsrc->generate_count; j++)
                                clone_node_internal(ngen->case_item_templates[ci],
                                    (uir_node_t *)tsrc->generates[j], subst);
                        }
                    }
                }
            }
            if (gsrc->case_default_template) {
                uir_loc_t tloc = {NULL, 0, 0};
                ngen->case_default_template = uir_create_design_unit(
                    "_gen_case_def_clone_", "verilog", tloc);
                uir_design_unit_t *dsrc_u = gsrc->case_default_template;
                for (size_t j = 0; j < dsrc_u->signal_count; j++)
                    clone_node_internal(ngen->case_default_template,
                        (uir_node_t *)dsrc_u->signals[j], subst);
                for (size_t j = 0; j < dsrc_u->process_count; j++)
                    clone_node_internal(ngen->case_default_template,
                        (uir_node_t *)dsrc_u->processes[j], subst);
                for (size_t j = 0; j < dsrc_u->instance_count; j++)
                    clone_node_internal(ngen->case_default_template,
                        (uir_node_t *)dsrc_u->instances[j], subst);
                for (size_t j = 0; j < dsrc_u->assign_count; j++)
                    clone_node_internal(ngen->case_default_template,
                        (uir_node_t *)dsrc_u->assigns[j], subst);
                for (size_t j = 0; j < dsrc_u->generate_count; j++)
                    clone_node_internal(ngen->case_default_template,
                        (uir_node_t *)dsrc_u->generates[j], subst);
            }
            return (uir_node_t *)ngen;
        }

        case UIR_DELAY: {
            uir_delay_t *dsrc = (uir_delay_t *)src;
            uir_delay_t *nd = (uir_delay_t *)uir_alloc_node(unit, UIR_DELAY, sizeof(uir_delay_t), src->loc);
            if (!nd) return NULL;
            nd->delay_value = dsrc->delay_value
                ? clone_node_internal(unit, dsrc->delay_value, subst) : NULL;
            nd->body = dsrc->body
                ? clone_node_internal(unit, dsrc->body, subst) : NULL;
            nd->always_loop = dsrc->always_loop;
            return (uir_node_t *)nd;
        }

        case UIR_WAIT: {
            uir_wait_t *wsrc = (uir_wait_t *)src;
            uir_wait_t *nw = (uir_wait_t *)uir_alloc_node(unit, UIR_WAIT, sizeof(uir_wait_t), src->loc);
            if (!nw) return NULL;
            nw->condition = wsrc->condition
                ? clone_node_internal(unit, wsrc->condition, subst) : NULL;
            nw->body = wsrc->body
                ? clone_node_internal(unit, wsrc->body, subst) : NULL;
            return (uir_node_t *)nw;
        }

        case UIR_EVENT_CTRL: {
            uir_event_ctrl_t *esrc = (uir_event_ctrl_t *)src;
            uir_event_ctrl_t *ne = (uir_event_ctrl_t *)uir_alloc_node(unit, UIR_EVENT_CTRL, sizeof(uir_event_ctrl_t), src->loc);
            if (!ne) return NULL;
            ne->signal_name = esrc->signal_name ? strdup(esrc->signal_name) : NULL;
            ne->edge = esrc->edge;
            ne->body = esrc->body
                ? clone_node_internal(unit, esrc->body, subst) : NULL;
            return (uir_node_t *)ne;
        }

        case UIR_DISABLE: {
            uir_disable_t *dsrc = (uir_disable_t *)src;
            uir_disable_t *nd = (uir_disable_t *)uir_alloc_node(unit, UIR_DISABLE, sizeof(uir_disable_t), src->loc);
            if (!nd) return NULL;
            nd->target_name = dsrc->target_name ? strdup(dsrc->target_name) : NULL;
            return (uir_node_t *)nd;
        }

        case UIR_SYS_FUNC_EXPR: {
            uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)src;
            uir_node_t **cloned_args = NULL;
            if (sf->arg_count > 0) {
                cloned_args = calloc(sf->arg_count, sizeof(uir_node_t *));
                if (!cloned_args) return NULL;
                for (size_t i = 0; i < sf->arg_count; i++)
                    cloned_args[i] = clone_node_internal(unit, sf->args[i], subst);
            }
            return uir_make_sys_func_expr(unit, sf->func_kind, cloned_args, sf->arg_count, loc);
        }

        case UIR_FORCE: {
            uir_force_t *fsrc = (uir_force_t *)src;
            uir_force_t *nf = (uir_force_t *)uir_alloc_node(unit, UIR_FORCE, sizeof(uir_force_t), src->loc);
            if (!nf) return NULL;
            nf->lhs = fsrc->lhs ? clone_node_internal(unit, fsrc->lhs, subst) : NULL;
            nf->rhs = fsrc->rhs ? clone_node_internal(unit, fsrc->rhs, subst) : NULL;
            return (uir_node_t *)nf;
        }

        case UIR_RELEASE: {
            uir_release_t *rsrc = (uir_release_t *)src;
            uir_release_t *nr = (uir_release_t *)uir_alloc_node(unit, UIR_RELEASE, sizeof(uir_release_t), src->loc);
            if (!nr) return NULL;
            nr->target = rsrc->target ? clone_node_internal(unit, rsrc->target, subst) : NULL;
            return (uir_node_t *)nr;
        }

        default:
            /* For unhandled node kinds, return NULL */
            return NULL;
    }
}

uir_node_t *uir_clone_node(uir_design_unit_t *target_unit,
                            uir_node_t *src,
                            const uir_subst_t *subst)
{
    return clone_node_internal(target_unit, src, subst);
}

/* ── UDP construction ── */

uir_udp_t *uir_add_udp(uir_design_unit_t *unit, int is_sequential)
{
    if (!unit) return NULL;
    uir_loc_t loc = {NULL, 0, 0};
    uir_udp_t *udp = (uir_udp_t *)uir_alloc_node(unit, UIR_UDP, sizeof(uir_udp_t), loc);
    if (!udp) return NULL;
    udp->is_sequential = is_sequential;
    udp->entries = NULL;
    udp->entry_count = 0;
    unit->udp = udp;
    return udp;
}

void uir_add_udp_entry(uir_udp_t *udp, const char *input_pattern,
                        const char *state_and_output)
{
    if (!udp || !input_pattern || !state_and_output) return;
    size_t n = udp->entry_count;
    uir_udp_entry_t *ne = realloc(udp->entries, (n + 1) * sizeof(uir_udp_entry_t));
    if (!ne) return;
    udp->entries = ne;
    udp->entries[n].input_pattern = strdup(input_pattern);
    if (udp->is_sequential && strlen(state_and_output) >= 2) {
        udp->entries[n].current_state = state_and_output[0];
        udp->entries[n].output = state_and_output[1];
    } else {
        udp->entries[n].current_state = '\0';
        udp->entries[n].output = state_and_output[0];
    }
    udp->entry_count = n + 1;
}
