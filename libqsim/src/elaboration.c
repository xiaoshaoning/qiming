#include "libqsim/elaboration.h"
#include "libqsim/vhdl_library.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ── */

static char *elab_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void add_diag(uir_elab_result_t *r, const char *msg) {
    char **new_d = realloc(r->diagnostics, (r->diag_count + 1) * sizeof(char *));
    if (!new_d) return;
    r->diagnostics = new_d;

    qsim_recovery_t **new_rec = realloc(r->recoveries, (r->diag_count + 1) * sizeof(qsim_recovery_t *));
    if (!new_rec) return;
    r->recoveries = new_rec;

    r->diagnostics[r->diag_count] = elab_strdup(msg);
    r->recoveries[r->diag_count] = NULL;
    r->diag_count++;
}

static void add_diag_with_recovery(uir_elab_result_t *r, const char *msg, qsim_recovery_t *rec) {
    add_diag(r, msg);
    if (r->diag_count > 0)
        r->recoveries[r->diag_count - 1] = rec;
}

/* Split off the first component of a dot-separated path.
 * Returns the component (up to sep) in buf, and returns pointer to rest.
 * If no dot, copies entire path to buf and returns NULL.
 */
static const char *split_hier(const char *path, char *buf, size_t buf_size) {
    const char *dot = strchr(path, '.');
    size_t len;
    if (dot) {
        len = (size_t)(dot - path);
        if (len >= buf_size) len = buf_size - 1;
        memcpy(buf, path, len);
        buf[len] = '\0';
        return dot + 1;
    }
    len = strlen(path);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
    return NULL;
}

/* ── Instance binding ── */

/* Build a name-to-unit mapping */
typedef struct {
    char **names;
    uir_design_unit_t **units;
    size_t count;
} unit_map_t;

static void unit_map_init(unit_map_t *m, uir_design_unit_t **units, size_t count) {
    m->names = NULL;
    m->units = NULL;
    m->count = 0;
    for (size_t i = 0; i < count; i++) {
        if (!units[i] || !units[i]->name) continue;
        char **nn = realloc(m->names, (m->count + 1) * sizeof(char *));
        if (!nn) return;
        m->names = nn;
        uir_design_unit_t **nu = realloc(m->units, (m->count + 1) * sizeof(uir_design_unit_t *));
        if (!nu) return;
        m->units = nu;
        m->names[m->count] = units[i]->name;
        m->units[m->count] = units[i];
        m->count++;
    }
}

static void unit_map_free(unit_map_t *m) {
    free(m->names);
    free(m->units);
    m->names = NULL;
    m->units = NULL;
    m->count = 0;
}

static uir_design_unit_t *unit_map_lookup(unit_map_t *m, const char *name) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->names[i], name) == 0)
            return m->units[i];
    }
    return NULL;
}

static void bind_instances(uir_design_unit_t *unit, unit_map_t *map, uir_elab_result_t *result) {
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (inst->bound_to) continue; /* already bound */

        uir_design_unit_t *target = unit_map_lookup(map, inst->module_name);
        if (!target) {
            char msg[256];
            snprintf(msg, sizeof(msg), "instance %s: module '%s' not found",
                     inst->instance_name, inst->module_name);

            /* Generate recovery with name suggestions */
            qsim_recovery_t *rec = NULL;
            if (map->count > 0) {
                const char **names = malloc(map->count * sizeof(const char *));
                if (names) {
                    for (size_t n = 0; n < map->count; n++)
                        names[n] = map->names[n];
                    rec = qsim_recovery_from_names(inst->module_name, names,
                                                    map->count, "compile");
                    free(names);
                }
            }
            add_diag_with_recovery(result, msg, rec);
            result->success = 0;
            continue;
        }
        inst->bound_to = target;
        uir_add_child(unit, target);
    }
}

/* ── Port binding ── */

