#include "libqsim/verilog_parser.h"
#include "libqsim/verilog_preprocessor.h"
#include "libqsim/uir.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Parser context (shared with grammar.peg actions)
 * ================================================================ */

static const char *_parse_filename = NULL;
static uir_design_unit_t *_parse_unit = NULL;
static int _parse_ok = 0;
static int _parse_all_consumed = 0;

/* Multi-definition support: collect all parsed units from one source file */
static uir_design_unit_t **_parse_units = NULL;
static size_t _parse_unit_count = 0;
static size_t _parse_unit_cap = 0;

static void save_unit(void) {
    if (!_parse_unit) return;
    if (_parse_unit_count >= _parse_unit_cap) {
        size_t nc = _parse_unit_cap ? _parse_unit_cap * 2 : 8;
        uir_design_unit_t **nu = realloc(_parse_units, nc * sizeof(uir_design_unit_t *));
        if (!nu) return;
        _parse_units = nu;
        _parse_unit_cap = nc;
    }
    _parse_units[_parse_unit_count++] = _parse_unit;
    _parse_unit = NULL;
}

/* Generate block active flag — if 0, body items are parsed but not emitted */
static int _parse_generate_active = 1;
static uir_design_unit_t *_parse_saved_unit = NULL;  /* save/restore for generate */

/* Generate for loop state */
static char *_parse_genvar_id = NULL;    /* genvar identifier name (for current loop header) */

/* Track all declared genvar names to avoid creating implicit wires for them.
   Fixed-size array — generous enough for any single module. */
#define MAX_GENVARS 64
static char *_parse_genvar_names[MAX_GENVARS];
static int   _parse_genvar_name_count = 0;

static char *parse_strdup(const char *s); /* forward declaration */

static void add_genvar_name(const char *name) {
    if (_parse_genvar_name_count >= MAX_GENVARS) return;
    for (int i = 0; i < _parse_genvar_name_count; i++)
        if (strcmp(_parse_genvar_names[i], name) == 0) return; /* already tracked */
    _parse_genvar_names[_parse_genvar_name_count++] = parse_strdup(name);
}

static int is_genvar_name(const char *name) {
    if (_parse_genvar_id && strcmp(name, _parse_genvar_id) == 0) return 1;
    for (int i = 0; i < _parse_genvar_name_count; i++)
        if (strcmp(_parse_genvar_names[i], name) == 0) return 1;
    return 0;
}

static int is_param_name(uir_design_unit_t *unit, const char *name) {
    if (!unit || !name) return 0;
    for (size_t i = 0; i < unit->param_count; i++)
        if (unit->params[i].hier_path && strcmp(unit->params[i].hier_path, name) == 0)
            return 1;
    return 0;
}

/* Port signal type tracking for ANSI port lists (output reg -> UIR_SIG_REG). */
static uir_signal_type_t _parse_port_sig_type = UIR_SIG_WIRE;

/* Net type tracking for net_decl — set by net_type grammar rule. */
static uir_signal_type_t _parse_net_type = UIR_SIG_WIRE;

/* Signed declaration flag — set by KW_SIGNED, used by decl_* helpers. */
static int _parse_decl_signed = 0;

/* Automatic function/task flag — set by KW_AUTOMATIC before function/task keyword. */
static int _parse_decl_automatic = 0;

/* Modport direction tracking inside interface modport declarations. */
static uir_port_dir_t _parse_modport_dir = UIR_PORT_IN;

/* ── Non-ANSI port tracking ── */
#define MAX_BARE_PORTS 64
static char *_parse_bare_port_names[MAX_BARE_PORTS];
static int _parse_bare_port_count = 0;

static void add_bare_port_name(const char *name) {
    if (!name || _parse_bare_port_count >= MAX_BARE_PORTS) return;
    for (int i = 0; i < _parse_bare_port_count; i++)
        if (_parse_bare_port_names[i] && strcmp(_parse_bare_port_names[i], name) == 0) return;
    _parse_bare_port_names[_parse_bare_port_count++] = parse_strdup(name);
}

static int is_bare_port_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < _parse_bare_port_count; i++)
        if (_parse_bare_port_names[i] && strcmp(_parse_bare_port_names[i], name) == 0) return 1;
    return 0;
}

static void mark_bare_port_handled(const char *name) {
    if (!name) return;
    for (int i = 0; i < _parse_bare_port_count; i++) {
        if (_parse_bare_port_names[i] && strcmp(_parse_bare_port_names[i], name) == 0) {
            free(_parse_bare_port_names[i]);
            _parse_bare_port_names[i] = NULL;
            return;
        }
    }
}

/* Create default INOUT ports for any bare port names not matched by
 * a direction declaration (input/output/inout) in the module body. */
static void create_ports_from_named(void) {
    if (!_parse_unit) return;
    for (int i = 0; i < _parse_bare_port_count; i++) {
        if (_parse_bare_port_names[i]) {
            uir_add_port(_parse_unit, _parse_bare_port_names[i], UIR_PORT_INOUT, 0, 0, UIR_SIG_WIRE);
        }
    }
}

static char *_parse_gen_label = NULL;    /* generate block label (save/restore for nesting) */
static char *_parse_gen_label_stack[16]; /* saved outer labels for nested gen_blocks */
static int   _parse_gen_label_sp = 0;    /* stack pointer */
static uir_generate_t *_parse_current_gen = NULL;  /* current generate node */

/* Generate if — nested/elaboration-time state (inside generate-for body) */
static int _parse_gen_if_nested = 0;             /* 1 if creating elaboration-time GEN_IF */
static uir_generate_t *_parse_gen_if_node = NULL;/* the UIR_GEN_IF node being built */
static uir_design_unit_t *_parse_gen_if_outer = NULL; /* saved _parse_unit for restore */
static int _parse_gen_if_branch = 0;             /* 1=TRUE, 2=ELSE */

/* Generate case state */
static uint64_t _parse_gen_case_val;     /* evaluated case expression */
static int      _parse_gen_case_matched; /* 1 if any item has matched */
static int      _parse_gen_case_saved_sp;/* expression stack boundary */

/* String save for cross-action state (strdup'd, freed after use) */
static char *_parse_saved = NULL;

/* Current always/initial process being parsed */
static uir_process_t *_parse_process = NULL;

/* Range [msb:lsb] capture from port_item and declarations */
static uint32_t _parse_range_msb = 0;
static uint32_t _parse_range_lsb = 0;
static int _parse_saved_range_width = 1;   /* for comma-separated IDs */

/* Array dimension [msb:lsb] capture */
static uint32_t _parse_array_msb = 0;
static uint32_t _parse_array_lsb = 0;
static uint32_t _parse_array_size = 0;        /* product of all unpacked dims; 0 = not an array */
static int _parse_saved_array_size = 0;       /* for comma-separated IDs */
static uint32_t _parse_array_dims[4];   /* individual unpacked dimensions */
static int _parse_array_dims_count = 0;       /* number of unpacked dimensions */

/* Multi-dimensional array index tracking */
static uir_node_t *_parse_multi_indices[8];
static int _parse_multi_index_count;
static char *_parse_multi_id;

/* Sensitivity list tracking during always-block parsing */
static char **_parse_sens_names = NULL;
static int *_parse_sens_edges = NULL;
static size_t _parse_sens_count = 0;
static int _parse_edge_flag = 0;

/* Block stack for nested statement collection */
static uir_block_t *_parse_block_stack[16];
static int _parse_block_sp = -1;

/* If-statement temporaries (set by grammar actions).
 * Nested if-statements (from else-if chains) overwrite these globals,
 * so we push/pop them. */
static uir_block_t *_parse_then_block = NULL;
static uir_block_t *_parse_else_block = NULL;
static int _parse_had_else = 0;
#define MAX_IF_NESTING 16
static uir_block_t *_parse_saved_then[MAX_IF_NESTING];
static uir_block_t *_parse_saved_else[MAX_IF_NESTING];
static int _parse_saved_had_else[MAX_IF_NESTING];
static int _parse_if_depth = 0;

/* LHS array index for blocking/nonblocking assignments */
static uir_node_t *_parse_lhs_index = NULL;

/* LHS name for assign/blocking/nonblocking assignments (separate from
 * _parse_saved to avoid conflict with primary_expr array-read rule). */
static char *_parse_assign_lhs = NULL;
/* Temporary for primary_expr array-read ID name (separate from _parse_saved) */
static char *_parse_array_id = NULL;
static uir_node_t *_parse_lhs_hi = NULL;
static uir_node_t *_parse_lhs_lo = NULL;

/* Delay value saved by do_save_delay() for either continuous assign or
 * statement-level delay control (#N stmt). Consumed by do_delay_finish()
 * or do_assign_stmt_finish(). */
static uir_node_t *_parse_assign_delay = NULL;

/* Case-statement temporaries */
static uir_case_item_t **_parse_case_items = NULL;
static size_t _parse_case_item_count = 0;
static size_t _parse_case_item_cap = 0;
static uir_node_t *_parse_case_default = NULL;
static int _parse_case_saved_sp_stack[16];
static int _parse_case_saved_sp_depth = 0;
static uir_node_t **_parse_case_item_patterns = NULL;
static size_t _parse_case_item_pattern_count = 0;
static uir_block_t *_parse_case_item_body = NULL;
static uir_node_t *_parse_case_default_body = NULL;

/* Auto-sensitivity flag for @(*) */
static int _parse_auto_sens = 0;

/* Stack marker for concat expression (saved before expr_list inside concat_expr) */
static int _parse_concat_sp = 0;
/* Stack of saved expression positions for replication+concat alt1 (handles nesting) */
static int _repl_sp_stack[16];
static int _repl_sp_ptr = -1;

/* Replication count saved from NUMBER before {expr} inside concat_expr */
static int _parse_repl_count = 0;

/* Names for concatenation LHS assignments: assign {a, b} = expr */
static char *_parse_concat_names[16];
static int _parse_concat_count = 0;

/* Function/task parsing state */
static uir_func_t *_parse_func = NULL;
static char *_parse_func_call_name = NULL;
static int _parse_func_ports_mode = 0;
static int _parse_func_call_sp = 0;

/* ── Loop (for/while/repeat/forever) parsing state ── */

static uir_node_t *_parse_loop_cond = NULL;  /* loop condition expression */
static uir_node_t *_parse_loop_init = NULL;  /* for-loop init statement */
static uir_node_t *_parse_loop_step = NULL;  /* for-loop step statement */
static uir_node_t *_parse_loop_count = NULL; /* repeat count expression */
static uir_block_t *_parse_loop_body = NULL; /* loop body block */

/* ── Named block state (for "begin : name ... end" and disable) ── */
static char *_parse_block_name = NULL;       /* saved name for named seq_block */

/* ── Attribute tracking for (* name [= value] *) ── */
#define MAX_ATTRS 64
static char *_parse_attr_names[MAX_ATTRS];
static char *_parse_attr_values[MAX_ATTRS];
static int _parse_attr_count = 0;

/* ── Defparam tracking ── */
static char *_parse_defparam_path = NULL;

/* ── Parameter tracking (parameter name = value) ── */
static char *_parse_param_name = NULL;

/* ── Specify / path-delay parsing state ── */
static uir_specify_t *_parse_specify = NULL;  /* current specify block */
static char *_parse_saved2 = NULL;             /* second string temp */
static char *_parse_saved3 = NULL;             /* third string temp (modport connections) */
static uir_node_t *_path_rise = NULL;          /* path rise delay expr */
static uir_node_t *_path_fall = NULL;          /* path fall delay expr */
static uir_node_t *_path_z = NULL;             /* full-path Z delay expr */
static uir_node_t *_path_x = NULL;             /* full-path X delay expr */
static char *_parse_specparam_name = NULL;     /* specparam name temp */
static int _path_src_edge = 0;                 /* 0=none, 1=posedge, -1=negedge */
static int _path_dst_pol = 0;                  /* 0=none, 1=+: same, -1=-: opposite */
static uir_node_t *_path_condition = NULL;     /* conditional path expression */
static char *_path_data_src = NULL;            /* data terminal expression data source */

/* ── UDP (User-Defined Primitive) parsing state ── */
static int _parse_udp_is_seq = 0;              /* 1 if sequential UDP */
static int _parse_udp_output_is_reg = 0;       /* 1 if KW_REG seen in output decl */
static char _parse_udp_edge_buf[16];           /* edge symbol from table row */
static char _parse_udp_input_buf[64];          /* input pattern accumulator */
static int _parse_udp_input_len;               /* chars in input_buf */
static char _parse_udp_state_char;             /* current state from table row */
static char _parse_udp_output_char;            /* output from table row */

/* ── Template-mode generate case state ── */
static int _parse_gen_case_active = 0;
static uir_generate_t *_parse_gen_case_node = NULL;
static uir_design_unit_t *_parse_gen_case_outer = NULL;
static uir_design_unit_t *_parse_gen_case_cur_temp = NULL;
static int _parse_gen_case_default_mode = 0;

/* ================================================================
 * Expression value stack (separate from peg's YYSTYPE stack)
 * ================================================================ */

#define EXPR_STACK_MAX 256
static uir_node_t *_expr_stack[EXPR_STACK_MAX];
static int _expr_sp = 0;

static void expr_push(uir_node_t *n) {
    if (_expr_sp < EXPR_STACK_MAX) _expr_stack[_expr_sp++] = n;
}

static uir_node_t *expr_pop(void) {
    return _expr_sp > 0 ? _expr_stack[--_expr_sp] : NULL;
}

/* Save current expression stack depth for replication+concat deferred action.
 * Uses a dedicated stack to handle nested replication concat expressions. */
static void do_save_repl_sp(void) {
    if (_repl_sp_ptr < 15) _repl_sp_stack[++_repl_sp_ptr] = _expr_sp;
}

/* ── Statement block stack ── */

static void push_stmt_block(uir_block_t *block) {
    if (_parse_block_sp < 15) _parse_block_stack[++_parse_block_sp] = block;
}

static void pop_stmt_block(void) {
    if (_parse_block_sp >= 0) _parse_block_sp--;
}

static uir_block_t *current_block(void) {
    return _parse_block_sp >= 0 ? _parse_block_stack[_parse_block_sp] : NULL;
}

static void append_stmt(uir_node_t *stmt) {
    uir_block_t *b = current_block();
    if (!b) return;
    uir_node_t **ns = realloc(b->stmts, (b->stmt_count + 1) * sizeof(uir_node_t *));
    if (ns) { b->stmts = ns; b->stmts[b->stmt_count++] = stmt; }
}