static void bind_ports(uir_design_unit_t *unit, uir_elab_result_t *result) {
    (void)result;
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;

        uir_design_unit_t *target = inst->bound_to;

        /* Validate each connection */
        for (size_t j = 0; j < inst->connection_count; j++) {
            uir_port_connection_t *conn = &inst->connections[j];
            if (!conn->formal_name) continue; /* bare expression */

            /* Find matching formal port */
            int found = 0;
            for (size_t k = 0; k < target->port_count; k++) {
                if (strcmp(target->ports[k]->name, conn->formal_name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg), "instance %s: port '%s' not found on module '%s'",
                         inst->instance_name, conn->formal_name, inst->module_name);

                /* Generate recovery with port name suggestions */
                qsim_recovery_t *rec = NULL;
                if (target->port_count > 0) {
                    const char **names = malloc(target->port_count * sizeof(const char *));
                    if (names) {
                        for (size_t n = 0; n < target->port_count; n++)
                            names[n] = target->ports[n]->name;
                        rec = qsim_recovery_from_names(conn->formal_name, names,
                                                        target->port_count, "compile");
                        free(names);
                    }
                }
                add_diag_with_recovery(result, msg, rec);
                result->success = 0;
            }
        }
    }
}

/* ── Hierarchical signal resolution ── */

static uir_node_t *find_local(uir_design_unit_t *unit, const char *name) {
    /* Check ports */
    for (size_t i = 0; i < unit->port_count; i++) {
        if (strcmp(unit->ports[i]->name, name) == 0)
            return (uir_node_t *)unit->ports[i];
    }
    /* Check signals */
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (strcmp(unit->signals[i]->name, name) == 0)
            return (uir_node_t *)unit->signals[i];
    }
    return NULL;
}

uir_node_t *uir_find_signal_hier(uir_design_unit_t *top, const char *hier_path) {
    if (!top || !hier_path) return NULL;

    char comp[256];
    const char *rest = split_hier(hier_path, comp, sizeof(comp));

    if (!rest) {
        /* Simple name — check top-level */
        return uir_find_signal(top, hier_path);
    }

    /* First component should be an instance name in top */
    for (size_t i = 0; i < top->instance_count; i++) {
        if (strcmp(top->instances[i]->instance_name, comp) == 0) {
            uir_design_unit_t *sub = top->instances[i]->bound_to;
            if (!sub) return NULL;
            return uir_find_signal_hier(sub, rest);
        }
    }

    return NULL;
}

/* ── Sensitivity resolution ── */

static void resolve_sensitivity(uir_design_unit_t *unit) {
    for (size_t i = 0; i < unit->process_count; i++) {
        uir_process_t *proc = unit->processes[i];
        if (!proc->sensitivity_list || proc->sensitivity_count == 0) continue;

        for (size_t j = 0; j < proc->sensitivity_count; j++) {
            uir_sensitivity_t *se = &proc->sensitivity_list[j];
            if (se->signal) continue; /* already resolved */

            /* The sensitivity entry stores the signal pointer.
             * If it's NULL, we need to resolve it.
             * Currently the parser doesn't populate sensitivity entries
             * with signal references — they're just stored as edge flags.
             * For now we leave them as-is since the parser handles them. */
        }
    }
}

/* ── Flat signal table ── */

/* Recursively collect all signals into a flat table with hierarchical names */
typedef struct {
    uir_node_t **signals;
    char **names;
    size_t count;
    size_t cap;
} flat_signal_table_t;

static void flat_add(flat_signal_table_t *t, uir_node_t *sig, const char *name) {
    if (t->count >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        uir_node_t **ns = realloc(t->signals, t->cap * sizeof(uir_node_t *));
        if (!ns) return;
        t->signals = ns;
        char **nn = realloc(t->names, t->cap * sizeof(char *));
        if (!nn) return;
        t->names = nn;
    }
    t->signals[t->count] = sig;
    t->names[t->count] = elab_strdup(name);
    t->count++;
}

static void flatten_unit(flat_signal_table_t *t, uir_design_unit_t *unit, const char *prefix) {
    char path[512];

    /* Add ports */
    for (size_t i = 0; i < unit->port_count; i++) {
        if (prefix[0])
            snprintf(path, sizeof(path), "%s.%s", prefix, unit->ports[i]->name);
        else
            snprintf(path, sizeof(path), "%s", unit->ports[i]->name);
        flat_add(t, (uir_node_t *)unit->ports[i], path);
    }

    /* Add signals */
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (prefix[0])
            snprintf(path, sizeof(path), "%s.%s", prefix, unit->signals[i]->name);
        else
            snprintf(path, sizeof(path), "%s", unit->signals[i]->name);
        flat_add(t, (uir_node_t *)unit->signals[i], path);
    }

    /* Recurse into instances */
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;
        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);
        flatten_unit(t, inst->bound_to, child_prefix);
    }
}

/* ── Use-clause resolution ── */

static void resolve_use_clauses(uir_design_unit_t **units, size_t count,
    vhdl_library_registry_t *reg, uir_elab_result_t *result)
{
    for (size_t i = 0; i < count; i++) {
        uir_design_unit_t *unit = units[i];
        if (!unit || unit->use_count == 0) continue;
        if (!unit->language || strcmp(unit->language, "vhdl") != 0) continue;

        for (size_t j = 0; j < unit->use_count; j++) {
            const char *clause = unit->use_clauses[j];
            if (!clause) continue;

            /* Split "ieee.std_logic_1164.all" → lib="ieee", pkg="std_logic_1164" */
            char lib[64], pkg[64];
            const char *first_dot = strchr(clause, '.');
            if (!first_dot) continue; /* malformed */
            size_t lib_len = (size_t)(first_dot - clause);
            if (lib_len >= sizeof(lib)) lib_len = sizeof(lib) - 1;
            memcpy(lib, clause, lib_len);
            lib[lib_len] = '\0';

            const char *rest = first_dot + 1;
            const char *second_dot = strchr(rest, '.');
            const char *pkg_end = second_dot ? second_dot : rest + strlen(rest);
            size_t pkg_len = (size_t)(pkg_end - rest);
            if (pkg_len >= sizeof(pkg)) pkg_len = sizeof(pkg) - 1;
            memcpy(pkg, rest, pkg_len);
            pkg[pkg_len] = '\0';

            vhdl_package_entry_t *entry = vhdl_library_find(reg, lib, pkg);
            if (!entry) {
                char msg[256];
                snprintf(msg, sizeof(msg), "use clause: library/package '%s.%s' not found (unit '%s')",
                         lib, pkg, unit->name ? unit->name : "?");
                add_diag(result, msg);
                /* Not fatal — backward compat with designs that rely on builtins */
            }
        }
    }
}

/* ── Main elaboration entry point ── */

uir_elab_result_t *uir_elaborate(uir_design_unit_t **units, size_t count) {
    uir_elab_result_t *result = calloc(1, sizeof(uir_elab_result_t));
    if (!result) return NULL;

    if (!units || count == 0) {
        result->success = 1;
        return result;
    }

    result->success = 1;

    /* Phase 0: Expand generate constructs */
    uir_expand_generates(units, count);

    /* Phase 0.5: Resolve library/use clauses */
    vhdl_library_registry_t lib_reg;
    vhdl_library_registry_init(&lib_reg);
    vhdl_library_register_builtins(&lib_reg);

    /* Register user-parsed VHDL packages (entities, package decls) as work.*/
    for (size_t i = 0; i < count; i++) {
        if (units[i] && units[i]->language &&
            strcmp(units[i]->language, "vhdl") == 0 && units[i]->name) {
            vhdl_library_register(&lib_reg, "work", units[i]->name, units[i], 0);
        }
    }
    resolve_use_clauses(units, count, &lib_reg, result);

    /* Phase 1: Build module map and bind instances */
    unit_map_t map;
    unit_map_init(&map, units, count);

    for (size_t i = 0; i < count; i++) {
        if (units[i]) {
            bind_instances(units[i], &map, result);
        }
    }

    /* Phase 2: Bind ports */
    for (size_t i = 0; i < count; i++) {
        if (units[i])
            bind_ports(units[i], result);
    }

    /* Phase 3: Resolve sensitivity lists */
    for (size_t i = 0; i < count; i++) {
        if (units[i])
            resolve_sensitivity(units[i]);
    }

    /* Return the elaborated units */
    result->units = units;
    result->unit_count = count;

    unit_map_free(&map);
    vhdl_library_registry_destroy(&lib_reg);
    return result;
}