/* Append a statement directly to a specific block (no current_block dependency). */
static void append_to_block(uir_block_t *block, uir_node_t *stmt) {
    if (!block || !stmt) return;
    uir_node_t **ns = realloc(block->stmts, (block->stmt_count + 1) * sizeof(uir_node_t *));
    if (ns) { block->stmts = ns; block->stmts[block->stmt_count++] = stmt; }
}

/* Forward declaration needed by delay helpers below (parse_loc defined later) */
static uir_loc_t parse_loc(void);

/* ── Delay control helpers ── */

static void do_save_delay(void) {
    uir_node_t *delay_val = expr_pop();
    _parse_assign_delay = delay_val;
}

/* Wraps the last appended statement in the current block in a UIR_DELAY
 * node. Called from grammar action: delay_control statement { do_delay_finish(); } */
static void do_delay_finish(void) {
    if (!_parse_assign_delay || !_parse_unit) {
        _parse_assign_delay = NULL;
        return;
    }
    uir_block_t *b = current_block();
    if (!b || b->stmt_count == 0) {
        _parse_assign_delay = NULL;
        return;
    }
    /* Remove last statement from block */
    uir_node_t *body = b->stmts[b->stmt_count - 1];
    b->stmt_count--;

    uir_delay_t *d = (uir_delay_t *)uir_alloc_node(_parse_unit, UIR_DELAY, sizeof(uir_delay_t), parse_loc());
    if (!d) { _parse_assign_delay = NULL; return; }
    d->delay_value = _parse_assign_delay;
    d->body = body;
    d->always_loop = (_parse_process &&
        _parse_process->proc_kind == UIR_PROC_ALWAYS &&
        _parse_sens_count == 0 && !_parse_auto_sens);
    _parse_assign_delay = NULL;

    /* Re-append the delay wrapper as the last statement */
    append_stmt((uir_node_t *)d);
}

/* ── Named block helpers (for "begin : name ... end") ── */

/* Called from seq_block rule when "begin : name" is parsed.
 * Creates a new named block, pushes it on the stack, and makes it current. */
static void begin_named_block(const char *name) {
    if (!_parse_unit) return;
    uir_block_t *b = uir_add_block(_parse_unit, 1);
    if (!b) return;
    b->name = parse_strdup(name);
    push_stmt_block(b);
}

/* Called from seq_block rule at "end" for a named block.
 * Pops the named block and appends it as a single statement to the parent block. */
static void end_named_block(void) {
    pop_stmt_block();
    uir_block_t *b = current_block();
    if (!b) {
        /* If no parent block (e.g., top-level named block), nothing to append to.
         * The named block was allocated via uir_add_block and registered in the
         * unit, so it's not leaked. */
        return;
    }
    /* The last block on the stack before begin_named_block pushed a new one is our parent.
     * The named block is NOT a child of b — it was created by uir_add_block which
     * registered it as a design-level item (signals/processes/etc), but it's NOT a
     * top-level item. We need to add it to the parent block's stmts. */
    /* The named block is the block that was just popped. However, since we used
     * uir_add_block, it's NOT in b->stmts. We need to find it.
     * We track _parse_block_name for this. */
    if (_parse_block_name) {
        /* Find the last-allocated block with this name (it's the one we just popped) */
        if (_parse_unit && _parse_unit->node_count > 0) {
            for (size_t i = _parse_unit->node_count; i > 0; i--) {
                uir_node_t *node = _parse_unit->all_nodes[i - 1];
                if (node->kind == UIR_BLOCK) {
                    uir_block_t *nb = (uir_block_t *)node;
                    if (nb->name && strcmp(nb->name, _parse_block_name) == 0) {
                        append_to_block(b, (uir_node_t *)nb);
                        free(_parse_block_name);
                        _parse_block_name = NULL;
                        break;
                    }
                }
            }
        }
    }
}

/* ── Wait statement helper ── */

static void do_wait_finish(void) {
    if (!_parse_unit) return;
    uir_node_t *cond = expr_pop();
    uir_block_t *b = current_block();
    if (!b || b->stmt_count == 0 || !cond) {
        if (cond) expr_push(cond); /* push back */
        return;
    }
    uir_node_t *body = b->stmts[b->stmt_count - 1];
    b->stmt_count--;

    uir_wait_t *w = (uir_wait_t *)uir_alloc_node(_parse_unit, UIR_WAIT, sizeof(uir_wait_t), parse_loc());
    if (!w) return;
    w->condition = cond;
    w->body = body;
    append_stmt((uir_node_t *)w);
}

/* ── Event control helpers ── */

/* Called from event_control rule: @(posedge/negedge signal) */
static void do_event_enter(const char *signal_name) {
    _parse_saved = parse_strdup(signal_name);
}

/* Called after event_control statement is parsed.
 * Wraps the last appended statement in UIR_EVENT_CTRL. */
static void do_event_finish(void) {
    if (!_parse_unit || !_parse_saved) {
        _parse_edge_flag = 0;
        return;
    }
    uir_block_t *b = current_block();
    if (!b || b->stmt_count == 0) {
        free(_parse_saved); _parse_saved = NULL;
        _parse_edge_flag = 0;
        return;
    }
    uir_node_t *body = b->stmts[b->stmt_count - 1];
    b->stmt_count--;

    uir_event_ctrl_t *ev = (uir_event_ctrl_t *)uir_alloc_node(
        _parse_unit, UIR_EVENT_CTRL, sizeof(uir_event_ctrl_t), parse_loc());
    if (!ev) {
        free(_parse_saved); _parse_saved = NULL;
        _parse_edge_flag = 0;
        return;
    }
    ev->signal_name = _parse_saved;
    ev->edge = _parse_edge_flag;
    ev->body = body;
    _parse_saved = NULL;
    _parse_edge_flag = 0;
    append_stmt((uir_node_t *)ev);
}

/* ── Disable statement helper ── */

static void do_disable_stmt(const char *name) {
    if (!_parse_unit) return;
    uir_disable_t *d = (uir_disable_t *)uir_alloc_node(
        _parse_unit, UIR_DISABLE, sizeof(uir_disable_t), parse_loc());
    if (!d) return;
    d->target_name = parse_strdup(name);
    append_stmt((uir_node_t *)d);
}

/* ── Force/release statement helpers ── */

static const char *skip_past_keyword(const char *text) {
    while (*text && ((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z')))
        text++;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
        text++;
    return text;
}

static void do_force_stmt(uir_design_unit_t *unit) {
    if (!unit || !_parse_saved) return;
    const char *name = skip_past_keyword(_parse_saved);
    uir_node_t *rhs = expr_pop();
    if (!rhs) return;
    uir_node_t *lhs = uir_make_ref(unit, name, parse_loc());
    if (!lhs) return;
    uir_force_t *f = (uir_force_t *)uir_alloc_node(
        unit, UIR_FORCE, sizeof(uir_force_t), parse_loc());
    if (!f) return;
    f->lhs = lhs;
    f->rhs = rhs;
    append_stmt((uir_node_t *)f);
}

static void do_release_stmt(uir_design_unit_t *unit) {
    if (!unit || !_parse_saved) return;
    const char *name = skip_past_keyword(_parse_saved);
    uir_node_t *tgt = uir_make_ref(unit, name, parse_loc());
    if (!tgt) return;
    uir_release_t *r = (uir_release_t *)uir_alloc_node(
        unit, UIR_RELEASE, sizeof(uir_release_t), parse_loc());
    if (!r) return;
    r->target = tgt;
    append_stmt((uir_node_t *)r);
}

/* ── Specify block action functions ── */

static void specify_enter(void) {
    if (!_parse_unit) return;
    _parse_specify = uir_add_specify(_parse_unit);
}

static void specify_leave(void) {
    _parse_specify = NULL;
}

static void save_specparam_name(const char *name) {
    if (!name) return;
    free(_parse_specparam_name);
    _parse_specparam_name = parse_strdup(name);
}

static void save_specparam_value(void) {
    if (!_parse_specify || !_parse_specparam_name) return;
    uir_node_t *val = expr_pop();
    if (val)
        uir_add_specparam(_parse_specify, _parse_specparam_name, val);
    free(_parse_specparam_name);
    _parse_specparam_name = NULL;
}

static void do_parallel_path(uir_specify_t *spec, const char *src, const char *dst,
                              const char *data_src,
                              uir_node_t *rise, uir_node_t *fall,
                              int src_edge, int dst_pol, uir_node_t *condition)
{
    if (!spec || !src || !dst) return;
    uir_path_delay_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.src = parse_strdup(src);
    pd.dst = parse_strdup(dst);
    pd.data_src = data_src ? parse_strdup(data_src) : NULL;
    pd.type = UIR_PATH_PARALLEL;
    pd.rise_delay = rise;
    pd.fall_delay = fall;
    pd.src_edge = src_edge;
    pd.dst_polarity = dst_pol;
    pd.condition = condition;
    uir_add_specpath(spec, &pd);
}

static void do_full_path(uir_specify_t *spec, const char *src, const char *dst,
                          const char *data_src,
                          uir_node_t *rise, uir_node_t *fall, uir_node_t *z, uir_node_t *x,
                          int src_edge, int dst_pol, uir_node_t *condition)
{
    if (!spec || !src || !dst) return;
    uir_path_delay_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.src = parse_strdup(src);
    pd.dst = parse_strdup(dst);
    pd.data_src = data_src ? parse_strdup(data_src) : NULL;
    pd.type = UIR_PATH_FULL;
    pd.rise_delay = rise;
    pd.fall_delay = fall;
    pd.z_delay = z;
    pd.x_delay = x;
    pd.src_edge = src_edge;
    pd.dst_polarity = dst_pol;
    pd.condition = condition;
    uir_add_specpath(spec, &pd);
}

static void do_timing_check(uir_specify_t *spec, uir_timing_check_kind_t kind,
                             const char *data, const char *ref, uir_node_t *limit,
                             const char *notifier)
{
    if (!spec) return;
    uir_timing_check_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.kind = kind;
    tc.data_pin = data ? parse_strdup(data) : NULL;
    tc.ref_pin = ref ? parse_strdup(ref) : NULL;
    tc.limit = limit;
    tc.notifier = notifier ? parse_strdup(notifier) : NULL;
    uir_add_timing_check(spec, &tc);
}

/* ── UDP primitive action functions ── */

static void primitive_enter(void) {
    if (!_parse_unit) return;
    _parse_udp_is_seq = 0;
    _parse_udp_output_is_reg = 0;
    _parse_udp_edge_buf[0] = '\0';
    _parse_udp_input_len = 0;
    _parse_udp_state_char = '\0';
    _parse_udp_output_char = '\0';
    uir_add_udp(_parse_unit, 0);
}

static void primitive_leave(void) {
}

static void udp_output_decl(const char *name) {
    if (!_parse_unit) return;
    uir_signal_type_t sig_type = _parse_udp_output_is_reg ? UIR_SIG_REG : UIR_SIG_WIRE;
    uir_add_port(_parse_unit, name, UIR_PORT_OUT, 0, 0, sig_type);
    mark_bare_port_handled(name);
    if (_parse_udp_output_is_reg) {
        _parse_udp_is_seq = 1;
        if (_parse_unit->udp) _parse_unit->udp->is_sequential = 1;
    }
}

static void udp_input_decl(const char *name) {
    if (!_parse_unit) return;
    uir_add_port(_parse_unit, name, UIR_PORT_IN, 0, 0, UIR_SIG_WIRE);
    mark_bare_port_handled(name);
}

static void udp_reg_decl(const char *name) {
    if (!_parse_unit) return;
    _parse_udp_is_seq = 1;
    if (_parse_unit->udp) _parse_unit->udp->is_sequential = 1;
    mark_bare_port_handled(name);
    for (size_t i = 0; i < _parse_unit->port_count; i++) {
        if (_parse_unit->ports[i]->direction == UIR_PORT_OUT &&
            strcmp(_parse_unit->ports[i]->name, name) == 0) {
            _parse_unit->ports[i]->sig_type = UIR_SIG_REG;
            break;
        }
    }
}

static void udp_table_enter(void) {
    _parse_udp_edge_buf[0] = '\0';
    _parse_udp_input_len = 0;
    _parse_udp_state_char = '\0';
    _parse_udp_output_char = '\0';
}

static void udp_table_leave(void) {
}

static void udp_parse_row(const char *row) {
    if (!_parse_unit || !_parse_unit->udp || !row) return;
    if (!*row) return;

    _parse_udp_edge_buf[0] = '\0';
    _parse_udp_state_char = '\0';
    _parse_udp_output_char = '\0';

    const char *p = row;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) return;

    if (*p == '(') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && strchr("01xX?bB-", *p)) {
            _parse_udp_edge_buf[0] = '(';
            _parse_udp_edge_buf[1] = *p;
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p && strchr("01xX?bB-", *p)) {
            _parse_udp_edge_buf[2] = *p;
            _parse_udp_edge_buf[3] = ')';
            _parse_udp_edge_buf[4] = '\0';
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ')') p++;
    } else if (strchr("rRfFpPnN*", *p)) {
        _parse_udp_edge_buf[0] = *p;
        _parse_udp_edge_buf[1] = '\0';
        p++;
    }

    int colon_count = 0;
    _parse_udp_input_len = 0;
    memset(_parse_udp_input_buf, 0, sizeof(_parse_udp_input_buf));

    while (*p && *p != ';') {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { p++; continue; }
        if (c == ':') { colon_count++; p++; continue; }
        if (c == '0' || c == '1' || c == 'x' || c == 'X' ||
            c == '?' || c == 'b' || c == 'B' || c == '-') {
            if (colon_count == 0) {
                if (_parse_udp_input_len < (int)(sizeof(_parse_udp_input_buf) - 1))
                    _parse_udp_input_buf[_parse_udp_input_len++] = c;
            } else if (colon_count == 1) {
                _parse_udp_state_char = c;
            } else if (colon_count >= 2) {
                _parse_udp_output_char = c;
            }
            p++;
        } else {
            p++;
        }
    }

    int is_seq = _parse_unit->udp->is_sequential;
    char pattern[128];
    pattern[0] = '\0';
    if (_parse_udp_edge_buf[0])
        strcat(pattern, _parse_udp_edge_buf);
    strncat(pattern, _parse_udp_input_buf, sizeof(pattern) - strlen(pattern) - 1);

    char state_and_output[3] = "";
    if (is_seq) {
        state_and_output[0] = _parse_udp_state_char ? _parse_udp_state_char : '?';
        state_and_output[1] = _parse_udp_output_char ? _parse_udp_output_char : _parse_udp_state_char;
    } else {
        if (_parse_udp_output_char)
            state_and_output[0] = _parse_udp_output_char;
        else if (_parse_udp_state_char)
            state_and_output[0] = _parse_udp_state_char;
        else if (_parse_udp_input_len > 0)
            state_and_output[0] = 'x';
    }

    if (pattern[0] && state_and_output[0])
        uir_add_udp_entry(_parse_unit->udp, pattern, state_and_output);
}