void uir_elab_result_free(uir_elab_result_t *result) {
    if (!result) return;
    for (size_t i = 0; i < result->diag_count; i++) {
        free(result->diagnostics[i]);
        if (result->recoveries && result->recoveries[i]) {
            for (size_t j = 0; j < result->recoveries[i]->suggestion_count; j++)
                free(result->recoveries[i]->suggestions[j]);
            free(result->recoveries[i]->suggestions);
            for (size_t j = 0; j < result->recoveries[i]->nearby_count; j++)
                free(result->recoveries[i]->nearby[j]);
            free(result->recoveries[i]->nearby);
            free(result->recoveries[i]);
        }
    }
    free(result->diagnostics);
    free(result->recoveries);
    free(result);
}

/* ── Generate expansion (elaboration-time) ── */

/* Evaluate a constant UIR expression to uint64_t, with genvar substitution.
 * genvar_name may be NULL (no substitution). */
static uint64_t eval_const_u64_subst(uir_design_unit_t *unit, uir_node_t *node,
                                      const char *genvar_name,
                                      uint64_t genvar_value)
{
    if (!node) return 0;
    switch (node->kind) {
        case UIR_LITERAL: {
            uir_literal_t *l = (uir_literal_t *)node;
            uint64_t val = 0;
            for (uint32_t i = 0; i < l->value->width && i < 64; i++)
                if (qsim_bit_get(l->value, i).state == QSIM_1) val |= (1ULL << i);
            return val;
        }
        case UIR_REF: {
            uir_ref_t *r = (uir_ref_t *)node;
            if (genvar_name && r->name && strcmp(r->name, genvar_name) == 0)
                return genvar_value;
            /* Check if the ref is a module parameter */
            if (unit && r->name) {
                for (size_t pi = 0; pi < unit->param_count; pi++) {
                    if (unit->params[pi].hier_path &&
                        strcmp(unit->params[pi].hier_path, r->name) == 0) {
                        return eval_const_u64_subst(unit, unit->params[pi].value, NULL, 0);
                    }
                }
            }
            return 0; /* unknown signal ref treated as 0 for constant eval */
        }
        case UIR_EXPR_BINARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            uint64_t a = eval_const_u64_subst(unit, e->operand_a, genvar_name, genvar_value);
            uint64_t b = eval_const_u64_subst(unit, e->operand_b, genvar_name, genvar_value);
            switch (e->op.bin_op) {
                case UIR_OP_ADD: return a + b;
                case UIR_OP_SUB: return a - b;
                case UIR_OP_MUL: return a * b;
                case UIR_OP_DIV: return b ? a / b : 0;
                case UIR_OP_MOD: return b ? a % b : 0;
                case UIR_OP_AND: return a & b;
                case UIR_OP_OR:  return a | b;
                case UIR_OP_XOR: return a ^ b;
                case UIR_OP_SHL: return a << b;
                case UIR_OP_SHR: return a >> b;
                case UIR_OP_LT:  return a < b ? 1 : 0;
                case UIR_OP_GT:  return a > b ? 1 : 0;
                case UIR_OP_LE:  return a <= b ? 1 : 0;
                case UIR_OP_GE:  return a >= b ? 1 : 0;
                case UIR_OP_EQ:  return a == b ? 1 : 0;
                case UIR_OP_NEQ: return a != b ? 1 : 0;
                default: return 0;
            }
        }
        case UIR_EXPR_UNARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            uint64_t a = eval_const_u64_subst(unit, e->operand_a, genvar_name, genvar_value);
            switch (e->op.un_op) {
                case UIR_OP_NEG: return (uint64_t)(-(int64_t)a);
                case UIR_OP_NOT: return ~a;
                default: return 0;
            }
        }
        default: return 0;
    }
}

/* Expand a single generate for-loop node. Clones body items for each
 * iteration with genvar substitution and name prefixing. */
static void expand_generate_for(uir_design_unit_t *unit, uir_generate_t *gen)
{
    if (!gen->body_template || gen->body_item_count == 0) return;
    if (!gen->label) return;
    const char *genvar_name = gen->genvar_name;

    if (gen->for_direction != 0) {
        /* VHDL-style: for <id> in <lo> to/downto <hi> generate */
        if (!gen->for_init || !gen->for_cond) return;
        uint64_t lo = eval_const_u64_subst(unit, gen->for_init, genvar_name, 0);
        uint64_t hi = eval_const_u64_subst(unit, gen->for_cond, genvar_name, 0);

        int max_iter = 1000;
        int iter = 0;
        uint64_t val = lo;

        while (iter < max_iter) {
            if (gen->for_direction > 0) {
                if (val > hi) break;
            } else {
                if (val < hi) break;
            }

            char prefix[1024];
            snprintf(prefix, sizeof(prefix), "%s[%llu].", gen->label,
                     (unsigned long long)val);

            uir_subst_t subst;
            subst.genvar_name = genvar_name;
            subst.genvar_value = val;
            subst.name_prefix = prefix;

            for (size_t i = 0; i < gen->body_item_count; i++) {
                uir_node_t *cloned = uir_clone_node(unit, gen->body_items[i], &subst);
                (void)cloned;
            }

            val = gen->for_direction > 0 ? val + 1 : val - 1;
            iter++;
        }
        return;
    }

    /* Verilog-style: init/cond/step */
    if (!gen->for_init || !gen->for_cond || !gen->for_step) return;

    /* Evaluate initial value */
    uint64_t val = eval_const_u64_subst(unit, gen->for_init, genvar_name, 0);

    /* Safety limit to prevent infinite loops */
    int max_iter = 1000;
    int iter = 0;

    while (iter < max_iter) {
        /* Evaluate condition with current genvar value */
        uint64_t cond = eval_const_u64_subst(unit, gen->for_cond, genvar_name, val);
        if (cond == 0) break;

        /* Build name prefix: "label[val]." */
        char prefix[1024];
        snprintf(prefix, sizeof(prefix), "%s[%llu].", gen->label,
                 (unsigned long long)val);

        /* Create substitution descriptor */
        uir_subst_t subst;
        subst.genvar_name = genvar_name;
        subst.genvar_value = val;
        subst.name_prefix = prefix;

        /* Clone each body item into the parent unit */
        for (size_t i = 0; i < gen->body_item_count; i++) {
            uir_node_t *cloned = uir_clone_node(unit, gen->body_items[i], &subst);
            (void)cloned;
        }

        /* Evaluate step expression to get next genvar value */
        val = eval_const_u64_subst(unit, gen->for_step, genvar_name, val);
        iter++;
    }
}

/* Expand all generate constructs in a single design unit.
 * Iterates until no new generates remain (handles nested expansion). */