/* Post-parse pass that restructures sequential delays in a block so that
 * remaining statements after a delay are nested inside the delay's body.
 *
 * Transforms:  Block[Delay{5, a=1}, Delay{10, a=0}]
 *       into:  Block[Delay{5, Block[a=1, Delay{10, a=0}]}]
 *
 * This ensures proper temporal semantics: each delay's body includes all
 * statements that should execute after it fires (including subsequent delays). */
static void restructure_delays(uir_block_t *block) {
    if (!block) return;

    /* Recursively restructure nested blocks and delay bodies first */
    for (size_t i = 0; i < block->stmt_count; i++) {
        uir_node_t *stmt = block->stmts[i];
        if (stmt->kind == UIR_BLOCK) {
            restructure_delays((uir_block_t *)stmt);
        } else if (stmt->kind == UIR_DELAY) {
            uir_delay_t *d = (uir_delay_t *)stmt;
            if (d->body && d->body->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)d->body);
        } else if (stmt->kind == UIR_IF) {
            uir_if_t *ifn = (uir_if_t *)stmt;
            if (ifn->then_branch && ifn->then_branch->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)ifn->then_branch);
            if (ifn->else_branch && ifn->else_branch->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)ifn->else_branch);
        } else if (stmt->kind == UIR_LOOP) {
            uir_loop_t *lp = (uir_loop_t *)stmt;
            if (lp->body && lp->body->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)lp->body);
        } else if (stmt->kind == UIR_WAIT) {
            uir_wait_t *w = (uir_wait_t *)stmt;
            if (w->body && w->body->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)w->body);
        } else if (stmt->kind == UIR_EVENT_CTRL) {
            uir_event_ctrl_t *ec = (uir_event_ctrl_t *)stmt;
            if (ec->body && ec->body->kind == UIR_BLOCK)
                restructure_delays((uir_block_t *)ec->body);
        }
    }

    /* Find first blocking node in this block (UIR_DELAY, UIR_WAIT, UIR_EVENT_CTRL).
     * If there are remaining statements after it, wrap them into a continuation. */
    for (size_t i = 0; i < block->stmt_count; i++) {
        uir_node_t *s = block->stmts[i];
        if (s->kind == UIR_DELAY || s->kind == UIR_WAIT || s->kind == UIR_EVENT_CTRL) {
            size_t remaining = block->stmt_count - i - 1;
            if (remaining > 0) {
                uir_block_t *cont = uir_add_block(_parse_unit, 1);
                if (!cont) return;
                /* Get the body field of the blocking node */
                uir_node_t *body = NULL;
                if (s->kind == UIR_DELAY) body = ((uir_delay_t *)s)->body;
                else if (s->kind == UIR_WAIT) body = ((uir_wait_t *)s)->body;
                else if (s->kind == UIR_EVENT_CTRL) body = ((uir_event_ctrl_t *)s)->body;
                /* Add the original body first */
                if (body) {
                    uir_node_t **ns = realloc(cont->stmts, (cont->stmt_count + 1) * sizeof(uir_node_t *));
                    if (ns) { cont->stmts = ns; cont->stmts[cont->stmt_count++] = body; }
                }
                /* Then add remaining statements */
                for (size_t j = i + 1; j < block->stmt_count; j++) {
                    uir_node_t **ns = realloc(cont->stmts, (cont->stmt_count + 1) * sizeof(uir_node_t *));
                    if (ns) { cont->stmts = ns; cont->stmts[cont->stmt_count++] = block->stmts[j]; }
                }
                /* Update the blocking node's body to the continuation */
                if (s->kind == UIR_DELAY) ((uir_delay_t *)s)->body = (uir_node_t *)cont;
                else if (s->kind == UIR_WAIT) ((uir_wait_t *)s)->body = (uir_node_t *)cont;
                else if (s->kind == UIR_EVENT_CTRL) ((uir_event_ctrl_t *)s)->body = (uir_node_t *)cont;
                block->stmt_count = i + 1;
            }
            break; /* Only the first blocking node in the block needs restructuring */
        }
    }
}

/* ================================================================
 * Source input for peg (read via YY_INPUT)
 * ================================================================ */

static const char *_parse_source = NULL;
static size_t _parse_source_len = 0;
static size_t _parse_source_pos = 0;

/* ================================================================
 * Helper functions for grammar actions
 * ================================================================ */

static uir_loc_t parse_loc(void) {
    uir_loc_t loc = {_parse_filename, 0, 0};
    return loc;
}

static char *parse_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void record_sensitivity(const char *name) {
    char **nn = realloc(_parse_sens_names, (_parse_sens_count + 1) * sizeof(char *));
    int *ee = realloc(_parse_sens_edges, (_parse_sens_count + 1) * sizeof(int));
    if (!nn || !ee) return;
    _parse_sens_names = nn;
    _parse_sens_edges = ee;
    _parse_sens_names[_parse_sens_count] = parse_strdup(name);
    _parse_sens_edges[_parse_sens_count] = _parse_edge_flag;
    _parse_sens_count++;
    _parse_edge_flag = 0;
}

static void finalize_process_sensitivity(uir_process_t *proc) {
    if (!proc || _parse_sens_count == 0) goto cleanup;
    proc->sensitivity_list = calloc(_parse_sens_count, sizeof(uir_sensitivity_t));
    if (!proc->sensitivity_list) goto cleanup;
    proc->sensitivity_count = _parse_sens_count;
    for (size_t i = 0; i < _parse_sens_count; i++) {
        proc->sensitivity_list[i].edge = _parse_sens_edges[i];
        proc->sensitivity_list[i].signal = uir_find_signal(
            _parse_unit, _parse_sens_names[i]);
    }
cleanup:
    for (size_t i = 0; i < _parse_sens_count; i++)
        free(_parse_sens_names[i]);
    free(_parse_sens_names);
    free(_parse_sens_edges);
    _parse_sens_names = NULL;
    _parse_sens_edges = NULL;
    _parse_sens_count = 0;
}

static void start_always_process(uir_design_unit_t *unit) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_ALWAYS);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
    }
}

static void start_initial_process(uir_design_unit_t *unit) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_INITIAL);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
    }
}

static void start_always_comb_process(uir_design_unit_t *unit) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_ALWAYS_COMB);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
        _parse_auto_sens = 1;
    }
}

static void start_always_ff_process(uir_design_unit_t *unit) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_ALWAYS_FF);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
    }
}

static void start_always_latch_process(uir_design_unit_t *unit) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_ALWAYS_LATCH);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
        _parse_auto_sens = 1;
    }
}

static void finish_process(void) {
    if (_parse_process) {
        pop_stmt_block();
        restructure_delays((uir_block_t *)_parse_process->body);
        finalize_process_sensitivity(_parse_process);
        if (_parse_auto_sens) {
            _parse_process->auto_sens = 1;
            _parse_auto_sens = 0;
        }
        _parse_process = NULL;
    }
}

/* Forward declaration for array_dims helper (defined after range_width). */
static void set_signal_array_dims(uir_signal_t *s);

/* ── Declaration dispatchers (function-aware) ── */

static void decl_input(const char *name, uint32_t width, uint32_t array_size) {
    if (_parse_func) {
        uir_add_func_port(_parse_func, name, UIR_PORT_IN, width);
    } else if (_parse_unit) {
        if (is_bare_port_name(name)) {
            uint32_t msb = (width > 0) ? width - 1 : 0;
            uir_port_t *p = uir_add_port(_parse_unit, name, UIR_PORT_IN, msb, 0, UIR_SIG_WIRE);
            if (p) p->is_signed = _parse_decl_signed;
            mark_bare_port_handled(name);
        } else {
            uir_signal_t *s = uir_add_signal(_parse_unit, name, UIR_SIG_WIRE, width, array_size);
            if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
        }
    }
}

static void do_decl_init_assign(const char *name) {
    if (!_parse_unit || !name) return;
    uir_node_t *rhs = expr_pop();
    if (!rhs) return;
    uir_node_t *lhs = uir_make_ref(_parse_unit, name, parse_loc());
    if (!lhs) return;
    uir_assign_t *a = uir_add_assign(_parse_unit);
    if (a) { a->lhs = lhs; a->rhs = rhs; }
}

static void decl_output(const char *name, uint32_t width, uint32_t array_size) {
    if (_parse_func) {
        uir_add_func_port(_parse_func, name, UIR_PORT_OUT, width);
    } else if (_parse_unit) {
        if (is_bare_port_name(name)) {
            uint32_t msb = (width > 0) ? width - 1 : 0;
            uir_port_t *p = uir_add_port(_parse_unit, name, UIR_PORT_OUT, msb, 0, _parse_port_sig_type);
            if (p) p->is_signed = _parse_decl_signed;
            mark_bare_port_handled(name);
        } else {
            uir_signal_t *s = uir_add_signal(_parse_unit, name, _parse_port_sig_type, width, array_size);
            if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
        }
    }
}

static void decl_inout(const char *name, uint32_t width, uint32_t array_size) {
    if (_parse_func) {
        uir_add_func_port(_parse_func, name, UIR_PORT_INOUT, width);
    } else if (_parse_unit) {
        if (is_bare_port_name(name)) {
            uint32_t msb = (width > 0) ? width - 1 : 0;
            uir_port_t *p = uir_add_port(_parse_unit, name, UIR_PORT_INOUT, msb, 0, UIR_SIG_WIRE);
            if (p) p->is_signed = _parse_decl_signed;
            mark_bare_port_handled(name);
        } else {
            uir_signal_t *s = uir_add_signal(_parse_unit, name, UIR_SIG_WIRE, width, array_size);
            if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
        }
    }
}

static void decl_reg(const char *name, uint32_t width, uint32_t array_size) {
    if (_parse_func) {
        uir_loc_t loc = {NULL, 0, 0};
        uir_signal_t *sig = (uir_signal_t *)uir_alloc_node(
            _parse_unit, UIR_SIGNAL, sizeof(uir_signal_t), loc);
        if (sig) {
            sig->name = parse_strdup(name);
            sig->sig_type = UIR_SIG_REG;
            sig->width = width;
            sig->array_size = array_size;
            sig->is_signed = _parse_decl_signed;
            set_signal_array_dims(sig);
            sig->init_value.state = QSIM_X;
            sig->init_value.strength = QSIM_STRENGTH_STRONG;
            uir_add_func_local(_parse_func, (uir_node_t *)sig);
        }
    } else if (_parse_unit) {
        uir_signal_t *s = uir_add_signal(_parse_unit, name, UIR_SIG_REG, width, array_size);
        if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
    }
}

static void decl_logic(const char *name, uint32_t width, uint32_t array_size) {
    if (_parse_func) {
        uir_loc_t loc = {NULL, 0, 0};
        uir_signal_t *sig = (uir_signal_t *)uir_alloc_node(
            _parse_unit, UIR_SIGNAL, sizeof(uir_signal_t), loc);
        if (sig) {
            sig->name = parse_strdup(name);
            sig->sig_type = UIR_SIG_LOGIC;
            sig->width = width;
            sig->array_size = array_size;
            sig->is_signed = _parse_decl_signed;
            set_signal_array_dims(sig);
            sig->init_value.state = QSIM_X;
            sig->init_value.strength = QSIM_STRENGTH_STRONG;
            uir_add_func_local(_parse_func, (uir_node_t *)sig);
        }
    } else if (_parse_unit) {
        uir_signal_t *s = uir_add_signal(_parse_unit, name, UIR_SIG_LOGIC, width, array_size);
        if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
    }
}

/* ── SystemVerilog modport helpers ── */

static void add_modport(const char *name) {
    if (!_parse_unit || !name) return;
    uir_modport_t *mp = uir_add_modport(_parse_unit, name);
    (void)mp;
}

static void add_modport_port(const char *name) {
    if (!_parse_unit || !name) return;
    if (_parse_unit->modport_count == 0) return;
    uir_modport_t *mp = &_parse_unit->modports[_parse_unit->modport_count - 1];
    uir_add_modport_port(mp, name, _parse_modport_dir);
}

/* Called from conn_item action for .port(modport.signal) */
static void do_conn_modport(char *formal, char *modport, char *signal) {
    if (!_parse_unit || _parse_unit->instance_count == 0) return;
    uir_instance_t *inst = _parse_unit->instances[_parse_unit->instance_count - 1];
    uir_node_t *ref = uir_make_ref(_parse_unit, signal, parse_loc());
    uir_add_connection(inst, formal, ref);
    if (inst->connection_count > 0)
        inst->connections[inst->connection_count - 1].modport_name = modport;
}

/* ── SystemVerilog package import helpers ── */

static void do_import(const char *pkg_name, const char *item_name, int is_wildcard) {
    if (!_parse_unit) return;
    uir_add_import(_parse_unit, pkg_name, is_wildcard ? NULL : item_name);
}

/* Forward declaration (defined after do_concat) */
static int parse_range_width(void);

/* Copy _parse_array_dims into a signal struct (called after uir_add_signal). */
static void set_signal_array_dims(uir_signal_t *s) {
    if (!s) return;
    for (int i = 0; i < _parse_array_dims_count && i < 4; i++)
        s->array_dims[i] = _parse_array_dims[i];
    s->array_dim_count = (size_t)_parse_array_dims_count;
}

/* PEG-action helpers that avoid nested {} (peg tool limitation). */
static void _parse_add_port_signed(const char *name, uir_port_dir_t dir,
                                    uint32_t msb, uint32_t lsb,
                                    uir_signal_type_t sig_type) {
    if (!_parse_unit) return;
    /* In function port-list mode, add to function instead of unit */
    if (_parse_func_ports_mode && _parse_func) {
        uint32_t width = (msb >= lsb) ? (msb - lsb + 1) : (lsb - msb + 1);
        uir_add_func_port(_parse_func, name, dir, width);
        return;
    }
    uir_port_t *p = uir_add_port(_parse_unit, name, dir, msb, lsb, sig_type);
    if (p) p->is_signed = _parse_decl_signed;
}