static void expand_unit_generates(uir_design_unit_t *unit)
{
    if (!unit) return;

    while (unit->generate_count > 0) {
        size_t count = unit->generate_count;

        for (size_t i = 0; i < count; i++) {
            uir_generate_t *gen = unit->generates[i];
            if (!gen) continue;

            switch (gen->gen_type) {
                case UIR_GEN_LOOP:
                    expand_generate_for(unit, gen);
                    break;
                case UIR_GEN_IF: {
                    uint64_t cond = gen->if_condition
                        ? eval_const_u64_subst(unit, gen->if_condition, NULL, 0) : 0;
                    uir_design_unit_t *temp = cond ? gen->body_template : gen->else_body_template;
                    if (temp) {
                        for (size_t j = 0; j < temp->signal_count; j++)
                            uir_clone_node(unit, (uir_node_t *)temp->signals[j], NULL);
                        for (size_t j = 0; j < temp->process_count; j++)
                            uir_clone_node(unit, (uir_node_t *)temp->processes[j], NULL);
                        for (size_t j = 0; j < temp->instance_count; j++)
                            uir_clone_node(unit, (uir_node_t *)temp->instances[j], NULL);
                        for (size_t j = 0; j < temp->assign_count; j++)
                            uir_clone_node(unit, (uir_node_t *)temp->assigns[j], NULL);
                        for (size_t j = 0; j < temp->generate_count; j++)
                            uir_clone_node(unit, (uir_node_t *)temp->generates[j], NULL);
                    }
                    break;
                }
                case UIR_GEN_BLOCK:
                    break;
                case UIR_GEN_CASE: {
                    uint64_t case_val = gen->case_expr
                        ? eval_const_u64_subst(unit, gen->case_expr, NULL, 0) : 0;
                    int matched = 0;
                    for (size_t ci = 0; ci < gen->case_item_count && !matched; ci++) {
                        for (size_t p = 0; p < gen->case_item_pattern_counts[ci]; p++) {
                            uint64_t pv = gen->case_item_patterns[ci][p]
                                ? eval_const_u64_subst(unit, gen->case_item_patterns[ci][p], NULL, 0) : 0;
                            if (pv == case_val) { matched = 1; break; }
                        }
                        if (matched && gen->case_item_templates[ci]) {
                            uir_design_unit_t *t = gen->case_item_templates[ci];
                            for (size_t j = 0; j < t->signal_count; j++)
                                uir_clone_node(unit, (uir_node_t *)t->signals[j], NULL);
                            for (size_t j = 0; j < t->process_count; j++)
                                uir_clone_node(unit, (uir_node_t *)t->processes[j], NULL);
                            for (size_t j = 0; j < t->instance_count; j++)
                                uir_clone_node(unit, (uir_node_t *)t->instances[j], NULL);
                            for (size_t j = 0; j < t->assign_count; j++)
                                uir_clone_node(unit, (uir_node_t *)t->assigns[j], NULL);
                            for (size_t j = 0; j < t->generate_count; j++)
                                uir_clone_node(unit, (uir_node_t *)t->generates[j], NULL);
                        }
                    }
                    if (!matched && gen->case_default_template) {
                        uir_design_unit_t *t = gen->case_default_template;
                        for (size_t j = 0; j < t->signal_count; j++)
                            uir_clone_node(unit, (uir_node_t *)t->signals[j], NULL);
                        for (size_t j = 0; j < t->process_count; j++)
                            uir_clone_node(unit, (uir_node_t *)t->processes[j], NULL);
                        for (size_t j = 0; j < t->instance_count; j++)
                            uir_clone_node(unit, (uir_node_t *)t->instances[j], NULL);
                        for (size_t j = 0; j < t->assign_count; j++)
                            uir_clone_node(unit, (uir_node_t *)t->assigns[j], NULL);
                        for (size_t j = 0; j < t->generate_count; j++)
                            uir_clone_node(unit, (uir_node_t *)t->generates[j], NULL);
                    }
                    break;
                }
                    break;
            }

            if (gen->body_template) {
                uir_destroy_design_unit(gen->body_template);
                gen->body_template = NULL;
            }
            if (gen->else_body_template) {
                uir_destroy_design_unit(gen->else_body_template);
                gen->else_body_template = NULL;
            }
        }

        /* Free sub-allocations for processed generates */
        for (size_t i = 0; i < count; i++) {
            if (unit->generates[i]) {
                free(unit->generates[i]->label);
                free(unit->generates[i]->genvar_name);
                free(unit->generates[i]->body_items);
                free(unit->generates[i]->else_body_items);
                unit->generates[i]->label = NULL;
                unit->generates[i]->genvar_name = NULL;
                unit->generates[i]->body_items = NULL;
                unit->generates[i]->else_body_items = NULL;
                unit->generates[i]->for_init = NULL;
                unit->generates[i]->for_cond = NULL;
                unit->generates[i]->for_step = NULL;
                unit->generates[i]->if_condition = NULL;
            }
        }

        /* Keep any newly added generates for the next iteration */
        size_t remaining = unit->generate_count - count;
        if (remaining > 0) {
            memmove(unit->generates, unit->generates + count,
                    remaining * sizeof(uir_generate_t *));
            unit->generate_count = remaining;
        } else {
            free(unit->generates);
            unit->generates = NULL;
            unit->generate_count = 0;
        }
    }
}

void uir_expand_generates(uir_design_unit_t **units, size_t count)
{
    if (!units || count == 0) return;

    for (size_t i = 0; i < count; i++) {
        if (units[i])
            expand_unit_generates(units[i]);
    }
}