static void _parse_add_signal_signed(const char *name, uir_signal_type_t type,
                                      uint32_t width, uint32_t array_size) {
    if (!_parse_unit) return;
    uir_signal_t *s = uir_add_signal(_parse_unit, name, type, width, array_size);
    if (s) { s->is_signed = _parse_decl_signed; set_signal_array_dims(s); }
}

/* ── Function/task lifecycle helpers ── */

/* ── Function port-list helpers ── */

static void start_function(uir_design_unit_t *unit, const char *name);

static void do_func_port_enter(void) {
    if (_parse_unit && _parse_saved) {
        start_function(_parse_unit, _parse_saved);
        _parse_func_ports_mode = 1;
    }
}

static void do_func_port_leave(void) {
    _parse_func_ports_mode = 0;
}

static void func_port_cleanup(void) {
    _parse_decl_automatic = 0;
    free(_parse_saved);
    _parse_saved = NULL;
}

static void do_func_port_simple(void) {
    if (_parse_unit) start_function(_parse_unit, _parse_saved);
    _parse_decl_automatic = 0;
    free(_parse_saved);
    _parse_saved = NULL;
}

static void start_function(uir_design_unit_t *unit, const char *name) {
    if (!unit) return;
    _parse_func = uir_add_func_task(unit, name, 1);
    if (!_parse_func || !_parse_unit) return;
    _parse_func->return_width = parse_range_width();
    _parse_func->is_automatic = _parse_decl_automatic;
    _parse_func->body = (uir_node_t *)uir_add_block(_parse_unit, 1);
    push_stmt_block((uir_block_t *)_parse_func->body);
    decl_reg(name, _parse_func->return_width, 0);
}

static void finish_function(void) {
    if (_parse_func) { pop_stmt_block(); _parse_func = NULL; }
}

static void start_task(uir_design_unit_t *unit, const char *name) {
    if (!unit) return;
    _parse_func = uir_add_func_task(unit, name, 0);
    if (!_parse_func || !_parse_unit) return;
    _parse_func->is_automatic = _parse_decl_automatic;
    _parse_func->body = (uir_node_t *)uir_add_block(_parse_unit, 1);
    push_stmt_block((uir_block_t *)_parse_func->body);
}

static void finish_task(void) {
    if (_parse_func) { pop_stmt_block(); _parse_func = NULL; }
}

/* ── Function call / task enable builders ── */

static void do_func_call_expr(uir_design_unit_t *unit, const char *name) {
    if (!unit) return;
    int n = _expr_sp - _parse_func_call_sp;
    if (n < 0) n = 0;
    uir_node_t **args = NULL;
    size_t arg_count = 0;
    if (n > 0) {
        args = malloc((size_t)n * sizeof(uir_node_t *));
        if (!args) return;
        for (int i = 0; i < n; i++)
            args[arg_count++] = _expr_stack[_parse_func_call_sp + i];
        _expr_sp = _parse_func_call_sp;
    }
    uir_func_call_t *fc = uir_make_func_call(unit, name, args, arg_count, parse_loc());
    if (fc) {
        expr_push((uir_node_t *)fc);
    } else {
        free(args);
    }
}

static void do_task_enable_stmt(uir_design_unit_t *unit, const char *name) {
    if (!unit) return;
    int n = _expr_sp - _parse_func_call_sp;
    if (n < 0) n = 0;
    uir_node_t **args = NULL;
    size_t arg_count = 0;
    if (n > 0) {
        args = malloc((size_t)n * sizeof(uir_node_t *));
        if (!args) return;
        for (int i = 0; i < n; i++)
            args[arg_count++] = _expr_stack[_parse_func_call_sp + i];
        _expr_sp = _parse_func_call_sp;
    }
    uir_func_call_t *tc = uir_make_func_call(unit, name, args, arg_count, parse_loc());
    if (tc) {
        tc->base.kind = UIR_TASK_ENABLE;
        append_stmt((uir_node_t *)tc);
    } else {
        free(args);
    }
}

static void do_sys_display(uir_design_unit_t *unit, uir_sys_task_kind_t kind, const char *fmt) {
    if (!unit) return;
    int n = _expr_sp - _parse_func_call_sp;
    if (n < 0) n = 0;
    uir_node_t **args = NULL;
    size_t arg_count = 0;
    if (n > 0) {
        args = malloc((size_t)n * sizeof(uir_node_t *));
        if (!args) return;
        for (int i = 0; i < n; i++)
            args[arg_count++] = _expr_stack[_parse_func_call_sp + i];
        _expr_sp = _parse_func_call_sp;
    }
    uir_node_t *t = uir_make_sys_task(unit, kind, NULL, NULL, fmt, args, arg_count, parse_loc());
    if (t) append_stmt(t);
    else free(args);
}

/* Expression-level system function helpers */

static void do_sys_func_expr0(uir_design_unit_t *unit, uir_sys_func_kind_t kind) {
    /* 0-arg system function: $time, $realtime, $random() */
    if (!unit) return;
    uir_node_t *n = uir_make_sys_func_expr(unit, kind, NULL, 0, parse_loc());
    if (n) expr_push(n);
}

static void do_sys_func_expr1(uir_design_unit_t *unit, uir_sys_func_kind_t kind) {
    /* 1-arg system function: $signed, $unsigned, $clog2, $random(seed) */
    if (!unit) return;
    uir_node_t *arg = expr_pop();
    if (!arg) return;
    uir_node_t **args = malloc(sizeof(uir_node_t *));
    if (!args) return;
    args[0] = arg;
    uir_node_t *n = uir_make_sys_func_expr(unit, kind, args, 1, parse_loc());
    if (n) { expr_push(n); }
    else { free(args); }
}

static void do_sys_func_fopen_stub(uir_design_unit_t *unit) {
    /* $fopen("filename") stub: push a 32-bit 0 literal (invalid MCD) */
    if (!unit) return;
    qsim_bit_vector_t *zero = qsim_bit_vector_alloc(32);
    if (zero) {
        for (int i = 0; i < 32; i++)
            qsim_bit_set(zero, i, QSIM_VAL_0);
    }
    uir_literal_t *lit = uir_make_literal(unit, zero, parse_loc());
    if (lit) expr_push((uir_node_t *)lit);
}

static void do_sys_stop_finish_with_arg(uir_design_unit_t *unit, uir_sys_task_kind_t kind) {
    if (!unit) return;
    uir_node_t *a = expr_pop();
    uir_node_t **args = malloc(sizeof(uir_node_t *));
    if (!args) return;
    args[0] = a;
    append_stmt(uir_make_sys_task(unit, kind, NULL, NULL, NULL, args, 1, parse_loc()));
}

/* ── Loop statement helpers (called from grammar actions) ── */

/* Forward declaration (defined after do_nonblocking_assign) */
static uir_expr_t *make_literal_int(uir_design_unit_t *unit, uint64_t val, int width);

/* For-loop: save init stmt (last stmt in current block), remove it from block */
static void do_for_init_save(void) {
    uir_block_t *b = current_block();
    if (b && b->stmt_count > 0) {
        _parse_loop_init = b->stmts[b->stmt_count - 1];
        b->stmt_count--;
    }
}

/* For-loop: pop condition from expression stack */
static void do_for_cond_save(void) {
    _parse_loop_cond = expr_pop();
}

/* For-loop: save step stmt (last stmt in current block), remove it from block */
static void do_for_step_save(void) {
    uir_block_t *b = current_block();
    if (b && b->stmt_count > 0) {
        _parse_loop_step = b->stmts[b->stmt_count - 1];
        b->stmt_count--;
    }
}

/* For-loop: create and push body block */
static void do_for_body_enter(void) {
    if (!_parse_unit) return;
    _parse_loop_body = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_loop_body);
}

/* For-loop: pop body block, build UIR_LOOP, run restructure */
static void do_for_finish(void) {
    pop_stmt_block();
    if (!_parse_unit || !_parse_loop_cond || !_parse_loop_body) {
        _parse_loop_init = NULL; _parse_loop_cond = NULL;
        _parse_loop_step = NULL; _parse_loop_body = NULL;
        return;
    }
    /* Build augmented body: [body, step, LoopBack{cond, augmented_body}] */
    uir_block_t *aug = uir_add_block(_parse_unit, 1);
    if (!aug) return;
    for (size_t i = 0; i < _parse_loop_body->stmt_count; i++)
        append_to_block(aug, _parse_loop_body->stmts[i]);
    if (_parse_loop_step)
        append_to_block(aug, _parse_loop_step);
    uir_loop_back_t *lb = (uir_loop_back_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP_BACK, sizeof(uir_loop_back_t), parse_loc());
    if (lb) {
        lb->condition = _parse_loop_cond;
        lb->body = (uir_node_t *)aug;
    }
    append_to_block(aug, (uir_node_t *)lb);
    restructure_delays(aug);

    uir_loop_t *loop = (uir_loop_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP, sizeof(uir_loop_t), parse_loc());
    if (loop) {
        loop->init_stmt = _parse_loop_init;
        loop->condition = _parse_loop_cond;
        loop->step_stmt = _parse_loop_step;
        loop->body = (uir_node_t *)aug;
        append_stmt((uir_node_t *)loop);
    }
    _parse_loop_init = NULL; _parse_loop_cond = NULL;
    _parse_loop_step = NULL; _parse_loop_body = NULL;
}

/* While-loop: save condition from expression stack */
static void do_while_enter(void) {
    _parse_loop_cond = expr_pop();
}

/* While-loop: create and push body block */
static void do_while_body_enter(void) {
    if (!_parse_unit) return;
    _parse_loop_body = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_loop_body);
}

/* While-loop: pop body block, build UIR_LOOP */
static void do_while_finish(void) {
    pop_stmt_block();
    if (!_parse_unit || !_parse_loop_cond || !_parse_loop_body) {
        _parse_loop_cond = NULL; _parse_loop_body = NULL;
        return;
    }
    uir_block_t *aug = uir_add_block(_parse_unit, 1);
    if (!aug) return;
    for (size_t i = 0; i < _parse_loop_body->stmt_count; i++)
        append_to_block(aug, _parse_loop_body->stmts[i]);
    uir_loop_back_t *lb = (uir_loop_back_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP_BACK, sizeof(uir_loop_back_t), parse_loc());
    if (lb) {
        lb->condition = _parse_loop_cond;
        lb->body = (uir_node_t *)aug;
    }
    append_to_block(aug, (uir_node_t *)lb);
    restructure_delays(aug);

    uir_loop_t *loop = (uir_loop_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP, sizeof(uir_loop_t), parse_loc());
    if (loop) {
        loop->init_stmt = NULL;
        loop->condition = _parse_loop_cond;
        loop->step_stmt = NULL;
        loop->body = (uir_node_t *)aug;
        append_stmt((uir_node_t *)loop);
    }
    _parse_loop_cond = NULL; _parse_loop_body = NULL;
}

/* Repeat-loop: save count expression from stack */
static void do_repeat_enter(void) {
    _parse_loop_count = expr_pop();
}

/* Repeat-loop: create and push body block */
static void do_repeat_body_enter(void) {
    if (!_parse_unit) return;
    _parse_loop_body = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_loop_body);
}

/* Repeat-loop: pop body, build UIR_LOOP with counter variable */
static void do_repeat_finish(void) {
    pop_stmt_block();
    if (!_parse_unit || !_parse_loop_body) {
        _parse_loop_count = NULL; _parse_loop_body = NULL;
        return;
    }
    /* Create a unique counter variable */
    static unsigned int _repeat_id = 0;
    char cnt_name[64];
    snprintf(cnt_name, sizeof(cnt_name), "_repeat_cnt_%u", _repeat_id++);
    char *name = parse_strdup(cnt_name);
    uir_add_signal(_parse_unit, name, UIR_SIG_REG, 32, 0);

    /* Build init: counter = <count_expr> */
    uir_node_t *cnt_ref_init = uir_make_ref(_parse_unit, name, parse_loc());
    uir_assign_t *init_a = (uir_assign_t *)uir_alloc_node(
        _parse_unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
    if (init_a) {
        init_a->lhs = cnt_ref_init;
        init_a->rhs = _parse_loop_count;
        init_a->delay = 0;
    }

    /* Build step: counter = counter - 1 */
    uir_node_t *cnt_ref_step = uir_make_ref(_parse_unit, name, parse_loc());
    uir_node_t *one_lit = (uir_node_t *)make_literal_int(_parse_unit, 1, 32);
    uir_node_t *dec = (uir_node_t *)uir_make_binary(_parse_unit, UIR_OP_SUB, cnt_ref_step, one_lit, parse_loc());
    uir_assign_t *step_a = (uir_assign_t *)uir_alloc_node(
        _parse_unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
    if (step_a) {
        uir_node_t *cnt_ref_step2 = uir_make_ref(_parse_unit, name, parse_loc());
        step_a->lhs = cnt_ref_step2;
        step_a->rhs = dec;
        step_a->delay = 0;
    }

    /* Build condition: counter > 0 */
    uir_node_t *cnt_ref_cond = uir_make_ref(_parse_unit, name, parse_loc());
    uir_node_t *zero_lit = (uir_node_t *)make_literal_int(_parse_unit, 0, 32);
    uir_node_t *cond = (uir_node_t *)uir_make_binary(_parse_unit, UIR_OP_GT, cnt_ref_cond, zero_lit, parse_loc());

    /* Build augmented body: [body, step, LoopBack{cond, augmented}] */
    uir_block_t *aug = uir_add_block(_parse_unit, 1);
    if (!aug) {
        _parse_loop_count = NULL; _parse_loop_body = NULL;
        return;
    }
    for (size_t i = 0; i < _parse_loop_body->stmt_count; i++)
        append_to_block(aug, _parse_loop_body->stmts[i]);
    if (step_a)
        append_to_block(aug, (uir_node_t *)step_a);
    uir_loop_back_t *lb = (uir_loop_back_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP_BACK, sizeof(uir_loop_back_t), parse_loc());
    if (lb) {
        lb->condition = cond;
        lb->body = (uir_node_t *)aug;
    }
    append_to_block(aug, (uir_node_t *)lb);
    restructure_delays(aug);

    /* Create UIR_LOOP */
    uir_loop_t *loop = (uir_loop_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP, sizeof(uir_loop_t), parse_loc());
    if (loop) {
        loop->init_stmt = init_a ? (uir_node_t *)init_a : NULL;
        loop->condition = cond;
        loop->step_stmt = NULL;
        loop->body = (uir_node_t *)aug;
        append_stmt((uir_node_t *)loop);
    }
    _parse_loop_count = NULL; _parse_loop_body = NULL;
}

/* Forever-loop */
static void do_forever_enter(void) {
    if (!_parse_unit) return;
    _parse_loop_body = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_loop_body);
}

/* Forever-loop: pop body, build infinite-loop UIR_LOOP */
static void do_forever_finish(void) {
    pop_stmt_block();
    if (!_parse_unit || !_parse_loop_body) {
        _parse_loop_body = NULL;
        return;
    }
    uir_block_t *aug = uir_add_block(_parse_unit, 1);
    if (!aug) return;
    for (size_t i = 0; i < _parse_loop_body->stmt_count; i++)
        append_to_block(aug, _parse_loop_body->stmts[i]);
    /* Forever has no condition — NULL means infinite loop in LoopBack */
    uir_loop_back_t *lb = (uir_loop_back_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP_BACK, sizeof(uir_loop_back_t), parse_loc());
    if (lb) {
        lb->condition = NULL;  /* NULL = infinite loop */
        lb->body = (uir_node_t *)aug;
    }
    append_to_block(aug, (uir_node_t *)lb);
    restructure_delays(aug);

    uir_loop_t *loop = (uir_loop_t *)uir_alloc_node(
        _parse_unit, UIR_LOOP, sizeof(uir_loop_t), parse_loc());
    if (loop) {
        loop->init_stmt = NULL;
        loop->condition = NULL;  /* NULL = infinite loop */
        loop->step_stmt = NULL;
        loop->body = (uir_node_t *)aug;
        append_stmt((uir_node_t *)loop);
    }
    _parse_loop_body = NULL;
}

/* ── If-statement helpers (called from grammar actions) ── */

static void if_then_start(void) {
    if (_parse_if_depth < MAX_IF_NESTING) {
        _parse_saved_then[_parse_if_depth] = _parse_then_block;
        _parse_saved_else[_parse_if_depth] = _parse_else_block;
        _parse_saved_had_else[_parse_if_depth] = _parse_had_else;
        _parse_if_depth++;
    }
    _parse_then_block = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_then_block);
    _parse_had_else = 0;
}

static void if_else_start(void) {
    pop_stmt_block();
    _parse_else_block = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_else_block);
    _parse_had_else = 1;
}

static void if_finish(void) {
    pop_stmt_block();
    restructure_delays(_parse_then_block);
    if (_parse_had_else) restructure_delays(_parse_else_block);
    uir_node_t *cond = expr_pop();
    if (cond && _parse_unit) {
        uir_if_t *if_node = (uir_if_t *)uir_alloc_node(_parse_unit, UIR_IF, sizeof(uir_if_t), parse_loc());
        if_node->condition = cond;
        if_node->then_branch = (uir_node_t *)_parse_then_block;
        if_node->else_branch = _parse_had_else ? (uir_node_t *)_parse_else_block : NULL;
        append_stmt((uir_node_t *)if_node);
    }
    /* Restore outer if-statement state */
    if (_parse_if_depth > 0) {
        _parse_if_depth--;
        _parse_then_block = _parse_saved_then[_parse_if_depth];
        _parse_else_block = _parse_saved_else[_parse_if_depth];
        _parse_had_else = _parse_saved_had_else[_parse_if_depth];
    } else {
        _parse_had_else = 0;
    }
}

/* ── Case-statement helpers ── */

static void do_case_enter(uir_design_unit_t *unit) {
    _parse_case_saved_sp_stack[_parse_case_saved_sp_depth++] = _expr_sp;
    _parse_case_items = NULL;
    _parse_case_item_count = 0;
    _parse_case_item_cap = 0;
    _parse_case_default = NULL;
}

static void do_case_item_enter(uir_design_unit_t *unit) {
    int n = _expr_sp - _parse_case_saved_sp_stack[_parse_case_saved_sp_depth - 1];
    if (n <= 0) return;

    uir_node_t **patterns = malloc((size_t)n * sizeof(uir_node_t *));
    if (!patterns) return;
    for (int i = 0; i < n; i++)
        patterns[i] = _expr_stack[_parse_case_saved_sp_stack[_parse_case_saved_sp_depth - 1] + i];
    _expr_sp = _parse_case_saved_sp_stack[_parse_case_saved_sp_depth - 1];

    _parse_case_item_patterns = patterns;
    _parse_case_item_pattern_count = (size_t)n;

    if (unit) {
        _parse_case_item_body = uir_add_block(unit, 1);
        push_stmt_block(_parse_case_item_body);
    }
}

static void do_case_item_finish(uir_design_unit_t *unit) {
    pop_stmt_block();
    if (!unit || !_parse_case_item_body) return;

    uir_case_item_t *item = (uir_case_item_t *)uir_alloc_node(
        unit, UIR_CASE_ITEM, sizeof(uir_case_item_t), parse_loc());
    if (!item) return;
    item->patterns = _parse_case_item_patterns;
    item->pattern_count = _parse_case_item_pattern_count;
    item->body = (uir_node_t *)_parse_case_item_body;

    size_t n = _parse_case_item_count;
    if (n >= _parse_case_item_cap) {
        size_t new_cap = _parse_case_item_cap ? _parse_case_item_cap * 2 : 8;
        uir_case_item_t **new_items = realloc(
            _parse_case_items, new_cap * sizeof(uir_case_item_t *));
        if (!new_items) return;
        _parse_case_items = new_items;
        _parse_case_item_cap = new_cap;
    }
    _parse_case_items[n] = item;
    _parse_case_item_count++;

    _parse_case_item_patterns = NULL;
    _parse_case_item_pattern_count = 0;
    _parse_case_item_body = NULL;
}

static void do_case_default_enter(uir_design_unit_t *unit) {
    if (unit) {
        _parse_case_default = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_case_default);
    }
}

static void do_case_default_finish(uir_design_unit_t *unit) {
    pop_stmt_block();
    /* _parse_case_default is already set from enter */
}

static void do_case_finish(uir_design_unit_t *unit, int is_wildcard) {
    if (_parse_case_saved_sp_depth > 0) _parse_case_saved_sp_depth--;
    if (!unit) return;

    uir_node_t *case_expr = expr_pop();

    uir_case_t *case_node = (uir_case_t *)uir_alloc_node(
        unit, UIR_CASE, sizeof(uir_case_t), parse_loc());
    if (!case_node) return;
    case_node->expr = case_expr;
    case_node->items = _parse_case_items;
    case_node->item_count = _parse_case_item_count;
    case_node->default_item = _parse_case_default;
    case_node->is_wildcard = is_wildcard;

    /* Transfer ownership: prevent next case from realloc'ing this items array */
    _parse_case_items = NULL;
    _parse_case_item_count = 0;
    _parse_case_item_cap = 0;

    append_stmt((uir_node_t *)case_node);
}

/* Capture range msb/lsb and return width. Resets globals to 0. */
static int parse_range_width(void) {
    uint32_t msb = _parse_range_msb;
    uint32_t lsb = _parse_range_lsb;
    _parse_range_msb = 0;
    _parse_range_lsb = 0;
    return (msb >= lsb) ? (int)(msb - lsb + 1) : (int)(lsb - msb + 1);
}

/* Width inference for RHS expressions at parse time */
static uint32_t infer_node_width(uir_design_unit_t *unit, uir_node_t *node) {
    if (!node || !unit) return 1;
    switch (node->kind) {
        case UIR_LITERAL: {
            uir_literal_t *lit = (uir_literal_t *)node;
            return lit->width > 0 ? lit->width : 1;
        }
        case UIR_REF: {
            uir_ref_t *ref = (uir_ref_t *)node;
            uir_node_t *found = uir_find_signal(unit, ref->name);
            if (found) {
                if (found->kind == UIR_SIGNAL) return ((uir_signal_t *)found)->width;
                if (found->kind == UIR_PORT) return ((uir_port_t *)found)->width;
            }
            return 1;
        }
        case UIR_EXPR_BINARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            uint32_t wa = infer_node_width(unit, e->operand_a);
            uint32_t wb = infer_node_width(unit, e->operand_b);
            return wa > wb ? wa : wb;
        }
        case UIR_EXPR_UNARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            return infer_node_width(unit, e->operand_a);
        }
        default:
            return 1;
    }
}

/* ── Event name tracking (Phase 5a) ── */
#define MAX_EVENTS 64
static char *_parse_event_names[MAX_EVENTS];
static int   _parse_event_name_count = 0;

static int is_event_name(const char *name) {
    for (int i = 0; i < _parse_event_name_count; i++)
        if (strcmp(_parse_event_names[i], name) == 0) return 1;
    return 0;
}

static void decl_event(const char *name) {
    if (!_parse_unit || !name) return;
    if (_parse_event_name_count >= MAX_EVENTS) return;
    _parse_event_names[_parse_event_name_count++] = parse_strdup(name);
    uir_add_signal(_parse_unit, name, UIR_SIG_EVENT, 1, 0);
}

static void do_trigger_stmt(const char *name) {
    if (_parse_unit) {
        append_stmt((uir_node_t *)uir_make_event_trigger(_parse_unit, name, parse_loc()));
    }
}

/* ── Attribute helpers for (* name [= value] *) ── */

static void save_attr_name(const char *name) {
    if (_parse_attr_count >= MAX_ATTRS) return;
    _parse_attr_names[_parse_attr_count] = parse_strdup(name);
    _parse_attr_values[_parse_attr_count] = NULL;
    _parse_attr_count++;
}

static void save_attr_val(const char *val) {
    if (_parse_attr_count <= 0) return;
    free(_parse_attr_values[_parse_attr_count - 1]);
    _parse_attr_values[_parse_attr_count - 1] = parse_strdup(val);
}

static void finish_attr_list(void) {
    if (!_parse_unit || _parse_attr_count == 0) return;
    /* Append attributes to design unit's attribute list */
    for (int i = 0; i < _parse_attr_count; i++) {
        size_t n = _parse_unit->attr_count;
        uir_attr_t *na = realloc(_parse_unit->attrs, (n + 1) * sizeof(uir_attr_t));
        if (!na) break;
        _parse_unit->attrs = na;
        _parse_unit->attrs[n].name = _parse_attr_names[i];
        _parse_unit->attrs[n].value = _parse_attr_values[i];
        _parse_unit->attr_count++;
    }
    _parse_attr_count = 0;
}

/* ── Defparam helpers for defparam hier.path.name = value; ── */

static void save_defparam_start(const char *name) {
    free(_parse_defparam_path);
    _parse_defparam_path = parse_strdup(name);
}

static void save_defparam_extend(const char *name) {
    if (!_parse_defparam_path) return;
    size_t cur_len = strlen(_parse_defparam_path);
    size_t name_len = strlen(name);
    char *new_path = malloc(cur_len + 1 + name_len + 1);
    if (!new_path) return;
    memcpy(new_path, _parse_defparam_path, cur_len);
    new_path[cur_len] = '.';
    memcpy(new_path + cur_len + 1, name, name_len + 1);
    free(_parse_defparam_path);
    _parse_defparam_path = new_path;
}

static void save_defparam_value(void) {
    if (!_parse_unit || !_parse_defparam_path) {
        expr_pop(); /* discard value */
        return;
    }
    uir_node_t *val = expr_pop();
    if (!val) return;
    size_t n = _parse_unit->defparam_count;
    uir_defparam_t *nd = realloc(_parse_unit->defparams, (n + 1) * sizeof(uir_defparam_t));
    if (!nd) return;
    _parse_unit->defparams = nd;
    _parse_unit->defparams[n].hier_path = _parse_defparam_path;
    _parse_unit->defparams[n].value = val;
    _parse_unit->defparam_count++;
    _parse_defparam_path = NULL;
}

/* ── Module parameter save (parameter name = value) ── */

static void save_param_name(const char *name) {
    free(_parse_param_name);
    _parse_param_name = parse_strdup(name);
}

static void save_param_value(void) {
    if (!_parse_unit || !_parse_param_name) {
        expr_pop(); /* discard value */
        return;
    }
    uir_node_t *val = expr_pop();
    if (!val) { free(_parse_param_name); _parse_param_name = NULL; return; }
    size_t n = _parse_unit->param_count;
    uir_defparam_t *np = realloc(_parse_unit->params, (n + 1) * sizeof(uir_defparam_t));
    if (!np) { free(_parse_param_name); _parse_param_name = NULL; return; }
    _parse_unit->params = np;
    _parse_unit->params[n].hier_path = _parse_param_name;  /* owns the string */
    _parse_unit->params[n].value = val;
    _parse_unit->param_count++;
    _parse_param_name = NULL;
}

/* Create implicit wire if name is not declared. Infers width from context.
   Skips genvar names and parameter names — they are resolved at elaboration time, not signals. */
static void ensure_implicit_wire(uir_design_unit_t *unit, const char *name,
                                  uir_node_t *context_expr) {
    if (!unit || !name) return;
    if (uir_find_signal(unit, name)) return;
    if (is_genvar_name(name)) return;
    if (is_event_name(name)) return;
    if (is_param_name(unit, name)) return;
    uint32_t w = context_expr ? infer_node_width(unit, context_expr) : 1;
    uir_add_signal(unit, name, UIR_SIG_WIRE, w, 0);
}

/* Wrapper for uir_make_ref that creates implicit wire for undeclared names. */
static uir_node_t *make_ref_implicit(uir_design_unit_t *unit, const char *name,
                                      uir_loc_t loc) {
    ensure_implicit_wire(unit, name, NULL);
    return uir_make_ref(unit, name, loc);
}

static void do_binop(uir_design_unit_t *unit, uir_binary_op_t op) {
    uir_node_t *b = expr_pop();
    uir_node_t *a = expr_pop();
    if (a && b)
        expr_push((uir_node_t *)uir_make_binary(unit, op, a, b, parse_loc()));
}

static void do_unop(uir_design_unit_t *unit, uir_unary_op_t op) {
    uir_node_t *a = expr_pop();
    if (a)
        expr_push((uir_node_t *)uir_make_unary(unit, op, a, parse_loc()));
}

static void do_ternary(uir_design_unit_t *unit) {
    uir_node_t *c = expr_pop();  /* else_expr */
    uir_node_t *b = expr_pop();  /* then_expr */
    uir_node_t *a = expr_pop();  /* condition */
    if (a && b && c) {
        uir_cond_t *cond = (uir_cond_t *)uir_alloc_node(unit, UIR_COND, sizeof(uir_cond_t), parse_loc());
        if (cond) {
            cond->condition = a;
            cond->then_expr = b;
            cond->else_expr = c;
            expr_push((uir_node_t *)cond);
        }
    }
}

static void do_part_select(uir_design_unit_t *unit, const char *name,
                            uir_node_t *hi, uir_node_t *lo) {
    if (!unit || !name || !hi || !lo) return;
    uir_node_t *ref = uir_make_ref_part_select(unit, name, hi, lo, parse_loc());
    if (ref) expr_push(ref);
}

/* Indexed part-select: arr[base +: width] or arr[base -: width].
 * Converts to [hi:lo] form at parse time using expression trees.
 * +: hi = base + width - 1, lo = base
 * -: hi = base, lo = base - width + 1 */
static void do_indexed_part_select(uir_design_unit_t *unit, const char *name,
                                    uir_node_t *base, uir_node_t *width,
                                    int direction) {
    if (!unit || !name || !base || !width) return;
    uir_node_t *one = (uir_node_t *)make_literal_int(unit, 1, 32);
    uir_node_t *width_minus_1 = (uir_node_t *)uir_make_binary(
        unit, UIR_OP_SUB, width, one, parse_loc());
    uir_node_t *hi, *lo;
    if (direction > 0) {
        /* +: hi = base + width - 1, lo = base */
        hi = (uir_node_t *)uir_make_binary(unit, UIR_OP_ADD, base, width_minus_1, parse_loc());
        lo = base;
    } else {
        /* -: hi = base, lo = base - width + 1 */
        hi = base;
        lo = (uir_node_t *)uir_make_binary(unit, UIR_OP_SUB, base, width_minus_1, parse_loc());
    }
    uir_node_t *ref = uir_make_ref_part_select(unit, name, hi, lo, parse_loc());
    if (ref) expr_push(ref);
}

static void do_multi_index_ref(uir_design_unit_t *unit) {
    if (!unit || !_parse_multi_id || _parse_multi_index_count == 0) return;
    /* Use uir_make_ref_index for single index, multi_index array for 2+ */
    uir_ref_t *ref = (uir_ref_t *)uir_make_ref(unit, _parse_multi_id, parse_loc());
    if (!ref) return;
    ref->multi_index = calloc((size_t)_parse_multi_index_count, sizeof(uir_node_t *));
    if (!ref->multi_index) return;
    for (int i = 0; i < _parse_multi_index_count; i++)
        ref->multi_index[i] = _parse_multi_indices[i];
    ref->multi_idx_count = (size_t)_parse_multi_index_count;
    expr_push((uir_node_t *)ref);
}

/* Evaluate a constant UIR expression to a uint64_t (for generate conditions).
 * Returns 0 on failure (X/unrecognized node). */
static uint64_t eval_const_u64(uir_node_t *node) {
    if (!node) return 0;
    switch (node->kind) {
        case UIR_LITERAL: {
            uir_literal_t *l = (uir_literal_t *)node;
            uint64_t val = 0;
            for (uint32_t i = 0; i < l->value->width && i < 64; i++)
                if (qsim_bit_get(l->value, i).state == QSIM_1) val |= (1ULL << i);
            return val;
        }
        case UIR_EXPR_BINARY: {
            uir_expr_t *e = (uir_expr_t *)node;
            uint64_t a = eval_const_u64(e->operand_a);
            uint64_t b = eval_const_u64(e->operand_b);
            switch (e->op.bin_op) {
                case UIR_OP_ADD: return a + b;
                case UIR_OP_SUB: return a - b;
                case UIR_OP_MUL: return a * b;
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
        default: return 0;
    }
}

/* Helper for generate if (cond): evaluate condition, suppress body if false */
static void generate_if_enter(void) {
    uir_node_t *node = expr_pop();

    if (_parse_current_gen != NULL) {
        /* Inside generate-for body: create UIR_GEN_IF for elaboration-time eval.
         * The condition (which may reference genvars) is stored for later evaluation. */
        uir_loc_t loc = {NULL, 0, 0};
        uir_design_unit_t *temp = uir_create_design_unit("_gen_if_body_", "verilog", loc);

        _parse_gen_if_node = uir_add_generate(_parse_unit, UIR_GEN_IF);
        _parse_gen_if_node->if_condition = node;
        _parse_gen_if_node->body_template = temp;

        _parse_gen_if_outer = _parse_unit;
        _parse_unit = temp;
        _parse_gen_if_nested = 1;
        _parse_gen_if_branch = 1;

        /* Push gen_label so inner gen_block doesn't corrupt outer label */
        _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
        _parse_gen_label = NULL;
        return;
    }

    /* Original parse-time evaluation for top-level generate-if */
    uint64_t val = eval_const_u64(node);
    _parse_generate_active = (val != 0) ? 1 : 0;
    if (!_parse_generate_active) {
        _parse_saved_unit = _parse_unit;
        _parse_unit = NULL;
    }
    _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
    _parse_gen_label = NULL;
}
/* Helper: restore _parse_unit after a conditional generate block */
static void generate_if_leave(void) {
    if (_parse_gen_if_nested) {
        uir_design_unit_t *temp = _parse_unit;
        if (temp && _parse_gen_if_node && _parse_gen_if_branch == 1) {
            for (size_t i = 0; i < temp->signal_count; i++)
                uir_add_generate_body_item(_parse_gen_if_node, (uir_node_t *)temp->signals[i]);
            for (size_t i = 0; i < temp->process_count; i++)
                uir_add_generate_body_item(_parse_gen_if_node, (uir_node_t *)temp->processes[i]);
            for (size_t i = 0; i < temp->instance_count; i++)
                uir_add_generate_body_item(_parse_gen_if_node, (uir_node_t *)temp->instances[i]);
            for (size_t i = 0; i < temp->assign_count; i++)
                uir_add_generate_body_item(_parse_gen_if_node, (uir_node_t *)temp->assigns[i]);
            for (size_t i = 0; i < temp->generate_count; i++)
                uir_add_generate_body_item(_parse_gen_if_node, (uir_node_t *)temp->generates[i]);
        }
        _parse_unit = _parse_gen_if_outer;
        _parse_gen_if_outer = NULL;
        free(_parse_gen_label);
        _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];

        if (_parse_gen_if_branch == 2) {
            /* else_body_template stays intact for elaboration-time expansion */
            _parse_gen_if_nested = 0;
            _parse_gen_if_node = NULL;
            _parse_gen_if_branch = 0;
        }
        return;
    }
    if (!_parse_unit) {
        _parse_unit = _parse_saved_unit;
        _parse_saved_unit = NULL;
    }
    free(_parse_gen_label);
    _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];
}

/* Helper: save genvar identifier during generate for header */
static void save_genvar_id(const char *name) {
    free(_parse_genvar_id);
    _parse_genvar_id = parse_strdup(name);
    add_genvar_name(name);
}

/* Helper: enter generate for loop.
 * Pops init, condition, step from expression stack; creates temp unit for body.
 */
static void generate_for_enter(void) {
    /* Pop stack: step_rhs (top), condition, init (bottom).
     * The for header "ID ASSIGN expr SEMI expr SEMI ID ASSIGN expr" pushes
     * init, then condition, then step_rhs — pop in reverse order. */
    uir_node_t *step_rhs = expr_pop();
    uir_node_t *condition = expr_pop();
    uir_node_t *init = expr_pop();

    if (!_parse_unit) return;

    /* Create the generate node in the parent unit */
    _parse_current_gen = uir_add_generate(_parse_unit, UIR_GEN_LOOP);
    _parse_current_gen->genvar_name = _parse_genvar_id ? strdup(_parse_genvar_id) : NULL;
    _parse_current_gen->for_init = init;
    _parse_current_gen->for_cond = condition;
    _parse_current_gen->for_step = step_rhs;
    _parse_current_gen->label = NULL; /* set later by gen_block label action */

    /* Create temp unit to hold body items (redirects _parse_unit) */
    uir_loc_t loc = {NULL, 0, 0};
    uir_design_unit_t *temp = uir_create_design_unit("_gen_body_", "verilog", loc);
    _parse_current_gen->body_template = temp;
    _parse_saved_unit = _parse_unit;
    _parse_unit = temp;

    /* Push gen_label — gen_block after this will set it; leave restores */
    _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
    _parse_gen_label = NULL;
}

/* Helper: leave generate for loop.
 * Transfers body items from temp unit to generate node, restores _parse_unit.
 */
static void generate_for_leave(void) {
    if (!_parse_current_gen || !_parse_saved_unit) return;

    /* Transfer body items from temp unit to generate node */
    uir_design_unit_t *temp = _parse_current_gen->body_template;
    if (temp) {
        for (size_t i = 0; i < temp->instance_count; i++)
            uir_add_generate_body_item(_parse_current_gen, (uir_node_t *)temp->instances[i]);
        for (size_t i = 0; i < temp->signal_count; i++)
            uir_add_generate_body_item(_parse_current_gen, (uir_node_t *)temp->signals[i]);
        for (size_t i = 0; i < temp->process_count; i++)
            uir_add_generate_body_item(_parse_current_gen, (uir_node_t *)temp->processes[i]);
        for (size_t i = 0; i < temp->assign_count; i++)
            uir_add_generate_body_item(_parse_current_gen, (uir_node_t *)temp->assigns[i]);
        for (size_t i = 0; i < temp->generate_count; i++)
            uir_add_generate_body_item(_parse_current_gen, (uir_node_t *)temp->generates[i]);
    }

    /* Set generate label from what gen_block saved */
    if (_parse_gen_label && _parse_current_gen)
        _parse_current_gen->label = strdup(_parse_gen_label);

    /* Restore parent unit */
    _parse_unit = _parse_saved_unit;
    _parse_saved_unit = NULL;

    /* Cleanup genvar state */
    free(_parse_genvar_id);
    _parse_genvar_id = NULL;
    /* Pop outer gen_label (free our label after using it above) */
    free(_parse_gen_label);
    _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];
    _parse_current_gen = NULL;
}
/* Helper for generate else: suppress body if the if-then was taken */
static void generate_else_enter(void) {
    if (_parse_gen_if_nested) {
        uir_loc_t loc = {NULL, 0, 0};
        uir_design_unit_t *temp = uir_create_design_unit("_gen_else_body_", "verilog", loc);
        _parse_gen_if_node->else_body_template = temp;
        _parse_gen_if_outer = _parse_unit;
        _parse_unit = temp;
        _parse_gen_if_branch = 2;
        _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
        _parse_gen_label = NULL;
        return;
    }
    if (_parse_generate_active) {
        _parse_saved_unit = _parse_unit;
        _parse_unit = NULL;
    }
    /* Push gen_label so else's gen_block doesn't corrupt outer label */
    _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
    _parse_gen_label = NULL;
}

/* Generate case: evaluate case expression at parse time, emit only matching item */
static void generate_case_enter(void) {
    uir_node_t *node = expr_pop();

    if (_parse_current_gen != NULL) {
        /* Template mode (inside generate-for body): create UIR_GEN_CASE
         * for elaboration-time evaluation. */
        uir_loc_t loc = {NULL, 0, 0};
        _parse_gen_case_node = uir_add_generate(_parse_unit, UIR_GEN_CASE);
        if (!_parse_gen_case_node) return;
        _parse_gen_case_node->case_expr = node;
        _parse_gen_case_active = 1;
        _parse_gen_case_outer = _parse_unit;
        _parse_gen_case_default_mode = 0;
        _parse_gen_case_cur_temp = NULL;
        _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
        _parse_gen_label = NULL;
        return;
    }

    /* Parse-time evaluation mode for top-level generate-case */
    _parse_gen_case_val = eval_const_u64(node);
    _parse_gen_case_matched = 0;
    _parse_gen_case_saved_sp = _expr_sp;
}

static void generate_case_item_enter(void) {
    if (_parse_gen_case_active) {
        /* Template mode: save patterns, create temp unit for body */
        int n = _expr_sp - _parse_gen_case_saved_sp;
        if (n > 0 && _parse_gen_case_node) {
            size_t idx = _parse_gen_case_node->case_item_count;
            /* Grow case_item_templates */
            uir_design_unit_t **nt = realloc(_parse_gen_case_node->case_item_templates,
                (idx + 1) * sizeof(uir_design_unit_t *));
            uir_node_t ***np = realloc(_parse_gen_case_node->case_item_patterns,
                (idx + 1) * sizeof(uir_node_t **));
            size_t *npc = realloc(_parse_gen_case_node->case_item_pattern_counts,
                (idx + 1) * sizeof(size_t));
            if (!nt || !np || !npc) { _expr_sp = _parse_gen_case_saved_sp; return; }
            _parse_gen_case_node->case_item_templates = nt;
            _parse_gen_case_node->case_item_patterns = np;
            _parse_gen_case_node->case_item_pattern_counts = npc;
            _parse_gen_case_node->case_item_templates[idx] = NULL;
            _parse_gen_case_node->case_item_patterns[idx] = calloc((size_t)n, sizeof(uir_node_t *));
            _parse_gen_case_node->case_item_pattern_counts[idx] = (size_t)n;
            for (int i = 0; i < n; i++)
                _parse_gen_case_node->case_item_patterns[idx][i] =
                    _expr_stack[_parse_gen_case_saved_sp + i];
            _parse_gen_case_node->case_item_count++;
        }
        _expr_sp = _parse_gen_case_saved_sp;

        /* Create temp unit for this case item's body */
        uir_loc_t loc = {NULL, 0, 0};
        _parse_gen_case_cur_temp = uir_create_design_unit("_gen_case_item_", "verilog", loc);
        _parse_gen_case_outer = _parse_unit;
        _parse_unit = _parse_gen_case_cur_temp;
        _parse_gen_case_default_mode = 0;
        _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
        _parse_gen_label = NULL;
        return;
    }

    /* Parse-time evaluation mode */
    int n = _expr_sp - _parse_gen_case_saved_sp;
    int matched = 0;
    for (int i = 0; i < n; i++) {
        uir_node_t *p = _expr_stack[_parse_gen_case_saved_sp + i];
        uint64_t pv = eval_const_u64(p);
        if (pv == _parse_gen_case_val) { matched = 1; break; }
    }
    _expr_sp = _parse_gen_case_saved_sp;

    if (matched && !_parse_gen_case_matched) {
        _parse_gen_case_matched = 1;
    } else {
        _parse_saved_unit = _parse_unit;
        _parse_unit = NULL;
    }
    /* Push gen_label so case item's gen_block doesn't corrupt outer label */
    _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
    _parse_gen_label = NULL;
}

static void generate_case_item_leave(void) {
    if (_parse_gen_case_active && _parse_gen_case_node) {
        if (_parse_gen_case_default_mode) {
            /* Store default template and restore outer unit */
            _parse_gen_case_node->case_default_template = _parse_gen_case_cur_temp;
            _parse_gen_case_cur_temp = NULL;
            _parse_unit = _parse_gen_case_outer;
            _parse_gen_case_outer = NULL;
        } else if (_parse_gen_case_cur_temp) {
            /* Store current temp unit in the case item templates array */
            size_t idx = _parse_gen_case_node->case_item_count - 1;
            _parse_gen_case_node->case_item_templates[idx] = _parse_gen_case_cur_temp;
            _parse_gen_case_cur_temp = NULL;
            _parse_unit = _parse_gen_case_outer;
            _parse_gen_case_outer = NULL;
        }
        free(_parse_gen_label);
        _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];
        return;
    }

    if (!_parse_unit) {
        _parse_unit = _parse_saved_unit;
        _parse_saved_unit = NULL;
    }
    /* Pop outer gen_label */
    free(_parse_gen_label);
    _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];
}

static void generate_case_default_enter(void) {
    if (_parse_gen_case_active && _parse_gen_case_node) {
        /* Template mode: create temp unit for default body */
        uir_loc_t loc = {NULL, 0, 0};
        _parse_gen_case_cur_temp = uir_create_design_unit("_gen_case_default_", "verilog", loc);
        _parse_gen_case_outer = _parse_unit;
        _parse_unit = _parse_gen_case_cur_temp;
        _parse_gen_case_default_mode = 1;
        _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
        _parse_gen_label = NULL;
        return;
    }

    if (_parse_gen_case_matched) {
        _parse_saved_unit = _parse_unit;
        _parse_unit = NULL;
    }
    /* Push gen_label so default item's gen_block doesn't corrupt outer label */
    _parse_gen_label_stack[_parse_gen_label_sp++] = _parse_gen_label;
    _parse_gen_label = NULL;
}

static void generate_case_leave(void) {
    if (_parse_gen_case_active && _parse_gen_case_node) {
        /* Template mode: finalize. The UIR_GEN_CASE node is already on the unit.
         * Restore _parse_unit in case cleanup failed to restore it. */
        if (_parse_unit != _parse_gen_case_outer && _parse_gen_case_outer) {
            _parse_unit = _parse_gen_case_outer;
        }
        /* Restore gen_label saved by generate_case_enter for template mode.
         * Without this, a generate-for's label pushed before the case would be
         * lost, causing expand_generate_for to see a NULL label. */
        free(_parse_gen_label);
        _parse_gen_label = _parse_gen_label_stack[--_parse_gen_label_sp];
        _parse_gen_case_node = NULL;
        _parse_gen_case_active = 0;
        _parse_gen_case_outer = NULL;
        _parse_gen_case_cur_temp = NULL;
        _parse_gen_case_default_mode = 0;
        return;
    }

    if (!_parse_unit) {
        _parse_unit = _parse_saved_unit;
        _parse_saved_unit = NULL;
    }
    _parse_gen_case_matched = 0;
}

static void do_concat(uir_design_unit_t *unit) {
    /* Collect items from stack into a chain of UIR_OP_CONCAT.
     * Only pop items above the marker saved before expr_list, so that
     * surrounding expression state (LHS refs, ternary conditions) is preserved. */
    uir_node_t *items[EXPR_STACK_MAX];
    int n = 0;
    while (_expr_sp > _parse_concat_sp) {
        items[n++] = expr_pop();
    }
    if (n == 0) return;
    /* Build left-to-right concat chain: items[n-1] is leftmost, items[0] is rightmost */
    uir_node_t *result = items[n - 1];
    for (int i = n - 2; i >= 0; i--)
        result = (uir_node_t *)uir_make_binary(unit, UIR_OP_CONCAT, items[i], result, parse_loc());
    expr_push(result);
}

/* Forward declaration for use by do_replicate */
static uir_expr_t *make_literal_int(uir_design_unit_t *unit, uint64_t val, int width);

static void do_repl_concat(uir_design_unit_t *unit) {
    if (_repl_sp_ptr < 0) return;
    int marker = _repl_sp_stack[_repl_sp_ptr--];
    uir_node_t *items[EXPR_STACK_MAX];
    int n = 0;
    while (_expr_sp > marker) items[n++] = expr_pop();
    if (n == 0) return;
    uir_node_t *result = items[n - 1];
    for (int i = n - 2; i >= 0; i--)
        result = (uir_node_t *)uir_make_binary(unit, UIR_OP_CONCAT, items[i], result, parse_loc());
    expr_push(result);
}

static void do_replicate(uir_design_unit_t *unit) {
    uir_node_t *expr = expr_pop();
    if (!expr) return;
    uir_expr_t *count_lit = make_literal_int(unit, _parse_repl_count, 32);
    if (!count_lit) return;
    uir_expr_t *result = uir_make_binary(unit, UIR_OP_REPLICATE, expr,
                                          (uir_node_t *)count_lit, parse_loc());
    if (result) expr_push((uir_node_t *)result);
}

static void do_continuous_assign(uir_design_unit_t *unit) {
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs = expr_pop();
    if (lhs && rhs) {
        uir_assign_t *a = uir_add_assign(unit);
        a->lhs = lhs;
        a->rhs = rhs;
    }
}

static void do_continuous_assign2(uir_design_unit_t *unit, const char *lhs_name,
                                    uir_node_t *lhs_index, uir_node_t *lhs_hi,
                                    uir_node_t *lhs_lo) {
    uir_node_t *rhs = expr_pop();
    ensure_implicit_wire(unit, lhs_name, rhs);
    uir_node_t *lhs = NULL;
    if (lhs_hi && lhs_lo)
        lhs = uir_make_ref_part_select(unit, lhs_name, lhs_hi, lhs_lo, parse_loc());
    else if (lhs_index)
        lhs = uir_make_ref_index(unit, lhs_name, lhs_index, parse_loc());
    else
        lhs = uir_make_ref(unit, lhs_name, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = uir_add_assign(unit);
        a->lhs = lhs;
        a->rhs = rhs;
        if (_parse_assign_delay) {
            a->delay_value = _parse_assign_delay;
            _parse_assign_delay = NULL;
        }
    }
}

static void do_blocking_assign(uir_design_unit_t *unit, const char *lhs_name,
                                uir_node_t *lhs_index, uir_node_t *lhs_hi,
                                uir_node_t *lhs_lo) {
    uir_node_t *rhs = expr_pop();
    ensure_implicit_wire(unit, lhs_name, rhs);
    uir_node_t *lhs = NULL;
    if (lhs_hi && lhs_lo)
        lhs = uir_make_ref_part_select(unit, lhs_name, lhs_hi, lhs_lo, parse_loc());
    else if (lhs_index)
        lhs = uir_make_ref_index(unit, lhs_name, lhs_index, parse_loc());
    else
        lhs = uir_make_ref(unit, lhs_name, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 0; /* blocking = immediate */
            append_stmt((uir_node_t *)a);
        }
    }
}

static void do_blocking_assign_multi(uir_design_unit_t *unit, const char *lhs_name,
                                      uir_node_t **multi_idx, int multi_count) {
    uir_node_t *rhs = expr_pop();
    ensure_implicit_wire(unit, lhs_name, rhs);
    uir_node_t *lhs = uir_make_ref(unit, lhs_name, parse_loc());
    if (lhs && multi_count > 0) {
        uir_ref_t *ref = (uir_ref_t *)lhs;
        ref->multi_index = calloc((size_t)multi_count, sizeof(uir_node_t *));
        if (ref->multi_index) {
            for (int i = 0; i < multi_count; i++)
                ref->multi_index[i] = multi_idx[i];
        }
        ref->multi_idx_count = (size_t)multi_count;
    }
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 0;
            append_stmt((uir_node_t *)a);
        }
    }
}

static void do_nonblocking_assign(uir_design_unit_t *unit, const char *lhs_name,
                                   uir_node_t *lhs_index, uir_node_t *lhs_hi,
                                   uir_node_t *lhs_lo) {
    uir_node_t *rhs = expr_pop();
    ensure_implicit_wire(unit, lhs_name, rhs);
    uir_node_t *lhs = NULL;
    if (lhs_hi && lhs_lo)
        lhs = uir_make_ref_part_select(unit, lhs_name, lhs_hi, lhs_lo, parse_loc());
    else if (lhs_index)
        lhs = uir_make_ref_index(unit, lhs_name, lhs_index, parse_loc());
    else
        lhs = uir_make_ref(unit, lhs_name, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 1; /* non-blocking = NBA */
            append_stmt((uir_node_t *)a);
        }
    }
}

static void do_nonblocking_assign_multi(uir_design_unit_t *unit, const char *lhs_name,
                                         uir_node_t **multi_idx, int multi_count) {
    uir_node_t *rhs = expr_pop();
    ensure_implicit_wire(unit, lhs_name, rhs);
    uir_node_t *lhs = uir_make_ref(unit, lhs_name, parse_loc());
    if (lhs && multi_count > 0) {
        uir_ref_t *ref = (uir_ref_t *)lhs;
        ref->multi_index = calloc((size_t)multi_count, sizeof(uir_node_t *));
        if (ref->multi_index) {
            for (int i = 0; i < multi_count; i++)
                ref->multi_index[i] = multi_idx[i];
        }
        ref->multi_idx_count = (size_t)multi_count;
    }
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 1;
            append_stmt((uir_node_t *)a);
        }
    }
}

/* ================================================================
 * Concatenation LHS helpers:  assign {a, b} = expr
 * ================================================================ */

static void store_concat_name(const char *name) {
    if (_parse_concat_count < 16)
        _parse_concat_names[_parse_concat_count++] = parse_strdup(name);
}

static uir_expr_t *make_literal_int(uir_design_unit_t *unit, uint64_t val, int width) {
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;
    for (int i = 0; i < width && i < 64; i++)
        qsim_bit_set(bv, i, ((val >> i) & 1) ? QSIM_VAL_1 : QSIM_VAL_0);
    return (uir_expr_t *)uir_make_literal(unit, bv, parse_loc());
}

static void do_concat_lhs_assign(uir_design_unit_t *unit, int continuous, int delay) {
    uir_node_t *rhs = expr_pop();

    if (unit && rhs && _parse_concat_count > 0) {
        /* Compute total RHS width by summing element widths */
        int total_width = 0;
        for (int i = 0; i < _parse_concat_count; i++) {
            uir_node_t *node = uir_find_signal(unit, _parse_concat_names[i]);
            if (node) total_width += (int)((uir_signal_t *)node)->width;
        }

        /* Process right-to-left: rightmost element gets LSBs */
        int bit_offset = 0;
        for (int i = _parse_concat_count - 1; i >= 0; i--) {
            const char *name = _parse_concat_names[i];
            uir_node_t *node = uir_find_signal(unit, name);
            if (!node) continue;
            uir_signal_t *sig = (uir_signal_t *)node;
            int w = (int)sig->width;

            uir_node_t *lhs = uir_make_ref(unit, name, parse_loc());
            uir_node_t *element_rhs;

            if (bit_offset == 0 && w >= total_width) {
                element_rhs = rhs;
            } else {
                /* element = (rhs >> bit_offset) & ((1 << w) - 1) */
                uir_node_t *shift_amt = (uir_node_t *)make_literal_int(unit, bit_offset, 32);
                uir_node_t *shifted = (uir_node_t *)uir_make_binary(unit, UIR_OP_SHR, rhs, shift_amt, parse_loc());

                uint64_t mask_val = (w >= 64) ? ~0ULL : ((1ULL << w) - 1);
                uir_node_t *mask = (uir_node_t *)make_literal_int(unit, mask_val, 32);
                element_rhs = (uir_node_t *)uir_make_binary(unit, UIR_OP_AND, shifted, mask, parse_loc());
            }

            if (continuous) {
                uir_assign_t *a = uir_add_assign(unit);
                if (a) {
                    a->lhs = lhs; a->rhs = element_rhs;
                    if (_parse_assign_delay) {
                        a->delay_value = _parse_assign_delay;
                        _parse_assign_delay = NULL;
                    }
                }
            } else {
                uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
                if (a) {
                    a->lhs = lhs;
                    a->rhs = element_rhs;
                    a->delay = delay;
                    append_stmt((uir_node_t *)a);
                }
            }

            bit_offset += w;
        }
    } else if (rhs) {
        /* No unit — consume the popped expression to avoid leaking */
    }

    /* Always clean up concat state */
    for (int i = 0; i < _parse_concat_count; i++)
        free(_parse_concat_names[i]);
    _parse_concat_count = 0;
}

/* Wrappers for grammar actions that avoid inner braces (PEG limitation) */

static void do_assign_stmt_finish(uir_design_unit_t *unit) {
    if (_parse_concat_count > 0) {
        if (unit) do_concat_lhs_assign(unit, 1, 0);
    } else {
        if (unit) do_continuous_assign2(unit, _parse_assign_lhs, _parse_lhs_index, _parse_lhs_hi, _parse_lhs_lo);
        free(_parse_assign_lhs); _parse_assign_lhs = NULL;
        _parse_lhs_index = NULL; _parse_lhs_hi = NULL; _parse_lhs_lo = NULL;
    }
}

static void do_blocking_assign_finish(uir_design_unit_t *unit) {
    if (_parse_concat_count > 0) {
        if (unit) do_concat_lhs_assign(unit, 0, 0);
    } else if (_parse_multi_index_count > 0) {
        if (unit) do_blocking_assign_multi(unit, _parse_assign_lhs,
                                            _parse_multi_indices, _parse_multi_index_count);
        free(_parse_assign_lhs); _parse_assign_lhs = NULL;
        _parse_lhs_index = NULL; _parse_lhs_hi = NULL; _parse_lhs_lo = NULL;
        _parse_multi_index_count = 0;
    } else {
        if (unit) do_blocking_assign(unit, _parse_assign_lhs, _parse_lhs_index, _parse_lhs_hi, _parse_lhs_lo);
        free(_parse_assign_lhs); _parse_assign_lhs = NULL;
        _parse_lhs_index = NULL; _parse_lhs_hi = NULL; _parse_lhs_lo = NULL;
    }
}

static void do_nonblocking_assign_finish(uir_design_unit_t *unit) {
    if (_parse_concat_count > 0) {
        if (unit) do_concat_lhs_assign(unit, 0, 1);
    } else if (_parse_multi_index_count > 0) {
        if (unit) do_nonblocking_assign_multi(unit, _parse_assign_lhs,
                                               _parse_multi_indices, _parse_multi_index_count);
        free(_parse_assign_lhs); _parse_assign_lhs = NULL;
        _parse_lhs_index = NULL; _parse_lhs_hi = NULL; _parse_lhs_lo = NULL;
        _parse_multi_index_count = 0;
    } else {
        if (unit) do_nonblocking_assign(unit, _parse_assign_lhs, _parse_lhs_index, _parse_lhs_hi, _parse_lhs_lo);
        free(_parse_assign_lhs); _parse_assign_lhs = NULL;
        _parse_lhs_index = NULL; _parse_lhs_hi = NULL; _parse_lhs_lo = NULL;
    }
}

/* Parse number text and create a literal node.
 * Handles plain decimal, sized based (b/o/d/h), and X/Z/? bits for casez. */
static uir_node_t *parse_number_literal(uir_design_unit_t *unit, const char *text, uir_loc_t loc) {
    unsigned long val = 0;
    int base = 10;
    int width = 0;
    int has_xz = 0;
    const char *digits = NULL;

    const char *p = text;
    const char *wstart = p;
    while (*p && *p != '\'') p++;
    if (*p == '\'') {
        /* Parse explicit width from digits before ' (e.g., "3'b011" → width=3) */
        const char *wp = wstart;
        while (wp < p) {
            width = width * 10 + (*wp - '0');
            wp++;
        }
        p++;
        if (*p == 's' || *p == 'S') p++;
        if (*p == 'h' || *p == 'H') { base = 16; p++; }
        else if (*p == 'd' || *p == 'D') { base = 10; p++; }
        else if (*p == 'o' || *p == 'O') { base = 8; p++; }
        else if (*p == 'b' || *p == 'B') { base = 2; p++; }
        digits = p;
        /* Check for X/Z/? characters */
        for (const char *q = p; *q; q++) {
            if (*q == '?' || *q == 'x' || *q == 'X' || *q == 'z' || *q == 'Z')
                has_xz = 1;
        }
        if (!has_xz) {
            while (*p) {
                char c = *p;
                if (c >= '0' && c <= '9') val = val * (unsigned long)base + (unsigned long)(c - '0');
                else if (c >= 'a' && c <= 'f') val = val * (unsigned long)base + (unsigned long)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') val = val * (unsigned long)base + (unsigned long)(c - 'A' + 10);
                else if (c == '_') { p++; continue; }
                p++;
            }
        }
    } else {
        val = strtoul(text, NULL, 10);
    }

    if (width == 0) width = 32;
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;

    if (base != 10 && has_xz && digits) {
        /* Build bit vector directly from the digit string for binary base */
        /* Count total digit bits needed */
        int bit_pos = 0;
        const char *q = digits;
        /* Find end of digit string */
        while (*q) q++;
        q--; /* start from rightmost digit */
        /* Simple per-character bit assignment (works for binary) */
        if (base == 2) {
            for (int bit = 0; q >= digits && bit < (int)width; bit++, q--) {
                char c = *q;
                if (c == '_') { bit--; continue; }
                if (c == '0') qsim_bit_set(bv, bit, QSIM_VAL_0);
                else if (c == '1') qsim_bit_set(bv, bit, QSIM_VAL_1);
                else if (c == '?' || c == 'z' || c == 'Z') qsim_bit_set(bv, bit, QSIM_VAL_Z);
                else qsim_bit_set(bv, bit, QSIM_VAL_X);
            }
        } else if (base == 16) {
            /* hex: each digit = 4 bits, right to left */
            for (int nib = 0; q >= digits && nib * 4 < (int)width; q--, nib++) {
                if (*q == '_') { nib--; continue; }
                int hval = 0;
                int hx = 0;
                char c = *q;
                if (c >= '0' && c <= '9') hval = c - '0';
                else if (c >= 'a' && c <= 'f') hval = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') hval = c - 'A' + 10;
                else hx = 1; /* X/Z/? */
                for (int b = 0; b < 4 && nib * 4 + b < (int)width; b++) {
                    if (hx) qsim_bit_set(bv, nib * 4 + b, QSIM_VAL_X);
                    else qsim_bit_set(bv, nib * 4 + b, (hval >> b) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                }
            }
        } else {
            /* For base 8 (octal) and others, mark all bits as X */
            for (uint32_t i = 0; i < width; i++)
                qsim_bit_set(bv, i, QSIM_VAL_X);
        }
    } else {
        uint64_t v64 = (uint64_t)val;
        for (uint32_t i = 0; i < width; i++)
            qsim_bit_set(bv, i, (i < 64 && ((v64 >> i) & 1)) ? QSIM_VAL_1 : QSIM_VAL_0);
    }
    return (uir_node_t *)uir_make_literal(unit, bv, loc);
}

/* ================================================================
 * Override peg YY_INPUT to read from _parse_source
 * ================================================================ */

#define YY_INPUT(buf, result, max_size) \
  do { \
    size_t _yy_avail = _parse_source_len - _parse_source_pos; \
    size_t _yy_n = _yy_avail < (size_t)(max_size) ? _yy_avail : (size_t)(max_size); \
    if (_yy_n > 0) memcpy(buf, _parse_source + _parse_source_pos, _yy_n); \
    _parse_source_pos += _yy_n; \
    (result) = (int)_yy_n; \
  } while (0)

/* ================================================================
 * Include generated PEG parser
 * ================================================================ */

/* #define YY_DEBUG */  /* enable for parser debug trace (very verbose) */
#include "grammar.peg.c"

/* ================================================================
 * Public API
 * ================================================================ */

parse_result_t verilog_parse(const char *filename, const char *source, size_t length)
{
    parse_result_t result;
    memset(&result, 0, sizeof(result));

    if (!source || length == 0) {
        result.success = 0;
        return result;
    }

    /* Set up parser context */
    _parse_filename = filename;
    _parse_source = source;
    _parse_source_len = length;
    _parse_source_pos = 0;
    _parse_unit = NULL;
    _parse_ok = 0;
    _parse_all_consumed = 0;
    _parse_units = NULL;
    _parse_unit_count = 0;
    _parse_unit_cap = 0;
    _parse_saved = NULL;
    _parse_process = NULL;
    _parse_edge_flag = 0;
    _parse_sens_names = NULL;
    _parse_sens_edges = NULL;
    _parse_sens_count = 0;
    _parse_range_msb = 0;
    _parse_range_lsb = 0;
    _parse_saved_range_width = 1;
    _parse_array_msb = 0;
    _parse_array_lsb = 0;
    _parse_array_size = 0;
    _parse_saved_array_size = 0;
    _expr_sp = 0;
    _parse_case_saved_sp_depth = 0;
    _parse_block_sp = -1;
    _parse_had_else = 0;
    _parse_auto_sens = 0;
    _parse_then_block = NULL;
    _parse_else_block = NULL;
    _parse_lhs_index = NULL;
    _parse_assign_lhs = NULL;
    _parse_array_id = NULL;
    _parse_concat_count = 0;
    _parse_array_dims_count = 0;
    _parse_assign_delay = NULL;
    _parse_generate_active = 1;
    _parse_saved_unit = NULL;
    _parse_genvar_id = NULL;
    _parse_gen_label = NULL;
    _parse_current_gen = NULL;
    _parse_gen_case_matched = 0;
    _parse_gen_label_sp = 0;
    _parse_gen_if_nested = 0;
    _parse_gen_if_node = NULL;
    _parse_gen_if_outer = NULL;
    _parse_gen_if_branch = 0;
    _parse_func = NULL;
    _parse_func_call_name = NULL;
    _parse_func_call_sp = 0;
    _parse_saved2 = NULL;
    _parse_saved3 = NULL;
    _parse_modport_dir = UIR_PORT_IN;
    _parse_port_sig_type = UIR_SIG_WIRE;
    _parse_loop_cond = NULL;
    _parse_loop_init = NULL;
    _parse_loop_step = NULL;
    _parse_loop_count = NULL;
    _parse_loop_body = NULL;
    _parse_block_name = NULL;
    _parse_bare_port_count = 0;
    /* Reset defparam and attr state */
    if (_parse_defparam_path) { free(_parse_defparam_path); _parse_defparam_path = NULL; }
    _parse_attr_count = 0;
    /* Reset template-mode generate case state */
    _parse_gen_case_active = 0;
    _parse_gen_case_node = NULL;
    _parse_gen_case_outer = NULL;
    _parse_gen_case_cur_temp = NULL;
    _parse_gen_case_default_mode = 0;
    /* Reset UDP parsing state */
    _parse_udp_is_seq = 0;
    _parse_udp_output_is_reg = 0;
    _parse_udp_edge_buf[0] = '\0';
    _parse_udp_input_len = 0;
    _parse_udp_state_char = '\0';
    _parse_udp_output_char = '\0';
    /* Run the PEG parser */
    yyctx->__limit = 0; /* force refill on first read (peg global state) */
    int ok = yyparsefrom(yy_design_file);

    if (ok && _parse_ok) {
        /* Save any remaining unit not yet saved by save_unit() */
        if (_parse_unit)
            save_unit();
        if (!_parse_all_consumed) {
            /* PEG rule succeeded but didn't consume all input — set error */
            result.error_count = 1;
            size_t ctx_pos = yyctx->__pos;
            /* Convert byte offset to line/column */
            int line = 1, col = 1;
            for (size_t i = 0; i < ctx_pos && i < _parse_source_len; i++) {
                if (_parse_source[i] == '\n') { line++; col = 1; }
                else col++;
            }
            if (ctx_pos > 20) ctx_pos -= 20; else ctx_pos = 0;
            snprintf(result.errors[0].message, sizeof(result.errors[0].message),
                     "Parse did not consume all input (remaining near '%.30s')",
                     _parse_source + ctx_pos);
            result.errors[0].line = line;
            result.errors[0].column = col;
        }
        result.units = _parse_units;
        result.unit_count = _parse_unit_count;
        result.unit = _parse_unit_count > 0 ? _parse_units[0] : NULL;
        result.success = 1;
        /* Ownership transferred to result — prevent cleanup from freeing them */
        _parse_units = NULL;
        _parse_unit_count = 0;
        _parse_unit_cap = 0;
    } else {
        result.error_count = 1;
        size_t ctx_pos = _parse_source_pos;
        /* Convert byte offset to line/column */
        int line = 1, col = 1;
        for (size_t i = 0; i < ctx_pos && i < _parse_source_len; i++) {
            if (_parse_source[i] == '\n') { line++; col = 1; }
            else col++;
        }
        if (ctx_pos > 20) ctx_pos -= 20; else ctx_pos = 0;
        snprintf(result.errors[0].message, sizeof(result.errors[0].message),
                 "Parse failed near '%.30s'", _parse_source + ctx_pos);
        result.errors[0].line = line;
        result.errors[0].column = col;
    }

    /* Clean up any saved string */
    if (_parse_saved) {
        free(_parse_saved);
        _parse_saved = NULL;
    }
    if (_parse_saved2) {
        free(_parse_saved2);
        _parse_saved2 = NULL;
    }
    if (_parse_saved3) {
        free(_parse_saved3);
        _parse_saved3 = NULL;
    }
    if (_parse_assign_lhs) {
        free(_parse_assign_lhs);
        _parse_assign_lhs = NULL;
    }
    if (_parse_assign_delay) {
        /* delay_value is an expression node; it belongs to the unit arena if
         * parsing succeeded, otherwise discard the pointer. */
        _parse_assign_delay = NULL;
    }
    if (_parse_array_id) {
        free(_parse_array_id);
        _parse_array_id = NULL;
    }
    if (_parse_block_name) {
        free(_parse_block_name);
        _parse_block_name = NULL;
    }

    /* Clean up genvar name tracking */
    for (int _gi = 0; _gi < _parse_genvar_name_count; _gi++)
        free(_parse_genvar_names[_gi]);
    _parse_genvar_name_count = 0;

    /* Clean up non-ANSI port name tracking */
    for (int _pi = 0; _pi < _parse_bare_port_count; _pi++)
        if (_parse_bare_port_names[_pi])
            free(_parse_bare_port_names[_pi]);
    _parse_bare_port_count = 0;

    return result;
}

parse_result_t verilog_parse_file_ex(const char *filename,
                                      const char **include_paths,
                                      int include_path_count)
{
    /* Read the file */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        parse_result_t result;
        memset(&result, 0, sizeof(result));
        result.success = 0;
        return result;
    }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (flen <= 0) {
        fclose(f);
        parse_result_t result;
        memset(&result, 0, sizeof(result));
        result.success = 0;
        return result;
    }

    char *buf = malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return verilog_parse(filename, "", 0); }

    size_t nread = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[nread] = '\0';

    /* Strip UTF-8 BOM (0xEF 0xBB 0xBF) if present */
    const char *src_start = buf;
    size_t src_len = nread;
    if (nread >= 3 && (unsigned char)buf[0] == 0xEF &&
                      (unsigned char)buf[1] == 0xBB &&
                      (unsigned char)buf[2] == 0xBF) {
        src_start = buf + 3;
        src_len = nread - 3;
    }

    /* Run preprocessor on the source */
    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    if (!pp) {
        parse_result_t result;
        memset(&result, 0, sizeof(result));
        result.success = 0;
        free(buf);
        return result;
    }

    /* Add caller's include paths */
    for (int i = 0; i < include_path_count; i++)
        verilog_preprocessor_add_include_path(pp, include_paths[i]);

    char *expanded = verilog_preprocessor_process(pp, filename, src_start, src_len);
    free(buf);

    if (!expanded) {
        parse_result_t result;
        memset(&result, 0, sizeof(result));
        const char *pp_err = verilog_preprocessor_get_error(pp);
        if (pp_err && pp_err[0]) {
            result.error_count = 1;
            snprintf(result.errors[0].message, sizeof(result.errors[0].message),
                     "preprocessor: %s", pp_err);
            result.errors[0].line = 0;
            result.errors[0].column = 0;
        }
        verilog_preprocessor_destroy(pp);
        return result;
    }

    /* Parse the expanded source */
    parse_result_t result = verilog_parse(filename, expanded, strlen(expanded));
    free(expanded);
    verilog_preprocessor_destroy(pp);
    return result;
}

parse_result_t verilog_parse_file(const char *filename)
{
    return verilog_parse_file_ex(filename, NULL, 0);
}
