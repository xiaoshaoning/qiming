#include "libqsim/vhdl_parser.h"
#include "libqsim/uir.h"
#include "libqsim/value.h"
#include "libqsim/vhdl_pkg_registry.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================
 * Parser context (shared with grammar.peg actions)
 * ================================================================ */

static const char *_parse_filename = NULL;
static uir_design_unit_t *_parse_unit = NULL;
static int _parse_ok = 0;
static char *_parse_saved = NULL;
static char *_parse_slice_saved = NULL;
static uir_process_t *_parse_process = NULL;

/* Sensitivity list tracking */
static char **_parse_sens_names = NULL;
static size_t _parse_sens_count = 0;
static int _parse_vhdl_process_all = 0; /* process(all) auto-sensitivity */

/* Decl and port tracking (shared with grammar) */
#define ID_LIST_MAX 64
static char *_parse_id_list[ID_LIST_MAX];
static size_t _parse_id_count = 0;
static uir_port_dir_t _parse_port_dir = UIR_PORT_IN;
static int _parse_saved_range_width = 1;
static int _parse_signal_is_bit_type = 0;
static int _parse_range_width = 32;
static uint32_t _parse_saved_array_size = 0;
static uint32_t _parse_saved_array_dims[4] = {0};
static size_t _parse_saved_array_dim_count = 0;

/* If-statement state (vhdl_if_* helpers) */
static int _parse_had_else = 0;
static uir_block_t *_parse_then_block = NULL;
static uir_block_t *_parse_else_block = NULL;

/* Assignment LHS part-select tracking */
static uir_node_t *_parse_target_hi = NULL;
static uir_node_t *_parse_target_lo = NULL;

/* With-select saved expression stack pointer */
static int _parse_select_saved_sp = 0;

/* With-select UIR construction state */
static uir_node_t *_parse_select_selector = NULL;
static char *_parse_select_target = NULL;
static int _parse_select_item_sp = 0;
static uir_node_t **_parse_select_values = NULL;
static uir_node_t ***_parse_select_patterns = NULL;
static size_t *_parse_select_pattern_counts = NULL;
static size_t _parse_select_item_count = 0;
static size_t _parse_select_item_cap = 0;
static uir_node_t *_parse_select_default_value = NULL;

/* File declaration state (TEXTIO) */
static char *_parse_file_mode = NULL;
static char *_parse_file_name_str = NULL;

/* Generate block state */
static char *_parse_genvar_name = NULL;
static uir_node_t *_parse_gen_lo = NULL;
static uir_node_t *_parse_gen_hi = NULL;
static int _parse_gen_direction = 0;
static uir_design_unit_t *_parse_gen_outer = NULL;
static uir_generate_t *_parse_current_gen = NULL;
static uir_node_t *_parse_gen_if_cond = NULL;

/* Subprogram UIR construction state */
static uir_design_unit_t *_parse_func_parent = NULL;
static uir_design_unit_t *_parse_func_temp = NULL;
static uir_func_t *_parse_func = NULL;

/* Component declaration UIR construction state */
static uir_design_unit_t *_parse_comp_parent = NULL;
static uir_design_unit_t *_parse_comp_temp = NULL;
static uir_component_t *_parse_comp = NULL;

/* Configuration declaration state */
static char *_parse_config_name = NULL;
static char *_parse_config_entity = NULL;
/* Alias declaration state */
static char *_parse_alias_name = NULL;
/* Attribute specification state */
static char *_parse_attr_name = NULL;
static char *_parse_attr_target = NULL;
static char *_parse_attr_class = NULL;
/* Group declaration state */
static char *_parse_group_name = NULL;
static uir_vhdl_group_t *_parse_group = NULL;
/* Type/subtype UIR construction state */
static uir_vhdl_type_t *_parse_type = NULL;

/* Type-spec state: initial value for subsequent signal/port declarations */
static int _parse_init_state = QSIM_X;

/* Selected-name state (a.b.c, ieee.std_logic_1164.rising_edge) */
static char *_parse_selname_buf = NULL;

/* Function/procedure call state */
static char _parse_call_name[256];
#define _PARSE_CALL_NAME_STACK_DEPTH 16
static char _parse_call_name_stack[_PARSE_CALL_NAME_STACK_DEPTH][256];
static int _parse_call_name_depth = 0;

static void vhdl_push_call_name(const char *name) {
    if (_parse_call_name_depth < _PARSE_CALL_NAME_STACK_DEPTH) {
        strncpy(_parse_call_name_stack[_parse_call_name_depth], name, 255);
        _parse_call_name_stack[_parse_call_name_depth][255] = '\0';
        _parse_call_name_depth++;
    }
}

static const char *vhdl_pop_call_name(void) {
    if (_parse_call_name_depth > 0) {
        _parse_call_name_depth--;
        return _parse_call_name_stack[_parse_call_name_depth];
    }
    return "";
}

static uir_node_t **_parse_call_args = NULL;
static size_t _parse_call_arg_count = 0;
static size_t _parse_call_arg_cap = 0;
static int _parse_call_saved_sp = 0;

/* Wait statement state */
static uir_node_t *_parse_wait_condition = NULL;
static uir_node_t *_parse_wait_delay = NULL;
static char **_parse_wait_sens_list = NULL;
static size_t _parse_wait_sens_count = 0;

/* Return statement state */
static uir_node_t *_parse_ret_expr = NULL;

/* Assert statement state */
static uir_node_t *_parse_assert_cond = NULL;
static uir_node_t *_parse_assert_msg = NULL;
static char *_parse_assert_sev = NULL;  /* severity string from assert */

/* Library/use clause storage */
static char **_parse_library_names = NULL;
static size_t _parse_library_count = 0;
static char **_parse_use_clauses = NULL;
static size_t _parse_use_count = 0;

/* Saved strings for use_clause grammar actions */
static char *_parse_saved2 = NULL;
static char *_parse_saved3 = NULL;

/* Case-statement state (vhdl_case_* helpers) */
static int _parse_case_saved_sp = 0;
static int _parse_process_idx = 0;
static uir_case_item_t **_parse_case_items = NULL;
static size_t _parse_case_item_count = 0;
static size_t _parse_case_item_cap = 0;
static uir_node_t *_parse_case_default = NULL;
static uir_node_t **_parse_case_item_patterns = NULL;
static size_t _parse_case_item_pattern_count = 0;
static uir_block_t *_parse_case_item_body = NULL;
static uir_node_t *_parse_case_default_body = NULL;

/* Nested case save/restore stack — nested cases overwrite shared _parse_case_item_*
 * variables (body, patterns, items list). We push before entering a new case and
 * pop when it finishes so the outer case's state is intact. */
#define CASE_NEST_MAX 16
typedef struct {
    uir_case_item_t **items;
    size_t item_count;
    size_t item_cap;
    uir_node_t *default_body;
    uir_node_t **patterns;
    size_t pattern_count;
    uir_block_t *item_body;
    uir_node_t *case_default;
    int saved_sp;
} case_nest_save_t;
static case_nest_save_t _case_nest_stack[CASE_NEST_MAX];
static int _case_nest_sp = -1;

/* ================================================================
 * Expression value stack
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

/* ================================================================
 * Statement block stack
 * ================================================================ */

static uir_block_t *_parse_block_stack[16];
static int _parse_block_sp = -1;

static void push_stmt_block(uir_block_t *block) {
    if (_parse_block_sp < 15)
        _parse_block_stack[++_parse_block_sp] = block;
}

static void pop_stmt_block(void) {
    if (_parse_block_sp >= 0)
        _parse_block_sp--;
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

/* ================================================================
 * Source input for peg (read via YY_INPUT)
 * ================================================================ */

static const char *_parse_source = NULL;
static size_t _parse_source_len = 0;
static size_t _parse_source_pos = 0;

/* ================================================================
 * Helper functions
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

static char *parse_strdup_and_lower(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    for (size_t i = 0; i < n; i++)
        p[i] = (char)tolower((unsigned char)s[i]);
    return p;
}

/* ── ID list helpers (for port_item, signal_decl, variable_decl) ── */

static void id_list_add(const char *name) {
    if (_parse_id_count < ID_LIST_MAX)
        _parse_id_list[_parse_id_count++] = parse_strdup(name);
}

static void id_list_clear(void) {
    for (size_t i = 0; i < _parse_id_count; i++)
        free(_parse_id_list[i]);
    _parse_id_count = 0;
}

static void finish_port_items(void) {
    if (!_parse_unit) { id_list_clear(); return; }
    for (size_t i = 0; i < _parse_id_count; i++) {
        uir_port_t *p = uir_add_port(_parse_unit, _parse_id_list[i],
                                       _parse_port_dir, 0, 0, UIR_SIG_VHDL_SIGNAL);
        if (p) {
            p->width = (uint32_t)_parse_saved_range_width;
            p->init_value.state = (qsim_logic_state_t)_parse_init_state;
            if (_parse_saved_array_size > 0) {
                p->array_size = _parse_saved_array_size;
                p->array_dim_count = 1;
                p->array_dims[0] = _parse_saved_array_size;
            }
        }
    }
    id_list_clear();
    _parse_saved_array_size = 0;
}

static void finish_signal_decls(uir_signal_type_t type) {
    if (!_parse_unit) { id_list_clear(); return; }
    for (size_t i = 0; i < _parse_id_count; i++) {
        uir_signal_t *sig = uir_add_signal(_parse_unit, _parse_id_list[i],
                        type, (uint32_t)_parse_saved_range_width, _parse_saved_array_size);
        if (sig) {
            sig->init_value.state = (qsim_logic_state_t)_parse_init_state;
            if (_parse_saved_array_dim_count > 0) {
                sig->array_dim_count = _parse_saved_array_dim_count;
                for (size_t d = 0; d < _parse_saved_array_dim_count && d < 4; d++)
                    sig->array_dims[d] = _parse_saved_array_dims[d];
            } else if (_parse_saved_array_size > 0) {
                sig->array_dim_count = 1;
                sig->array_dims[0] = _parse_saved_array_size;
            }
        }
    }
    id_list_clear();
    _parse_saved_array_size = 0;
    _parse_saved_array_dim_count = 0;
    memset(_parse_saved_array_dims, 0, sizeof(_parse_saved_array_dims));
}

/* ── File variable declarations (TEXTIO) ── */

static void finish_file_decls(void) {
    if (!_parse_unit) { id_list_clear(); return; }
    int mode = -1; /* default */
    if (_parse_file_mode) {
        if (strcmp(_parse_file_mode, "read_mode") == 0 ||
            strcmp(_parse_file_mode, "read") == 0)
            mode = 0;
        else if (strcmp(_parse_file_mode, "write_mode") == 0 ||
                 strcmp(_parse_file_mode, "write") == 0)
            mode = 1;
        else if (strcmp(_parse_file_mode, "append_mode") == 0 ||
                 strcmp(_parse_file_mode, "append") == 0)
            mode = 2;
        free(_parse_file_mode);
        _parse_file_mode = NULL;
    }
    for (size_t i = 0; i < _parse_id_count; i++) {
        uir_signal_t *sig = uir_add_signal(_parse_unit, _parse_id_list[i],
                        UIR_SIG_VHDL_VARIABLE, (uint32_t)_parse_saved_range_width, 0);
        if (sig) {
            sig->init_value.state = (qsim_logic_state_t)_parse_init_state;
        }
        uir_add_file_meta(_parse_unit, _parse_id_list[i], mode,
                          _parse_file_name_str);
    }
    free(_parse_file_name_str);
    _parse_file_name_str = NULL;
    id_list_clear();
    _parse_saved_array_size = 0;
    _parse_saved_array_dim_count = 0;
    memset(_parse_saved_array_dims, 0, sizeof(_parse_saved_array_dims));
}

/* ── Sensitivity ── */

static void record_sensitivity(const char *name) {
    char **nn = realloc(_parse_sens_names, (_parse_sens_count + 1) * sizeof(char *));
    if (!nn) return;
    _parse_sens_names = nn;
    _parse_sens_names[_parse_sens_count] = parse_strdup(name);
    _parse_sens_count++;
}

static void finalize_process_sensitivity(uir_process_t *proc) {
    if (!proc || _parse_sens_count == 0) return;
    proc->sensitivity_list = calloc(_parse_sens_count, sizeof(uir_sensitivity_t));
    if (!proc->sensitivity_list) return;
    proc->sensitivity_count = _parse_sens_count;
    for (size_t i = 0; i < _parse_sens_count; i++) {
        proc->sensitivity_list[i].edge = 0; /* level-sensitive */
        proc->sensitivity_list[i].signal = uir_find_signal(
            _parse_unit, _parse_sens_names[i]);
    }
    for (size_t i = 0; i < _parse_sens_count; i++)
        free(_parse_sens_names[i]);
    free(_parse_sens_names);
    _parse_sens_names = NULL;
    _parse_sens_count = 0;
}

/* ── Process helpers ── */

static void start_vhdl_process(uir_design_unit_t *unit, const char *name) {
    if (unit) {
        _parse_process = uir_add_process(unit, UIR_PROC_VHDL);
        if (name) _parse_process->name = parse_strdup(name);
        _parse_process->body = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_process->body);
    } else {
        fprintf(stderr, "ERROR - unit is NULL for process '%s'\n",
                name ? name : "(unnamed)");
    }
}

static void finish_vhdl_process(void) {
    if (_parse_process) {
        pop_stmt_block();
        if (_parse_vhdl_process_all) {
            _parse_process->auto_sens = 1;
            _parse_vhdl_process_all = 0;
        }
        finalize_process_sensitivity(_parse_process);
        _parse_process = NULL;
    }
}

static void vhdl_process_all(void) {
    _parse_vhdl_process_all = 1;
}

/* ── If-statement helpers ── */

/* VHDL if/elsif/else uses nested UIR_IF in else branches.
 * if A then B elsif C then D else E end if;
 *   → UIR_IF(cond=A, then=B, else=UIR_IF(cond=C, then=D, else=E))
 *
 * We build from the outside inwards, pushing/poping block stacks:
 * 1. vhdl_if_then_start:  push then-block
 * 2. vhdl_if_elsif_start: pop then-block, create & push else-block (which will contain a nested if)
 * 3. vhdl_if_else_start:  pop then-block (or elsif-block), create & push else-block
 * 4. vhdl_if_finish:      pop, build UIR_IF chain from innermost outward
 */

/*
 * Elif chaining uses a block-stack approach:
 *
 * For each elsif we encounter, the PREVIOUS level's then-block is saved
 * on _vhdl_elsif_blocks. Conditions are left on the expr stack so that
 * vhdl_if_finish can pop them innermost-first (reverse order), building
 * the correct IF chain:
 *
 *   if A then B elsif C then D end if;
 *     → UIR_IF(cond=A, then=B, else=UIR_IF(cond=C, then=D))
 *
 * The expr stack at finish has: [A, C] (A pushed first, C on top).
 * vhdl_if_finish pops C first → innermost IF, then A → outermost IF.
 */
#define VHDL_ELSIF_STACK_MAX 16

/* Stack of then-blocks from each if/elsif level (excluding the active one) */
static uir_block_t *_vhdl_elsif_blocks[VHDL_ELSIF_STACK_MAX];
static int _vhdl_elsif_sp = -1;

/* Stack for saving/restoring if-state across nested if statements */
#define VHDL_IF_NEST_MAX 16
static int _saved_elsif_sp_stack[VHDL_IF_NEST_MAX];
static uir_block_t *_saved_then_block_stack[VHDL_IF_NEST_MAX];
static uir_block_t *_saved_else_block_stack[VHDL_IF_NEST_MAX];
static int _saved_had_else_stack[VHDL_IF_NEST_MAX];
/* Saved elsif blocks array contents — preserves the array across nested IF entry/exit */
static uir_block_t *_saved_elsif_blocks_stack[VHDL_IF_NEST_MAX][VHDL_ELSIF_STACK_MAX];
static int _vhdl_if_nest_sp = -1;

static void vhdl_if_then_start(void) {
    /* Save current if-state before starting this new if statement */
    if (_vhdl_if_nest_sp < VHDL_IF_NEST_MAX - 1) {
        _vhdl_if_nest_sp++;
        _saved_elsif_sp_stack[_vhdl_if_nest_sp] = _vhdl_elsif_sp;
        /* Save the entire elsif blocks array contents so nested elsif
         * processing doesn't corrupt the outer IF's saved blocks */
        for (int _i = 0; _i <= _vhdl_elsif_sp; _i++)
            _saved_elsif_blocks_stack[_vhdl_if_nest_sp][_i] = _vhdl_elsif_blocks[_i];
        _saved_then_block_stack[_vhdl_if_nest_sp] = _parse_then_block;
        _saved_else_block_stack[_vhdl_if_nest_sp] = _parse_else_block;
        _saved_had_else_stack[_vhdl_if_nest_sp] = _parse_had_else;
    }

    /* Reset for this new if statement */
    _vhdl_elsif_sp = -1;
    _parse_had_else = 0;
    _parse_then_block = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_then_block);
}

static void vhdl_if_elsif_start(void) {
    pop_stmt_block(); /* pop previous then/elsif block from stmt stack */

    /* Save the previous level's then-block on the elsif block stack.
     * Its condition is already on the expr stack — leave it there for
     * vhdl_if_finish to pop in reverse order. */
    if (_vhdl_elsif_sp < VHDL_ELSIF_STACK_MAX - 1) {
        _vhdl_elsif_blocks[++_vhdl_elsif_sp] = _parse_then_block;
    }

    /* Create new block for this elsif's then-branch */
    _parse_then_block = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_then_block);
}

static void vhdl_if_else_start(void) {
    pop_stmt_block();
    _parse_else_block = uir_add_block(_parse_unit, 1);
    push_stmt_block(_parse_else_block);
    _parse_had_else = 1;
}

static void vhdl_if_finish(void) {
    pop_stmt_block(); /* pop the last body block from stmt stack */

    /* Build the UIR_IF chain from innermost to outermost.
     *
     * The expr stack has all conditions in source order:
     *   [if_cond, elsif1_cond, ..., elsifN_cond]
     *
     * The elsif block stack has then-blocks for each if/elsif level
     * EXCEPT the last one (which is still in _parse_then_block).
     *
     * We pop from the ends (innermost = last condition + last body)
     * and wrap outward until all conditions are consumed.
     */
    uir_if_t *current_if = NULL;

    if (_vhdl_elsif_sp < 0) {
        /* ── Simple if-then[-else], no elsif ── */
        uir_node_t *cond = expr_pop();
        if (_parse_then_block) {
            current_if = (uir_if_t *)uir_alloc_node(_parse_unit, UIR_IF, sizeof(uir_if_t), parse_loc());
            current_if->condition = cond;
            current_if->then_branch = (uir_node_t *)_parse_then_block;
            current_if->else_branch = _parse_had_else ? (uir_node_t *)_parse_else_block : NULL;
        }
    } else {
        /* ── One or more elsif clauses ── */
        /* The innermost condition is on top of the expr stack.
         * _parse_then_block holds its then-body (or the else-body if
         * _parse_had_else is set). */
        uir_node_t *cond = expr_pop();

        if (_parse_then_block) {
            current_if = (uir_if_t *)uir_alloc_node(_parse_unit, UIR_IF, sizeof(uir_if_t), parse_loc());
            current_if->condition = cond;
            current_if->then_branch = (uir_node_t *)_parse_then_block;
            current_if->else_branch = _parse_had_else ? (uir_node_t *)_parse_else_block : NULL;
        }

        /* Wrap outward: each saved block becomes the then-branch of the
         * next outer IF, and the current IF chain becomes its else-branch. */
        while (_vhdl_elsif_sp >= 0) {
            uir_block_t *prev_then = _vhdl_elsif_blocks[_vhdl_elsif_sp--];
            uir_node_t *prev_cond = expr_pop();

            uir_if_t *new_if = (uir_if_t *)uir_alloc_node(_parse_unit, UIR_IF, sizeof(uir_if_t), parse_loc());
            new_if->condition = prev_cond;
            new_if->then_branch = (uir_node_t *)prev_then;
            new_if->else_branch = (uir_node_t *)current_if;
            current_if = new_if;
        }
    }

    if (current_if) {
        append_stmt((uir_node_t *)current_if);
    }

    /* Restore parent if-state from stack */
    if (_vhdl_if_nest_sp >= 0) {
        /* Restore elsif blocks array contents first (preserve the saved values) */
        int _saved_sp = _saved_elsif_sp_stack[_vhdl_if_nest_sp];
        for (int _i = 0; _i <= _saved_sp; _i++)
            _vhdl_elsif_blocks[_i] = _saved_elsif_blocks_stack[_vhdl_if_nest_sp][_i];
        _vhdl_elsif_sp = _saved_sp;
        _parse_then_block = _saved_then_block_stack[_vhdl_if_nest_sp];
        _parse_else_block = _saved_else_block_stack[_vhdl_if_nest_sp];
        _parse_had_else = _saved_had_else_stack[_vhdl_if_nest_sp];
        _vhdl_if_nest_sp--;
    } else {
        _parse_then_block = NULL;
        _parse_else_block = NULL;
        _parse_had_else = 0;
        _vhdl_elsif_sp = -1;
    }
}

/* ── Case-statement helpers ── */

static void vhdl_case_enter(void) {
    /* Save outer case state (for nested cases) */
    if (_case_nest_sp < CASE_NEST_MAX - 1) {
        _case_nest_sp++;
        _case_nest_stack[_case_nest_sp].items = _parse_case_items;
        _case_nest_stack[_case_nest_sp].item_count = _parse_case_item_count;
        _case_nest_stack[_case_nest_sp].item_cap = _parse_case_item_cap;
        _case_nest_stack[_case_nest_sp].default_body = _parse_case_default_body;
        _case_nest_stack[_case_nest_sp].patterns = _parse_case_item_patterns;
        _case_nest_stack[_case_nest_sp].pattern_count = _parse_case_item_pattern_count;
        _case_nest_stack[_case_nest_sp].item_body = _parse_case_item_body;
        _case_nest_stack[_case_nest_sp].case_default = _parse_case_default;
        _case_nest_stack[_case_nest_sp].saved_sp = _parse_case_saved_sp;
    }
    _parse_case_saved_sp = _expr_sp;
    _parse_case_items = NULL;
    _parse_case_item_count = 0;
    _parse_case_item_cap = 0;
    _parse_case_default = NULL;
}

static void vhdl_case_item_enter(uir_design_unit_t *unit) {
    int n = _expr_sp - _parse_case_saved_sp;
    if (n <= 0) return;

    uir_node_t **patterns = malloc((size_t)n * sizeof(uir_node_t *));
    if (!patterns) return;
    for (int i = 0; i < n; i++)
        patterns[i] = _expr_stack[_parse_case_saved_sp + i];
    _expr_sp = _parse_case_saved_sp;

    _parse_case_item_patterns = patterns;
    _parse_case_item_pattern_count = (size_t)n;

    if (unit) {
        _parse_case_item_body = uir_add_block(unit, 1);
        push_stmt_block(_parse_case_item_body);
    }
}

static void vhdl_case_item_finish(uir_design_unit_t *unit) {
    if (_parse_case_item_body) pop_stmt_block();
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

static void vhdl_case_default_enter(uir_design_unit_t *unit) {
    if (unit) {
        _parse_case_default = (uir_node_t *)uir_add_block(unit, 1);
        push_stmt_block((uir_block_t *)_parse_case_default);
    }
}

static void vhdl_case_default_finish(void) {
    if (_parse_case_default) pop_stmt_block();
    /* _parse_case_default is set from enter */
}

static void vhdl_case_finish(uir_design_unit_t *unit) {
    if (!unit) return;

    uir_node_t *case_expr = expr_pop();

    uir_case_t *case_node = (uir_case_t *)uir_alloc_node(
        unit, UIR_CASE, sizeof(uir_case_t), parse_loc());
    if (!case_node) return;
    case_node->expr = case_expr;
    case_node->items = _parse_case_items;
    case_node->item_count = _parse_case_item_count;
    case_node->default_item = _parse_case_default;
    case_node->is_wildcard = 0; /* VHDL case is always exact */

    /* Transfer ownership: prevent next case from realloc'ing this items array */
    _parse_case_items = NULL;
    _parse_case_item_count = 0;
    _parse_case_item_cap = 0;

    append_stmt((uir_node_t *)case_node);

    _parse_case_default = NULL;

    /* Restore outer case state if we're inside a nested case */
    if (_case_nest_sp >= 0) {
        _parse_case_items = _case_nest_stack[_case_nest_sp].items;
        _parse_case_item_count = _case_nest_stack[_case_nest_sp].item_count;
        _parse_case_item_cap = _case_nest_stack[_case_nest_sp].item_cap;
        _parse_case_default_body = _case_nest_stack[_case_nest_sp].default_body;
        _parse_case_item_patterns = _case_nest_stack[_case_nest_sp].patterns;
        _parse_case_item_pattern_count = _case_nest_stack[_case_nest_sp].pattern_count;
        _parse_case_item_body = _case_nest_stack[_case_nest_sp].item_body;
        _parse_case_default = _case_nest_stack[_case_nest_sp].case_default;
        _parse_case_saved_sp = _case_nest_stack[_case_nest_sp].saved_sp;
        _case_nest_sp--;
    }
}

/* ── For-loop helpers ── */

static void vhdl_for_loop_enter(uir_design_unit_t *unit, const char *id) {
    if (!unit) return;
    _parse_saved = parse_strdup(id);
    /* Body block is pushed by grammar action before parsing stmts */
}

static void vhdl_for_loop_finish(uir_design_unit_t *unit) {
    if (!unit) return;

    /* Pop condition (range upper bound) and range lower bound from expr stack */
    uir_node_t *upper = expr_pop();
    uir_node_t *lower = expr_pop();
    (void)upper;
    (void)lower;

    uir_loop_t *loop = (uir_loop_t *)uir_alloc_node(unit, UIR_LOOP, sizeof(uir_loop_t), parse_loc());
    if (!loop) return;
    loop->init_stmt = NULL;
    loop->condition = NULL;  /* simplified: unconditional loop body */
    loop->step_stmt = NULL;
    /* Body was on block stack, popped by grammar after loop stmts.
     * At this point the body block has been filled by append_stmt calls. */

    append_stmt((uir_node_t *)loop);
}

/* ── Wait statement helpers ── */

static void vhdl_wait_on_name(const char *name) {
    char **nn = realloc(_parse_wait_sens_list, (_parse_wait_sens_count + 1) * sizeof(char *));
    if (!nn) return;
    _parse_wait_sens_list = nn;
    _parse_wait_sens_list[_parse_wait_sens_count] = parse_strdup(name);
    _parse_wait_sens_count++;
}

static void vhdl_wait_until_expr(void) {
    _parse_wait_condition = expr_pop();
}

static void vhdl_wait_for_expr(void) {
    _parse_wait_delay = expr_pop();
}

static void vhdl_do_wait(uir_design_unit_t *unit) {
    if (!unit) return;

    if (_parse_wait_delay) {
        uir_delay_t *d = (uir_delay_t *)uir_alloc_node(unit, UIR_DELAY, sizeof(uir_delay_t), parse_loc());
        if (d) {
            d->delay_value = _parse_wait_delay;
            d->body = NULL;
            d->always_loop = 0;
            append_stmt((uir_node_t *)d);
        }
    } else if (_parse_wait_condition || _parse_wait_sens_count > 0) {
        uir_wait_t *w = (uir_wait_t *)uir_alloc_node(unit, UIR_WAIT, sizeof(uir_wait_t), parse_loc());
        if (w) {
            w->condition = _parse_wait_condition;
            w->body = NULL;
            append_stmt((uir_node_t *)w);
        }
    } else {
        uir_wait_t *w = (uir_wait_t *)uir_alloc_node(unit, UIR_WAIT, sizeof(uir_wait_t), parse_loc());
        if (w) {
            w->condition = NULL;
            w->body = NULL;
            append_stmt((uir_node_t *)w);
        }
    }

    _parse_wait_condition = NULL;
    _parse_wait_delay = NULL;
    for (size_t i = 0; i < _parse_wait_sens_count; i++)
        free(_parse_wait_sens_list[i]);
    free(_parse_wait_sens_list);
    _parse_wait_sens_list = NULL;
    _parse_wait_sens_count = 0;
}

/* ── Exit, next, return helpers ── */

static void vhdl_do_exit(uir_design_unit_t *unit, const char *label) {
    if (!unit) return;
    uir_exit_t *e = (uir_exit_t *)uir_alloc_node(unit, UIR_EXIT, sizeof(uir_exit_t), parse_loc());
    if (e) {
        e->loop_label = label ? parse_strdup(label) : NULL;
        append_stmt((uir_node_t *)e);
    }
}

static void vhdl_do_next(uir_design_unit_t *unit, const char *label) {
    if (!unit) return;
    uir_next_t *n = (uir_next_t *)uir_alloc_node(unit, UIR_NEXT, sizeof(uir_next_t), parse_loc());
    if (n) {
        n->loop_label = label ? parse_strdup(label) : NULL;
        append_stmt((uir_node_t *)n);
    }
}

static void vhdl_do_return(uir_design_unit_t *unit) {
    if (!unit) return;
    uir_return_t *r = (uir_return_t *)uir_alloc_node(unit, UIR_RETURN, sizeof(uir_return_t), parse_loc());
    if (r) {
        r->expr = _parse_ret_expr;
        append_stmt((uir_node_t *)r);
    }
    _parse_ret_expr = NULL;
}

/* ── Subprogram UIR construction ── */

static void vhdl_func_enter(const char *name, int is_function) {
    _parse_func_parent = _parse_unit;
    _parse_func = NULL;
    _parse_func_temp = NULL;
    if (!_parse_unit) return;
    _parse_func = uir_add_func_task(_parse_unit, name, is_function);
    if (!_parse_func) return;
    char *lower = parse_strdup_and_lower(name);
    uir_loc_t loc = parse_loc();
    _parse_func_temp = uir_create_design_unit(lower, "vhdl", loc);
    free(lower);
    if (!_parse_func_temp) { _parse_unit = _parse_func_parent; return; }
    _parse_unit = _parse_func_temp;
}

static void vhdl_func_set_return_width(uint32_t width) {
    if (_parse_func) _parse_func->return_width = width;
}

static void vhdl_func_body_enter(void) {
    if (!_parse_func || !_parse_unit) return;
    uir_block_t *block = uir_add_block(_parse_unit, 1);
    _parse_func->body = (uir_node_t *)block;
    push_stmt_block(block);
}

static void vhdl_func_leave(void) {
    if (!_parse_func) {
        _parse_unit = _parse_func_parent;
        _parse_func_parent = NULL;
        pop_stmt_block();
        return;
    }
    pop_stmt_block();
    if (_parse_func_temp) {
        for (size_t i = 0; i < _parse_func_temp->port_count; i++) {
            uir_port_t *p = _parse_func_temp->ports[i];
            if (p) uir_add_func_port(_parse_func, p->name, p->direction, p->width);
        }
        for (size_t i = 0; i < _parse_func_temp->signal_count; i++) {
            uir_signal_t *s = _parse_func_temp->signals[i];
            if (s) uir_add_func_local(_parse_func, (uir_node_t *)s);
        }
    }
    _parse_unit = _parse_func_parent;
    _parse_func_parent = NULL;
    _parse_func = NULL;
    _parse_func_temp = NULL;
}

/* ── Component declaration UIR construction ── */

static void vhdl_comp_enter(const char *name) {
    _parse_comp_parent = _parse_unit;
    _parse_comp = NULL;
    _parse_comp_temp = NULL;
    if (!_parse_unit) return;
    _parse_comp = uir_add_component(_parse_unit, name);
    if (!_parse_comp) return;
    char *lower = parse_strdup_and_lower(name);
    uir_loc_t loc = parse_loc();
    _parse_comp_temp = uir_create_design_unit(lower, "vhdl_temp_comp", loc);
    free(lower);
    if (!_parse_comp_temp) { _parse_unit = _parse_comp_parent; return; }
    _parse_unit = _parse_comp_temp;
}

static void vhdl_comp_leave(void) {
    if (!_parse_comp) {
        _parse_unit = _parse_comp_parent;
        _parse_comp_parent = NULL;
        return;
    }
    if (_parse_comp_temp) {
        for (size_t i = 0; i < _parse_comp_temp->port_count; i++) {
            uir_port_t *p = _parse_comp_temp->ports[i];
            if (p) uir_add_component_port(_parse_comp,
                       p->name, p->direction, p->width);
        }
        uir_destroy_design_unit(_parse_comp_temp);
    }
    _parse_unit = _parse_comp_parent;
    _parse_comp_parent = NULL;
    _parse_comp = NULL;
    _parse_comp_temp = NULL;
}

/* ── Configuration declaration UIR construction ── */

static void vhdl_config_capture_name(const char *name) {
    free(_parse_config_name);
    _parse_config_name = name ? strdup(name) : NULL;
}

static void vhdl_config_capture_entity(const char *name) {
    free(_parse_config_entity);
    _parse_config_entity = name ? strdup(name) : NULL;
}

static void vhdl_config_enter(void) {
    if (!_parse_config_name) return;
    uir_loc_t loc = parse_loc();
    _parse_unit = uir_create_design_unit(_parse_config_name, "vhdl", loc);
    if (_parse_unit && _parse_config_entity)
        _parse_unit->config_entity_name = strdup(_parse_config_entity);
    free(_parse_config_name); _parse_config_name = NULL;
    free(_parse_config_entity); _parse_config_entity = NULL;
}

static void vhdl_config_add_block(const char *arch_name) {
    if (_parse_unit && arch_name)
        uir_add_vhdl_config_block(_parse_unit, arch_name);
}

/* ── Alias declaration UIR construction ── */

static void vhdl_alias_enter(const char *name) {
    free(_parse_alias_name);
    _parse_alias_name = name ? strdup(name) : NULL;
}

static void vhdl_alias_set_target(const char *target) {
    if (!_parse_unit || !_parse_alias_name || !target) {
        free(_parse_alias_name); _parse_alias_name = NULL;
        return;
    }
    uir_add_vhdl_alias(_parse_unit, _parse_alias_name, target);
    free(_parse_alias_name); _parse_alias_name = NULL;
}

static void vhdl_alias_finish(void) {
    /* Called after SEMI; target already set by vhdl_alias_set_target.
     * Clear alias name state in case the grammar didn't reach set_target. */
    free(_parse_alias_name); _parse_alias_name = NULL;
}

/* ── Attribute specification UIR construction ── */

static void vhdl_attr_spec_begin(const char *name) {
    free(_parse_attr_name); _parse_attr_name = name ? strdup(name) : NULL;
    free(_parse_attr_target); _parse_attr_target = NULL;
    free(_parse_attr_class); _parse_attr_class = NULL;
}

static void vhdl_attr_spec_set_target(const char *target) {
    free(_parse_attr_target); _parse_attr_target = target ? strdup(target) : NULL;
}

static void vhdl_attr_spec_set_class(const char *cls) {
    free(_parse_attr_class); _parse_attr_class = cls ? strdup(cls) : NULL;
}

static void vhdl_attr_spec_finish(void) {
    if (_parse_unit) {
        uir_node_t *val = expr_pop();
        const char *value_str = NULL;
        if (val && val->kind == UIR_LITERAL) {
            uir_literal_t *lit = (uir_literal_t *)val;
            uint32_t uv = 0;
            if (lit->value) {
                for (uint32_t i = 0; i < lit->value->width && i < 32; i++) {
                    qsim_value_t bit = qsim_bit_get(lit->value, i);
                    if (bit.state == QSIM_1) uv |= (1ULL << i);
                }
            }
            if (uv == 1) value_str = "true";
            else value_str = "false";
        }
        uir_add_vhdl_attr_spec(_parse_unit, _parse_attr_name, _parse_attr_target,
                                _parse_attr_class, value_str);
    }
    free(_parse_attr_name); _parse_attr_name = NULL;
    free(_parse_attr_target); _parse_attr_target = NULL;
    free(_parse_attr_class); _parse_attr_class = NULL;
}

/* ── Group/template UIR construction ── */

static void vhdl_group_save_name(const char *name) {
    free(_parse_group_name);
    _parse_group_name = name ? strdup(name) : NULL;
    _parse_group = NULL;
}

static void vhdl_group_begin(int kind, const char *template_name) {
    if (!_parse_unit || !_parse_group_name) { _parse_group = NULL; return; }
    _parse_group = uir_add_vhdl_group(_parse_unit, _parse_group_name,
        (uir_vhdl_group_kind_t)kind, template_name);
}

static void vhdl_group_add_constituent(const char *name) {
    if (_parse_group && name)
        uir_add_vhdl_group_constituent(_parse_group, name);
}

static void vhdl_group_finish(void) {
    free(_parse_group_name); _parse_group_name = NULL;
    _parse_group = NULL;
}

/* ── Type/subtype UIR construction ── */

static void vhdl_type_enter(const char *name, uir_vhdl_type_kind_t kind) {
    _parse_type = NULL;
    if (!_parse_unit || !name) return;
    _parse_type = uir_add_vhdl_type(_parse_unit, name, kind);
}

static void vhdl_type_add_literal(const char *name) {
    if (_parse_type) uir_add_vhdl_type_literal(_parse_type, name);
}

static void vhdl_type_finish_enum(void) {
    if (!_parse_type) return;
    _parse_type->kind = UIR_VHDL_TYPE_ENUM;
    /* Width = ceil(log2(count)); at least 1 */
    size_t n = _parse_type->enum_literal_count;
    uint32_t w = 1;
    while ((1ULL << w) < n) w++;
    _parse_type->width = w;
}

static int extract_uint32_from_node(uir_node_t *node, uint32_t *out);

static void vhdl_type_finish_range(void) {
    if (!_parse_type) return;
    _parse_type->kind = UIR_VHDL_TYPE_RANGE;
    /* Expr stack: left bound pushed first, right bound pushed second */
    uir_node_t *right = expr_pop();
    uir_node_t *left = expr_pop();
    uint32_t lv = 0, rv = 0;
    if (extract_uint32_from_node(left, &lv) && extract_uint32_from_node(right, &rv)) {
        int dir = (rv >= lv) ? 1 : -1;
        uir_set_vhdl_type_range(_parse_type, (int64_t)lv, (int64_t)rv, dir);
    } else {
        _parse_type->width = 32;
    }
}

static void vhdl_type_finish(void) {
    _parse_type = NULL;
}

/* ── Record type helpers ── */

static void vhdl_type_finish_record(void) {
    if (_parse_type) {
        _parse_type->kind = UIR_VHDL_TYPE_RECORD;
        _parse_type->width = 1; /* placeholder */
    }
    _parse_type = NULL;
}

static void vhdl_record_item_begin(const char *name) {
    id_list_add(name);
}

static void vhdl_record_item_add_name(const char *name) {
    id_list_add(name);
}

static void vhdl_record_item_finish(void) {
    if (!_parse_unit) { id_list_clear(); return; }
    uint32_t fwidth = (uint32_t)_parse_saved_range_width;
    for (size_t i = 0; i < _parse_id_count; i++) {
        if (_parse_type) uir_add_vhdl_record_field(_parse_type, _parse_id_list[i], fwidth);
    }
    id_list_clear();
}

static void vhdl_subtype_enter(const char *name) {
    _parse_type = NULL;
    if (!_parse_unit || !name) return;
    _parse_type = uir_add_vhdl_type(_parse_unit, name, UIR_VHDL_TYPE_SUBTYPE);
}

static void vhdl_subtype_base(const char *base_name) {
    if (_parse_type) uir_set_vhdl_type_base(_parse_type, base_name);
}

static void vhdl_subtype_set_range(void) {
    if (!_parse_type) return;
    uir_node_t *right = expr_pop();
    uir_node_t *left = expr_pop();
    uint32_t lv = 0, rv = 0;
    if (extract_uint32_from_node(left, &lv) && extract_uint32_from_node(right, &rv)) {
        int dir = (rv >= lv) ? 1 : -1;
        uir_set_vhdl_type_range(_parse_type, (int64_t)lv, (int64_t)rv, dir);
    }
}

static void vhdl_subtype_finish(void) {
    if (_parse_type) {
        /* If no explicit range, default width to 32 for integer subtypes */
        if (_parse_type->range_lo == 0 && _parse_type->range_hi == 0) {
            _parse_type->width = 32;
        }
    }
    _parse_type = NULL;
}

/* ── Array type helpers ── */

static int is_signal_array(uir_design_unit_t *unit, const char *name) {
    if (!unit || !name) return 0;
    uir_signal_t *sig = (uir_signal_t *)uir_find_signal(unit, name);
    return sig && sig->base.kind == UIR_SIGNAL && sig->array_size > 0;
}

static uir_vhdl_type_t *vhdl_lookup_type_by_name(const char *name) {
    if (!_parse_unit || !name) return NULL;
    for (size_t i = 0; i < _parse_unit->vhdl_type_count; i++) {
        if (strcmp(_parse_unit->vhdl_types[i].name, name) == 0)
            return &_parse_unit->vhdl_types[i];
    }
    return NULL;
}

static void vhdl_type_spec_id(const char *name) {
    _parse_init_state = QSIM_X;
    _parse_saved_array_size = 0;
    _parse_saved_array_dim_count = 0;
    /* Check if name matches a user-defined array type */
    uir_vhdl_type_t *t = vhdl_lookup_type_by_name(name);
    if (t && t->kind == UIR_VHDL_TYPE_ARRAY) {
        _parse_saved_range_width = (int)t->element_width;
        _parse_saved_array_size = t->array_size;
        _parse_saved_array_dim_count = t->array_dim_count;
        for (size_t i = 0; i < t->array_dim_count && i < 4; i++)
            _parse_saved_array_dims[i] = t->array_dims[i];
    } else {
        _parse_saved_range_width = 1;
    }
}

static void vhdl_type_finish_array(void) {
    if (!_parse_type) return;
    _parse_type->kind = UIR_VHDL_TYPE_ARRAY;
    _parse_type->array_dim_count = _parse_saved_array_dim_count;
    if (_parse_saved_array_dim_count > 0) {
        _parse_type->array_size = 1;
        for (size_t i = 0; i < _parse_saved_array_dim_count && i < 4; i++) {
            _parse_type->array_dims[i] = _parse_saved_array_dims[i];
            _parse_type->array_size *= _parse_saved_array_dims[i];
        }
    } else {
        /* Fallback for single-dim via _parse_saved_array_size */
        _parse_type->array_size = _parse_saved_array_size;
        _parse_type->array_dims[0] = _parse_saved_array_size;
        _parse_type->array_dim_count = 1;
    }
    _parse_type->element_width = (uint32_t)_parse_saved_range_width;
    _parse_type->width = _parse_type->array_size * _parse_type->element_width;
    _parse_saved_array_size = 0;
    _parse_saved_array_dim_count = 0;
    memset(_parse_saved_array_dims, 0, sizeof(_parse_saved_array_dims));
}

/* ── Subprogram spec helpers (package interface, no body) ── */

static void vhdl_func_spec_enter(const char *name, int is_function) {
    _parse_func_parent = _parse_unit;
    _parse_func = NULL;
    _parse_func_temp = NULL;
    if (!_parse_unit) return;
    _parse_func = uir_add_func_task(_parse_unit, name, is_function);
    if (!_parse_func) return;
    char *lower = parse_strdup_and_lower(name);
    uir_loc_t loc = parse_loc();
    _parse_func_temp = uir_create_design_unit(lower, "vhdl", loc);
    free(lower);
    if (!_parse_func_temp) { _parse_unit = _parse_func_parent; return; }
    _parse_unit = _parse_func_temp;
}

static void vhdl_func_spec_set_return_width(uint32_t width) {
    if (_parse_func) _parse_func->return_width = width;
}

static void vhdl_func_spec_leave(void) {
    if (!_parse_func) {
        _parse_unit = _parse_func_parent;
        _parse_func_parent = NULL;
        return;
    }
    if (_parse_func_temp) {
        for (size_t i = 0; i < _parse_func_temp->port_count; i++) {
            uir_port_t *p = _parse_func_temp->ports[i];
            if (p) uir_add_func_port(_parse_func, p->name, p->direction, p->width);
        }
        uir_destroy_design_unit(_parse_func_temp);
    }
    _parse_unit = _parse_func_parent;
    _parse_func_parent = NULL;
    _parse_func = NULL;
    _parse_func_temp = NULL;
}

/* ── Assert/report helpers ── */

static void vhdl_do_assert(uir_design_unit_t *unit);

static void vhdl_do_report(uir_design_unit_t *unit) {
    /* report stmt: pop message, set NULL condition (fires always), clear severity, call assert */
    _parse_assert_msg = expr_pop();
    _parse_assert_cond = NULL;
    _parse_assert_sev = NULL;
    vhdl_do_assert(unit);
}
/* ── With-select helpers ── */

static void vhdl_do_assert(uir_design_unit_t *unit) {
    if (!unit) { _parse_assert_cond = NULL; _parse_assert_msg = NULL; _parse_assert_sev = NULL; return; }
    uir_node_t *cond = _parse_assert_cond;
    uir_node_t *msg = _parse_assert_msg;
    if (!cond) {
        qsim_bit_vector_t *zero = qsim_bit_vector_alloc(1);
        qsim_bit_set(zero, 0, QSIM_VAL_0);
        cond = (uir_node_t *)uir_make_literal(unit, zero, parse_loc());
    }
    int severity = 0;
    if (_parse_assert_sev) {
        if (strcmp(_parse_assert_sev, "warning") == 0) severity = 1;
        else if (strcmp(_parse_assert_sev, "error") == 0) severity = 2;
        else if (strcmp(_parse_assert_sev, "failure") == 0) severity = 3;
        free(_parse_assert_sev);
        _parse_assert_sev = NULL;
    }
    uir_vhdl_assert_t *a = uir_make_vhdl_assert(unit, cond, msg, severity, parse_loc());
    if (a) {
        if (current_block())
            append_stmt((uir_node_t *)a);
        else
            fprintf(stderr, "WARNING: concurrent assert not supported, discarding\n");
    }
    _parse_assert_cond = NULL;
    _parse_assert_msg = NULL;
}

static void vhdl_select_add_item(void) {
    int n = _expr_sp - _parse_select_item_sp;
    if (n <= 0) return;

    /* First expr at _parse_select_item_sp is the value, rest are patterns */
    uir_node_t *value = _expr_stack[_parse_select_item_sp];
    int pattern_count = n - 1;

    if (pattern_count == 0) {
        /* "when others" — default */
        _parse_select_default_value = value;
    } else {
        if (_parse_select_item_count >= _parse_select_item_cap) {
            size_t new_cap = _parse_select_item_cap ? _parse_select_item_cap * 2 : 8;
            uir_node_t **new_vals = realloc(_parse_select_values, new_cap * sizeof(uir_node_t *));
            uir_node_t ***new_pats = realloc(_parse_select_patterns, new_cap * sizeof(uir_node_t **));
            size_t *new_counts = realloc(_parse_select_pattern_counts, new_cap * sizeof(size_t));
            if (new_vals) _parse_select_values = new_vals;
            if (new_pats) _parse_select_patterns = new_pats;
            if (new_counts) _parse_select_pattern_counts = new_counts;
            _parse_select_item_cap = new_cap;
        }

        uir_node_t **pats = malloc((size_t)pattern_count * sizeof(uir_node_t *));
        if (pats) {
            for (int i = 0; i < pattern_count; i++)
                pats[i] = _expr_stack[_parse_select_item_sp + 1 + i];
        }

        _parse_select_values[_parse_select_item_count] = value;
        _parse_select_patterns[_parse_select_item_count] = pats;
        _parse_select_pattern_counts[_parse_select_item_count] = (size_t)pattern_count;
        _parse_select_item_count++;
    }

    _expr_sp = _parse_select_item_sp;
}

static void vhdl_select_finish(uir_design_unit_t *unit) {
    if (!unit) return;

    /* Determine if concurrent (no active block) or sequential (inside process) */
    int is_concurrent = (current_block() == NULL);

    if (is_concurrent) {
        /* Concurrent: wrap in a process with auto sensitivity */
        uir_process_t *proc = uir_add_process(unit, UIR_PROC_VHDL);
        proc->body = (uir_node_t *)uir_add_block(unit, 1);
        proc->auto_sens = 1;
        push_stmt_block((uir_block_t *)proc->body);
    }

    /* Build case statement */
    uir_case_t *case_node = (uir_case_t *)uir_alloc_node(unit, UIR_CASE, sizeof(uir_case_t), parse_loc());
    case_node->expr = _parse_select_selector;
    case_node->is_wildcard = 1;

    uir_case_item_t **items = NULL;
    size_t item_count = 0;

    if (_parse_select_item_count > 0) {
        items = malloc(_parse_select_item_count * sizeof(uir_case_item_t *));
        if (items) {
            for (size_t i = 0; i < _parse_select_item_count; i++) {
                uir_case_item_t *item = (uir_case_item_t *)uir_alloc_node(unit, UIR_CASE_ITEM, sizeof(uir_case_item_t), parse_loc());
                item->patterns = _parse_select_patterns[i];
                item->pattern_count = _parse_select_pattern_counts[i];

                uir_block_t *block = uir_add_block(unit, 1);
                uir_node_t *lhs = uir_make_ref(unit, _parse_select_target, parse_loc());
                uir_assign_t *assign = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
                assign->lhs = lhs;
                assign->rhs = _parse_select_values[i];
                assign->delay = 1;
                block->stmts = malloc(sizeof(uir_node_t *));
                if (block->stmts) {
                    block->stmts[0] = (uir_node_t *)assign;
                    block->stmt_count = 1;
                }
                item->body = (uir_node_t *)block;
                items[item_count++] = item;
            }
        }
    }
    case_node->items = items;
    case_node->item_count = item_count;

    if (_parse_select_default_value) {
        uir_block_t *db = uir_add_block(unit, 1);
        uir_node_t *lhs = uir_make_ref(unit, _parse_select_target, parse_loc());
        uir_assign_t *assign = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        assign->lhs = lhs;
        assign->rhs = _parse_select_default_value;
        assign->delay = 1;
        db->stmts = malloc(sizeof(uir_node_t *));
        if (db->stmts) {
            db->stmts[0] = (uir_node_t *)assign;
            db->stmt_count = 1;
        }
        case_node->default_item = (uir_node_t *)db;
    }

    append_stmt((uir_node_t *)case_node);

    if (is_concurrent) {
        pop_stmt_block();
    }

    /* Clean up temporary state */
    _parse_select_selector = NULL;
    free(_parse_select_target);
    _parse_select_target = NULL;
    free(_parse_select_values);
    _parse_select_values = NULL;
    free(_parse_select_patterns);
    _parse_select_patterns = NULL;
    free(_parse_select_pattern_counts);
    _parse_select_pattern_counts = NULL;
    _parse_select_item_count = 0;
    _parse_select_item_cap = 0;
    _parse_select_default_value = NULL;
}

/* ── Generate block helpers ── */

static void vhdl_gen_for_enter(uir_design_unit_t *unit) {
    if (!unit) return;

    uir_loc_t loc = {NULL, 0, 0};
    uir_generate_t *gen = uir_add_generate(unit, UIR_GEN_LOOP);
    gen->label = _parse_saved ? strdup(_parse_saved) : NULL;
    gen->genvar_name = _parse_genvar_name ? strdup(_parse_genvar_name) : NULL;
    gen->for_init = _parse_gen_lo;
    gen->for_cond = _parse_gen_hi;
    gen->for_step = NULL;
    gen->for_direction = _parse_gen_direction;
    _parse_current_gen = gen;

    /* Create template unit for body collection */
    uir_design_unit_t *temp = uir_create_design_unit("_gen_body_", "vhdl", loc);
    gen->body_template = temp;

    /* Redirect parsing into temp unit */
    _parse_gen_outer = unit;
    _parse_unit = temp;
}

static void vhdl_gen_transfer_body(uir_generate_t *gen, uir_design_unit_t *temp) {
    if (!gen || !temp) return;
    for (size_t i = 0; i < temp->process_count; i++)
        uir_add_generate_body_item(gen, (uir_node_t *)temp->processes[i]);
    for (size_t i = 0; i < temp->assign_count; i++)
        uir_add_generate_body_item(gen, (uir_node_t *)temp->assigns[i]);
    for (size_t i = 0; i < temp->instance_count; i++)
        uir_add_generate_body_item(gen, (uir_node_t *)temp->instances[i]);
    for (size_t i = 0; i < temp->generate_count; i++)
        uir_add_generate_body_item(gen, (uir_node_t *)temp->generates[i]);
    for (size_t i = 0; i < temp->signal_count; i++)
        uir_add_generate_body_item(gen, (uir_node_t *)temp->signals[i]);
}

static void vhdl_gen_transfer_else_body(uir_generate_t *gen, uir_design_unit_t *temp) {
    if (!gen || !temp) return;
    for (size_t i = 0; i < temp->process_count; i++)
        uir_add_generate_else_body_item(gen, (uir_node_t *)temp->processes[i]);
    for (size_t i = 0; i < temp->assign_count; i++)
        uir_add_generate_else_body_item(gen, (uir_node_t *)temp->assigns[i]);
    for (size_t i = 0; i < temp->instance_count; i++)
        uir_add_generate_else_body_item(gen, (uir_node_t *)temp->instances[i]);
    for (size_t i = 0; i < temp->generate_count; i++)
        uir_add_generate_else_body_item(gen, (uir_node_t *)temp->generates[i]);
    for (size_t i = 0; i < temp->signal_count; i++)
        uir_add_generate_else_body_item(gen, (uir_node_t *)temp->signals[i]);
}

static void vhdl_gen_for_leave(void) {
    uir_generate_t *gen = _parse_current_gen;
    if (gen && gen->body_template) {
        vhdl_gen_transfer_body(gen, gen->body_template);
    }

    if (_parse_gen_outer)
        _parse_unit = _parse_gen_outer;

    _parse_current_gen = NULL;
    _parse_gen_outer = NULL;
    _parse_gen_lo = NULL;
    _parse_gen_hi = NULL;
    _parse_gen_direction = 0;
    free(_parse_genvar_name);
    _parse_genvar_name = NULL;
}

static void vhdl_gen_if_enter(uir_design_unit_t *unit) {
    if (!unit) return;

    uir_loc_t loc = {NULL, 0, 0};
    uir_generate_t *gen = uir_add_generate(unit, UIR_GEN_IF);
    gen->label = _parse_saved ? strdup(_parse_saved) : NULL;
    gen->if_condition = _parse_gen_if_cond;
    _parse_current_gen = gen;

    /* Create template unit for if-body */
    uir_design_unit_t *temp = uir_create_design_unit("_gen_if_body_", "vhdl", loc);
    gen->body_template = temp;

    _parse_gen_outer = unit;
    _parse_unit = temp;
}

/* PEG grammar action: pop condition and enter if-generate (no nested braces in peg rule) */
static void vhdl_gen_if_cond_start(uir_design_unit_t *unit) {
    if (_parse_unit) {
        _parse_gen_if_cond = expr_pop();
        vhdl_gen_if_enter(unit);
    }
}

static void vhdl_gen_if_else_enter(void) {
    uir_generate_t *gen = _parse_current_gen;
    if (!gen) return;

    uir_loc_t loc = {NULL, 0, 0};
    uir_design_unit_t *temp = uir_create_design_unit("_gen_else_body_", "vhdl", loc);
    gen->else_body_template = temp;
    _parse_unit = temp;
}

static void vhdl_gen_if_finish(void) {
    uir_generate_t *gen = _parse_current_gen;
    if (!gen) return;

    if (gen->body_template) {
        vhdl_gen_transfer_body(gen, gen->body_template);
    }
    if (gen->else_body_template) {
        vhdl_gen_transfer_else_body(gen, gen->else_body_template);
    }

    if (_parse_gen_outer)
        _parse_unit = _parse_gen_outer;

    _parse_current_gen = NULL;
    _parse_gen_outer = NULL;
    _parse_gen_if_cond = NULL;
}

/* ── Assignment helpers ── */

static void do_signal_assign(uir_design_unit_t *unit, const char *target) {
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs = uir_make_ref(unit, target, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 1; /* signal assignment = non-blocking */
            append_stmt((uir_node_t *)a);
        }
    }
}

static void do_variable_assign(uir_design_unit_t *unit, const char *target) {
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs = uir_make_ref(unit, target, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
        if (a) {
            a->lhs = lhs;
            a->rhs = rhs;
            a->delay = 0; /* variable assignment = blocking */
            append_stmt((uir_node_t *)a);
        }
    }
}

/* Helper wrappers for grammar actions (no nested braces allowed in peg actions) */
static void do_signal_assign_and_free(uir_design_unit_t *unit) {
    if (unit && _parse_saved) {
        do_signal_assign(unit, _parse_saved);
        free(_parse_saved);
        _parse_saved = NULL;
    }
}

static void do_variable_assign_and_free(uir_design_unit_t *unit) {
    if (unit && _parse_saved) {
        do_variable_assign(unit, _parse_saved);
        free(_parse_saved);
        _parse_saved = NULL;
    }
}

static void do_concurrent_signal_assign(uir_design_unit_t *unit) {
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs = uir_make_ref(unit, _parse_saved, parse_loc());
    if (lhs && rhs && unit) {
        uir_assign_t *a = uir_add_assign(unit);
        a->lhs = lhs;
        a->rhs = rhs;
        a->delay = 1; /* VHDL concurrent signal assignment = delta delay */
    }
    free(_parse_saved);
    _parse_saved = NULL;
}

/* Extended signal assign with optional LHS part-select (for dmem_tmp(a downto b) <= val) */
static void do_ext_sig_assign(uir_design_unit_t *unit) {
    if (!unit || !_parse_saved) return;
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs;
    int is_arr = 0;
    if (_parse_target_hi) {
        if (_parse_target_hi == _parse_target_lo && (is_arr = is_signal_array(unit, _parse_saved)))
            lhs = uir_make_ref_index(unit, _parse_saved, _parse_target_hi, parse_loc());
        else
            lhs = uir_make_ref_part_select(unit, _parse_saved, _parse_target_hi, _parse_target_lo, parse_loc());
    } else {
        lhs = uir_make_ref(unit, _parse_saved, parse_loc());
    }
    if (lhs && rhs) {
        if (current_block()) {
            uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
            if (a) { a->lhs = lhs; a->rhs = rhs; a->delay = 1; append_stmt((uir_node_t *)a); }
        } else {
            uir_assign_t *a = uir_add_assign(unit);
            if (a) { a->lhs = lhs; a->rhs = rhs; a->delay = 1; }
        }
    }
    _parse_target_hi = NULL;
    _parse_target_lo = NULL;
}

/* Extended variable assign with optional LHS part-select */
static void do_ext_var_assign(uir_design_unit_t *unit) {
    if (!unit || !_parse_saved) return;
    uir_node_t *rhs = expr_pop();
    uir_node_t *lhs;
    if (_parse_target_hi) {
        if (_parse_target_hi == _parse_target_lo && is_signal_array(unit, _parse_saved))
            lhs = uir_make_ref_index(unit, _parse_saved, _parse_target_hi, parse_loc());
        else
            lhs = uir_make_ref_part_select(unit, _parse_saved, _parse_target_hi, _parse_target_lo, parse_loc());
    } else
        lhs = uir_make_ref(unit, _parse_saved, parse_loc());
    if (lhs && rhs) {
        if (current_block()) {
            uir_assign_t *a = (uir_assign_t *)uir_alloc_node(unit, UIR_ASSIGN, sizeof(uir_assign_t), parse_loc());
            if (a) { a->lhs = lhs; a->rhs = rhs; a->delay = 0; append_stmt((uir_node_t *)a); }
        } else {
            uir_assign_t *a = uir_add_assign(unit);
            if (a) { a->lhs = lhs; a->rhs = rhs; a->delay = 0; }
        }
    }
    _parse_target_hi = NULL;
    _parse_target_lo = NULL;
}

/* Port map connection for component instantiation: pop expr and connect to last instance */
static void vhdl_port_map_connect(const char *formal_name) {
    uir_node_t *actual = expr_pop();
    if (_parse_unit && _parse_unit->instance_count > 0 && formal_name && actual) {
        uir_add_connection(
            _parse_unit->instances[_parse_unit->instance_count - 1],
            formal_name, actual);
    }
}

/* Port map open (unconnected): connect NULL to leave port unbound */
static void vhdl_port_map_open(const char *formal_name) {
    if (_parse_unit && _parse_unit->instance_count > 0 && formal_name) {
        uir_add_connection(
            _parse_unit->instances[_parse_unit->instance_count - 1],
            formal_name, NULL);
    }
}


static void do_vhdl_binop(uir_design_unit_t *unit, uir_binary_op_t op) {
    uir_node_t *b = expr_pop();
    uir_node_t *a = expr_pop();
    if (a && b) {
        /* VHDL concatenation: a & b means a is MSB, b is LSB.
         * UIR_OP_CONCAT puts left operand at LSB, right at MSB.
         * Swap so VHDL semantics are preserved. */
        if (op == UIR_OP_CONCAT) {
            uir_node_t *tmp = a; a = b; b = tmp;
        }
        expr_push((uir_node_t *)uir_make_binary(unit, op, a, b, parse_loc()));
    }
}

static void do_vhdl_unop(uir_design_unit_t *unit, uir_unary_op_t op) {
    uir_node_t *a = expr_pop();
    if (a)
        expr_push((uir_node_t *)uir_make_unary(unit, op, a, parse_loc()));
}

/* (others => expr) aggregate: pop element expr, push UIR_OP_OTHERS fill node */
static void vhdl_do_others_aggregate(uir_design_unit_t *unit) {
    uir_node_t *elem = expr_pop();
    if (elem && unit)
        expr_push((uir_node_t *)uir_make_unary(unit, UIR_OP_OTHERS, elem, parse_loc()));
}

/* (N downto M => expr) / (N to M => expr) range aggregate.
 * Pops hi, lo, elem; computes width = |hi - lo| + 1; creates a literal filled with elem's value.
 * Fallback to UIR_OP_OTHERS if bounds are not known at parse time. */
static int lit_to_u64(uir_node_t *n, uint64_t *out);
static void vhdl_do_range_aggregate(uir_design_unit_t *unit) {
    uir_node_t *elem = expr_pop();
    uir_node_t *lo = expr_pop();
    uir_node_t *hi = expr_pop();
    if (!unit || !elem || !lo || !hi) {
        if (elem) expr_push(elem);
        return;
    }
    uint64_t hi_val = 0, lo_val = 0;
    if (lit_to_u64(hi, &hi_val) && lit_to_u64(lo, &lo_val)) {
        uint64_t w = (hi_val >= lo_val) ? (hi_val - lo_val + 1) : (lo_val - hi_val + 1);
        if (w > 0 && w <= 4096) {
            qsim_bit_vector_t *bv = qsim_bit_vector_alloc((uint32_t)w);
            if (bv) {
                qsim_value_t fill = QSIM_VAL_0;
                if (elem->kind == UIR_LITERAL) {
                    uir_literal_t *lit = (uir_literal_t *)elem;
                    if (lit->value && lit->value->width > 0)
                        fill = qsim_bit_get(lit->value, 0);
                }
                for (uint32_t i = 0; i < w; i++)
                    qsim_bit_set(bv, i, fill);
                expr_push((uir_node_t *)uir_make_literal(unit, bv, parse_loc()));
                return;
            }
        }
    }
    /* Fallback: use OTHERS fill */
    expr_push(elem);
    if (unit)
        expr_push((uir_node_t *)uir_make_unary(unit, UIR_OP_OTHERS, elem, parse_loc()));
}

/* when-else conditional: a when cond else b → UIR_CND */
static void vhdl_do_when_else(uir_design_unit_t *unit) {
    uir_node_t *false_expr = expr_pop();  /* else branch */
    uir_node_t *condition  = expr_pop();  /* condition after WHEN */
    uir_node_t *true_expr  = expr_pop();  /* then branch before WHEN */
    if (true_expr && condition && false_expr && unit) {
        uir_cond_t *cond = (uir_cond_t *)uir_alloc_node(unit, UIR_COND, sizeof(uir_cond_t), parse_loc());
        if (cond) {
            cond->condition = condition;
            cond->then_expr = true_expr;
            cond->else_expr = false_expr;
            expr_push((uir_node_t *)cond);
        }
    }
}

/* ── Function/procedure call helpers ── */

static void vhdl_call_init(const char *name) {
    strncpy(_parse_call_name, name, sizeof(_parse_call_name) - 1);
    _parse_call_name[sizeof(_parse_call_name) - 1] = '\0';
    _parse_call_arg_count = 0;
}

static void vhdl_save_call_arg(void) {
    uir_node_t *val = expr_pop();
    if (!_parse_unit) return;
    if (_parse_call_arg_count >= _parse_call_arg_cap) {
        size_t new_cap = _parse_call_arg_cap ? _parse_call_arg_cap * 2 : 8;
        uir_node_t **new_args = realloc(_parse_call_args, new_cap * sizeof(uir_node_t *));
        if (!new_args) return;
        _parse_call_args = new_args;
        _parse_call_arg_cap = new_cap;
    }
    _parse_call_args[_parse_call_arg_count++] = val;
}

static void vhdl_proc_call_begin(const char *name) {
    vhdl_call_init(name);
}

static void vhdl_proc_call_finish(void) {
    if (!_parse_unit) { _parse_call_arg_count = 0; return; }
    uir_func_call_t *tc = uir_make_func_call(_parse_unit, _parse_call_name,
        _parse_call_args, _parse_call_arg_count, parse_loc());
    if (tc) {
        tc->base.kind = UIR_TASK_ENABLE;
        append_stmt((uir_node_t *)tc);
    }
    _parse_call_arg_count = 0;
}

/* Combined rule helpers for primary_expr ID_OR_KW LPAREN ... RPAREN */

static int is_numeric_std_builtin(const char *name) {
    if (!name) return 0;
    return (strcmp(name, "unsigned") == 0 ||
            strcmp(name, "signed") == 0 ||
            strcmp(name, "to_integer") == 0 ||
            strcmp(name, "to_unsigned") == 0 ||
            strcmp(name, "to_signed") == 0 ||
            strcmp(name, "shift_left") == 0 ||
            strcmp(name, "shift_right") == 0);
}

static int is_textio_builtin(const char *name) {
    if (!name) return 0;
    return (strcmp(name, "endfile") == 0 ||
            strcmp(name, "readline") == 0 ||
            strcmp(name, "writeline") == 0);
}

static void vhdl_do_slice_ref2(void) {
    uir_node_t *lo = expr_pop();
    uir_node_t *hi = expr_pop();
    const char *name = vhdl_pop_call_name();
    expr_push(uir_make_ref_part_select(_parse_unit, name, hi, lo, parse_loc()));
}

/* Extract uint64 from a UIR literal node. Returns 0 if not a literal or has X/Z. */
static int lit_to_u64(uir_node_t *n, uint64_t *out) {
    if (!n || n->kind != UIR_LITERAL) return 0;
    uir_literal_t *lit = (uir_literal_t *)n;
    if (!lit->value) return 0;
    uint64_t val = 0;
    for (uint32_t i = 0; i < lit->value->width; i++) {
        qsim_value_t b = qsim_bit_get(lit->value, i);
        if (b.state == QSIM_1) val |= (1ULL << i);
        else if (b.state != QSIM_0) return 0;
    }
    *out = val;
    return 1;
}

/* Handle double-slice: name(hi1 downto lo1)(hi2 downto lo2)
 * Pops hi2, lo2 then the first slice ref, computes combined range. */
static void vhdl_do_dslice_adjust(void) {
    uir_node_t *lo2 = expr_pop();
    uir_node_t *hi2 = expr_pop();
    uir_node_t *orig = expr_pop();
    if (!orig || orig->kind != UIR_REF || !hi2 || !lo2) {
        if (orig) expr_push(orig);
        return;
    }
    uir_ref_t *ref = (uir_ref_t *)orig;
    uint64_t p_lo = 0, lo2_val = 0, hi2_val = 0;
    int has_lo = lit_to_u64(ref->part_lo, &p_lo);
    int has_lo2 = lit_to_u64(lo2, &lo2_val);
    int has_hi2 = lit_to_u64(hi2, &hi2_val);
    if (has_lo && has_lo2) {
        uint64_t new_lo = p_lo + lo2_val;
        uint64_t new_hi = has_hi2 ? (p_lo + hi2_val) : new_lo;
        qsim_bit_vector_t *bv_hi = qsim_bit_vector_alloc(32);
        qsim_bit_vector_t *bv_lo = qsim_bit_vector_alloc(32);
        if (bv_hi && bv_lo) {
            for (uint32_t i = 0; i < 32; i++) {
                qsim_bit_set(bv_hi, i, (new_hi >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                qsim_bit_set(bv_lo, i, (new_lo >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            }
            uir_literal_t *nhi = uir_make_literal(_parse_unit, bv_hi, parse_loc());
            uir_literal_t *nlo = uir_make_literal(_parse_unit, bv_lo, parse_loc());
            if (nhi && nlo)
                expr_push(uir_make_ref_part_select(_parse_unit, ref->name,
                    (uir_node_t *)nhi, (uir_node_t *)nlo, parse_loc()));
            else {
                if (nhi) qsim_bit_vector_free(bv_hi);
                if (nlo) qsim_bit_vector_free(bv_lo);
                expr_push(orig);
            }
        } else {
            if (bv_hi) qsim_bit_vector_free(bv_hi);
            if (bv_lo) qsim_bit_vector_free(bv_lo);
            expr_push(orig);
        }
    } else {
        expr_push(orig);
    }
}

static void vhdl_do_idx_ref2(void) {
    const char *name = vhdl_pop_call_name();
    int saved_call_sp = _parse_call_saved_sp;
    if (is_numeric_std_builtin(name) || vhdl_lookup_builtin_func(name) || is_textio_builtin(name)) {
        /* Treat as builtin function: create func_call instead of part-select */
        _parse_call_saved_sp = _expr_sp;
        int arg_count = 1;
        uir_node_t **args = malloc((size_t)arg_count * sizeof(uir_node_t *));
        if (!args) { _parse_call_saved_sp = saved_call_sp; expr_push(uir_make_ref(_parse_unit, name, parse_loc())); return; }
        args[0] = expr_pop();
        uir_func_call_t *fc = uir_make_func_call(_parse_unit, name,
            args, (size_t)arg_count, parse_loc());
        free(args);
        _parse_call_saved_sp = saved_call_sp;
        if (fc)
            expr_push((uir_node_t *)fc);
        else
            expr_push(uir_make_ref(_parse_unit, name, parse_loc()));
        return;
    }
    uir_node_t *idx = expr_pop();
    if (_parse_unit && is_signal_array(_parse_unit, name))
        expr_push(uir_make_ref_index(_parse_unit, name, idx, parse_loc()));
    else
        expr_push(uir_make_ref_part_select(_parse_unit, name, idx, idx, parse_loc()));
    _parse_call_saved_sp = saved_call_sp;
}

static void vhdl_do_func_call(void) {
    const char *name = vhdl_pop_call_name();
    int arg_count = _expr_sp - _parse_call_saved_sp;
    if (arg_count <= 0) {
        expr_push(uir_make_ref(_parse_unit, name, parse_loc()));
        return;
    }

    /* Check if this is a multi-dimensional array index */
    if (_parse_unit) {
        uir_node_t *ns = uir_find_signal(_parse_unit, name);
        size_t array_dim_count = 0;
        if (ns) {
            if (ns->kind == UIR_SIGNAL) {
                array_dim_count = ((uir_signal_t *)ns)->array_dim_count;
            } else if (ns->kind == UIR_PORT) {
                array_dim_count = ((uir_port_t *)ns)->array_dim_count;
            }
        }
        if (array_dim_count > 1 && (size_t)arg_count == array_dim_count) {
                uir_node_t **indices = malloc((size_t)arg_count * sizeof(uir_node_t *));
                if (!indices) return;
                for (int i = 0; i < arg_count; i++)
                    indices[i] = _expr_stack[_parse_call_saved_sp + i];
                _expr_sp = _parse_call_saved_sp;
                expr_push(uir_make_ref_multi_index(_parse_unit, name, indices, (size_t)arg_count, parse_loc()));
                free(indices);
                return;
            }
        }

    uir_node_t **args = malloc((size_t)arg_count * sizeof(uir_node_t *));
    if (!args) return;
    for (int i = 0; i < arg_count; i++)
        args[i] = _expr_stack[_parse_call_saved_sp + i];
    _expr_sp = _parse_call_saved_sp;
    uir_func_call_t *fc = uir_make_func_call(_parse_unit, name,
        args, (size_t)arg_count, parse_loc());
    free(args);
    if (fc)
        expr_push((uir_node_t *)fc);
}

/* ── Selected-name helpers (a.b.c, ieee.std_logic_1164.rising_edge) ── */

static void vhdl_selname_init(const char *first) {
    free(_parse_selname_buf);
    _parse_selname_buf = parse_strdup(first);
}

static void vhdl_selname_extend(const char *part) {
    if (!_parse_selname_buf) return;
    size_t old = strlen(_parse_selname_buf);
    size_t plen = strlen(part);
    char *new_buf = realloc(_parse_selname_buf, old + 1 + plen + 1);
    if (!new_buf) return;
    _parse_selname_buf = new_buf;
    _parse_selname_buf[old] = '.';
    memcpy(_parse_selname_buf + old + 1, part, plen + 1);
}

static void vhdl_selname_cleanup(void) {
    free(_parse_selname_buf);
    _parse_selname_buf = NULL;
}

static void vhdl_do_selname_ref(void) {
    if (_parse_unit && _parse_selname_buf)
        expr_push(uir_make_ref(_parse_unit, _parse_selname_buf, parse_loc()));
    vhdl_selname_cleanup();
}

static void vhdl_do_selname_slice_ref(void) {
    const char *name = vhdl_pop_call_name();
    if (_parse_unit && name && name[0]) {
        uir_node_t *lo = expr_pop();
        uir_node_t *hi = expr_pop();
        expr_push(uir_make_ref_part_select(_parse_unit, name, hi, lo, parse_loc()));
    }
}

static void vhdl_do_selname_func_call(void) {
    const char *name = vhdl_pop_call_name();
    if (_parse_unit && name && name[0]) {
        int arg_count = _expr_sp - _parse_call_saved_sp;
        if (arg_count > 0) {
            uir_node_t **args = malloc((size_t)arg_count * sizeof(uir_node_t *));
            if (args) {
                for (int i = 0; i < arg_count; i++)
                    args[i] = _expr_stack[_parse_call_saved_sp + i];
                _expr_sp = _parse_call_saved_sp;
                uir_func_call_t *fc = uir_make_func_call(_parse_unit, name,
                    args, (size_t)arg_count, parse_loc());
                free(args);
                if (fc)
                    expr_push((uir_node_t *)fc);
            }
        }
    }
}

/* ── Library/use helpers ── */

static void store_library_name(const char *name) {
    char **nn = realloc(_parse_library_names, (_parse_library_count + 1) * sizeof(char *));
    if (!nn) return;
    _parse_library_names = nn;
    _parse_library_names[_parse_library_count++] = parse_strdup(name);
}

static void store_use_clause_all(void) {
    if (!_parse_saved || !_parse_saved2) return;
    size_t len = strlen(_parse_saved) + 1 + strlen(_parse_saved2) + 4 + 1;
    char *clause = malloc(len);
    if (!clause) return;
    snprintf(clause, len, "%s.%s.all", _parse_saved, _parse_saved2);
    char **nc = realloc(_parse_use_clauses, (_parse_use_count + 1) * sizeof(char *));
    if (!nc) { free(clause); return; }
    _parse_use_clauses = nc;
    _parse_use_clauses[_parse_use_count++] = clause;
}

static void store_use_clause2(void) {
    /* "use lib.pkg;" — store "lib.pkg" */
    if (!_parse_saved || !_parse_saved2) return;
    size_t len = strlen(_parse_saved) + 1 + strlen(_parse_saved2) + 1;
    char *clause = malloc(len);
    if (!clause) return;
    snprintf(clause, len, "%s.%s", _parse_saved, _parse_saved2);
    char **nc = realloc(_parse_use_clauses, (_parse_use_count + 1) * sizeof(char *));
    if (!nc) { free(clause); return; }
    _parse_use_clauses = nc;
    _parse_use_clauses[_parse_use_count++] = clause;
}

static void store_use_clause3(void) {
    /* "use lib.pkg.all;" or "use lib.pkg.item;" — store "lib.pkg.all" */
    if (!_parse_saved || !_parse_saved2 || !_parse_saved3) return;
    size_t len = strlen(_parse_saved) + 1 + strlen(_parse_saved2) + 1 + strlen(_parse_saved3) + 1;
    char *clause = malloc(len);
    if (!clause) return;
    snprintf(clause, len, "%s.%s.%s", _parse_saved, _parse_saved2, _parse_saved3);
    char **nc = realloc(_parse_use_clauses, (_parse_use_count + 1) * sizeof(char *));
    if (!nc) { free(clause); return; }
    _parse_use_clauses = nc;
    _parse_use_clauses[_parse_use_count++] = clause;
}

/* ── Number literal parsing ── */

static uir_node_t *parse_vhdl_number(uir_design_unit_t *unit, const char *text, uir_loc_t loc) {
    uint32_t width = 32;
    unsigned long val = 0;
    int base = 10;

    /* Check for based literal: <base>#<digits># */
    const char *hash1 = strchr(text, '#');
    if (hash1) {
        /* Parse base */
        char base_str[16];
        size_t blen = (size_t)(hash1 - text);
        if (blen < sizeof(base_str)) {
            memcpy(base_str, text, blen);
            base_str[blen] = '\0';
            base = (int)strtoul(base_str, NULL, 10);
        }
        const char *digits = hash1 + 1;
        const char *hash2 = strchr(digits, '#');
        if (hash2) {
            /* Parse digits in that base */
            const char *p = digits;
            while (p < hash2) {
                char c = (char)tolower((unsigned char)*p);
                if (c == '_') { p++; continue; }
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                if (digit >= 0 && digit < base) {
                    val = val * (unsigned long)base + (unsigned long)digit;
                }
                p++;
            }
        }
    } else {
        val = strtoul(text, NULL, 10);
    }

    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;
    uint64_t v64 = (uint64_t)val;
    for (uint32_t i = 0; i < width; i++)
        qsim_bit_set(bv, i, (i < 64 && ((v64 >> i) & 1)) ? QSIM_VAL_1 : QSIM_VAL_0);

    return (uir_node_t *)uir_make_literal(unit, bv, loc);
}

/* Parse VHDL bit string literal: B"1010", X"FF", O"377", UB"1010" etc. */
static uir_node_t *parse_vhdl_bit_string(uir_design_unit_t *unit, const char *text, uir_loc_t loc) {
    uint32_t width = 0;
    int base = 2;

    /* Check for base prefix: [UBXO]?"string" */
    const char *p = text;
    char prefix = (char)toupper((unsigned char)*p);
    if (prefix == 'X' || prefix == 'B' || prefix == 'O' || prefix == 'U') {
        if (prefix == 'X') base = 16;
        else if (prefix == 'O') base = 8;
        else if (prefix == 'B') base = 2;
        else base = 2; /* U = unresolved */
        p++;
    }

    /* Skip opening quote */
    if (*p == '"') p++;

    /* Count digit string length */
    const char *start = p;
    size_t digit_len = 0;
    while (*p && *p != '"') {
        if (*p != '_') digit_len++;
        p++;
    }

    /* Compute width from digits and base */
    if (base == 16) width = (uint32_t)digit_len * 4;
    else if (base == 8) width = (uint32_t)digit_len * 3;
    else width = (uint32_t)digit_len;

    if (width == 0) width = 1;

    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;

    /* Parse digits right-to-left */
    int bit_pos = 0;
    const char *q = p - 1; /* p is at closing quote, work backwards */
    if (*q == '"') q--;
    while (q >= start && bit_pos < (int)width) {
        char c = (char)tolower((unsigned char)*q);
        if (c == '_') { q--; continue; }

        if (base == 2) {
            if (c == '1') qsim_bit_set(bv, bit_pos, QSIM_VAL_1);
            else qsim_bit_set(bv, bit_pos, QSIM_VAL_0);
            bit_pos++;
        } else if (base == 16) {
            int hval = 0;
            if (c >= '0' && c <= '9') hval = c - '0';
            else if (c >= 'a' && c <= 'f') hval = c - 'a' + 10;
            for (int b = 0; b < 4 && bit_pos + b < (int)width; b++)
                qsim_bit_set(bv, bit_pos + b, (hval >> b) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            bit_pos += 4;
        } else if (base == 8) {
            int oval = 0;
            if (c >= '0' && c <= '7') oval = c - '0';
            for (int b = 0; b < 3 && bit_pos + b < (int)width; b++)
                qsim_bit_set(bv, bit_pos + b, (oval >> b) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            bit_pos += 3;
        }
        q--;
    }

    return (uir_node_t *)uir_make_literal(unit, bv, loc);
}

/* Parse VHDL character literal: '0', '1', 'X', etc. */
static uir_node_t *parse_vhdl_char_literal(uir_design_unit_t *unit, const char *text, uir_loc_t loc) {
    uint32_t width = 1;
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;

    char c = text[0]; /* text is the captured char (PEG: "'" < . > "'" — yytext is just the inner char) */
    if (c == '0') qsim_bit_set(bv, 0, QSIM_VAL_0);
    else if (c == '1') qsim_bit_set(bv, 0, QSIM_VAL_1);
    else if (c == 'X' || c == 'x') qsim_bit_set(bv, 0, QSIM_VAL_X);
    else if (c == 'Z' || c == 'z') qsim_bit_set(bv, 0, QSIM_VAL_Z);
    else qsim_bit_set(bv, 0, QSIM_VAL_X); /* unknown */

    return (uir_node_t *)uir_make_literal(unit, bv, loc);
}

/* Parse VHDL boolean literal: true/false */
static uir_node_t *parse_vhdl_boolean(uir_design_unit_t *unit, const char *text, uir_loc_t loc) {
    uint32_t width = 1;
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;

    int val = 0;
    if (text[0] == 't' || text[0] == 'T') val = 1;
    qsim_bit_set(bv, 0, val ? QSIM_VAL_1 : QSIM_VAL_0);

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

static int extract_uint32_from_node(uir_node_t *node, uint32_t *out) {
    if (!node || node->kind != UIR_LITERAL) return 0;
    uir_literal_t *lit = (uir_literal_t *)node;
    if (!lit->value) return 0;
    uint64_t val = 0;
    for (uint32_t i = 0; i < lit->value->width && i < 32; i++) {
        qsim_value_t bit = qsim_bit_get(lit->value, i);
        if (bit.state == QSIM_X || bit.state == QSIM_Z) return 0;
        if (bit.state == QSIM_1) val |= (1ULL << i);
    }
    *out = (uint32_t)val;
    return 1;
}

/* Finish constant_decl: pop expr value, store constant(s) for each ID. */
static void finish_constant_decls(void) {
    if (!_parse_unit) { id_list_clear(); return; }
    uir_node_t *val_node = expr_pop();
    uint32_t width = (uint32_t)_parse_saved_range_width;
    uint64_t value = 0;
    if (val_node && val_node->kind == UIR_LITERAL) {
        uir_literal_t *lit = (uir_literal_t *)val_node;
        if (lit->value) {
            for (uint32_t i = 0; i < lit->value->width && i < 64; i++) {
                qsim_value_t bit = qsim_bit_get(lit->value, i);
                if (bit.state == QSIM_1) value |= (1ULL << i);
            }
        }
    }
    for (size_t i = 0; i < _parse_id_count; i++)
        uir_add_vhdl_constant(_parse_unit, _parse_id_list[i], width, value);
    id_list_clear();
}

/* Pop two range-bound expressions and set _parse_range_width. */
static void vhdl_compute_range(void) {
    uir_node_t *upper = expr_pop();
    uir_node_t *lower = expr_pop();
    uint32_t lo = 0, hi = 0;
    if (extract_uint32_from_node(lower, &lo) && extract_uint32_from_node(upper, &hi)) {
        _parse_range_width = (hi >= lo) ? (int)(hi - lo + 1) : (int)(lo - hi + 1);
    } else {
        _parse_range_width = 32;
    }
}

/* ================================================================
 * Include generated PEG parser
 * Rename globals to avoid linker conflicts with verilog_parser
 * ================================================================ */

#define YYPARSE       vhdlyyparse
#define YYPARSEFROM   vhdlyyparsefrom
#define YYPARSEFROM_R vhdlyyparsefrom_r
#define YYRELEASE     vhdlyyrelease
#define yyctx         vhldyyctx
#define _yyctx        _vhldyyctx
#include "vhdl_grammar.peg.c"
#undef YYPARSE
#undef YYPARSEFROM
#undef YYPARSEFROM_R
#undef YYRELEASE
#undef yyctx
#undef _yyctx

/* Release internal PEG buffers (call before each parse to reset state) */
static void release_peg(void) {
    vhdlyyrelease(vhldyyctx);
}

/* ================================================================
 * Public API
 * ================================================================ */

vhdl_parse_result_t vhdl_parse(const char *filename, const char *source, size_t length)
{
    vhdl_parse_result_t result;
    memset(&result, 0, sizeof(result));

    if (!source || length == 0) {
        result.success = 0;
        return result;
    }

    /* Create lowercased copy of source (VHDL is case-insensitive) */
    char *lower_source = malloc(length + 1);
    if (!lower_source) {
        result.success = 0;
        return result;
    }
    for (size_t i = 0; i < length; i++)
        lower_source[i] = (char)tolower((unsigned char)source[i]);
    lower_source[length] = '\0';

    /* Release PEG internal state from any previous parse */
    release_peg();

    /* Set up parser context */
    _parse_filename = filename;
    _parse_source = lower_source;
    _parse_source_len = length;
    _parse_source_pos = 0;
    _parse_unit = NULL;
    _parse_ok = 0;
    _parse_saved = NULL;
    _parse_process = NULL;
    _parse_sens_names = NULL;
    _parse_sens_count = 0;
    _parse_vhdl_process_all = 0;
    _expr_sp = 0;
    _parse_block_sp = -1;
    _parse_had_else = 0;
    _vhdl_elsif_sp = -1;
    _parse_then_block = NULL;
    _parse_else_block = NULL;
    _parse_case_saved_sp = 0;
    _parse_case_items = NULL;
    _parse_case_item_count = 0;
    _parse_case_item_cap = 0;
    _parse_case_default = NULL;
    _parse_case_item_patterns = NULL;
    _parse_case_item_pattern_count = 0;
    _parse_case_item_body = NULL;
    _parse_case_default_body = NULL;
    _parse_id_count = 0;
    _parse_port_dir = UIR_PORT_IN;
    _parse_saved_range_width = 1;
    _parse_range_width = 32;
    _parse_saved_array_size = 0;
    _parse_saved_array_dim_count = 0;
    memset(_parse_saved_array_dims, 0, sizeof(_parse_saved_array_dims));
    _parse_target_hi = NULL;
    _parse_target_lo = NULL;
    _parse_wait_condition = NULL;
    _parse_wait_delay = NULL;
    _parse_wait_sens_list = NULL;
    _parse_wait_sens_count = 0;
    _parse_ret_expr = NULL;
    _parse_assert_cond = NULL;
    _parse_assert_msg = NULL;
    _parse_assert_sev = NULL;
    _parse_select_saved_sp = 0;
    _parse_select_selector = NULL;
    _parse_select_target = NULL;
    _parse_select_item_sp = 0;
    _parse_select_values = NULL;
    _parse_select_patterns = NULL;
    _parse_select_pattern_counts = NULL;
    _parse_select_item_count = 0;
    _parse_select_item_cap = 0;
    _parse_select_default_value = NULL;
    _parse_file_mode = NULL;
    _parse_file_name_str = NULL;
    _parse_genvar_name = NULL;
    _parse_gen_lo = NULL;
    _parse_gen_hi = NULL;
    _parse_gen_direction = 0;
    _parse_gen_outer = NULL;
    _parse_current_gen = NULL;
    _parse_gen_if_cond = NULL;
    _parse_func_parent = NULL;
    _parse_func_temp = NULL;
    _parse_func = NULL;
    _parse_comp_parent = NULL;
    _parse_comp_temp = NULL;
    _parse_comp = NULL;
    _parse_config_name = NULL;
    _parse_config_entity = NULL;
    _parse_alias_name = NULL;
    _parse_attr_name = NULL;
    _parse_attr_target = NULL;
    _parse_attr_class = NULL;
    _parse_group_name = NULL;
    _parse_group = NULL;
    _parse_type = NULL;
    _parse_call_name[0] = '\0';
    _parse_call_arg_count = 0;
    _parse_call_saved_sp = 0;

    /* Reset selected-name state */
    vhdl_selname_cleanup();

    /* Reset library/use state */
    _parse_library_names = NULL;
    _parse_library_count = 0;
    _parse_use_clauses = NULL;
    _parse_use_count = 0;
    _parse_call_saved_sp = 0;
    _parse_call_name[0] = '\0';
    _parse_saved2 = NULL;
    _parse_saved3 = NULL;

    /* Run the PEG parser */
    int ok = vhdlyyparsefrom(yy_design_file);

    if (ok && _parse_ok && _parse_unit) {
        /* Transfer library/use arrays to design unit */
        _parse_unit->library_names = _parse_library_names;
        _parse_unit->library_count = _parse_library_count;
        _parse_unit->use_clauses = _parse_use_clauses;
        _parse_unit->use_count = _parse_use_count;
        _parse_library_names = NULL;
        _parse_use_clauses = NULL;
        result.unit = _parse_unit;
        result.success = 1;
    } else {
        result.success = 0;
        result.error_count = 1;
        size_t ctx_pos = _parse_source_pos;
        /* Convert byte offset to line/column (use original source, not lowercased) */
        const char *orig_source = source ? source : _parse_source;
        size_t orig_len = length;
        if (!orig_source) { orig_source = _parse_source; orig_len = _parse_source_len; }
        int line = 1, col = 1;
        for (size_t i = 0; i < ctx_pos && i < orig_len; i++) {
            if (orig_source[i] == '\n') { line++; col = 1; }
            else col++;
        }
        if (ctx_pos > 20) ctx_pos -= 20; else ctx_pos = 0;
        snprintf(result.errors[0].message, sizeof(result.errors[0].message),
                 "Parse failed near '%.30s'", orig_source + ctx_pos);
        result.errors[0].line = line;
        result.errors[0].column = col;
    }

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

    /* On failure, free any accumulated library/use arrays */
    if (!result.success) {
        for (size_t li = 0; li < _parse_library_count; li++)
            free(_parse_library_names[li]);
        free(_parse_library_names);
        _parse_library_names = NULL;
        _parse_library_count = 0;
        for (size_t ui = 0; ui < _parse_use_count; ui++)
            free(_parse_use_clauses[ui]);
        free(_parse_use_clauses);
        _parse_use_clauses = NULL;
        _parse_use_count = 0;
    }

    free(lower_source);

    return result;
}

vhdl_parse_result_t vhdl_parse_file(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        vhdl_parse_result_t result;
        memset(&result, 0, sizeof(result));
        result.success = 0;
        return result;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        vhdl_parse_result_t result;
        memset(&result, 0, sizeof(result));
        result.success = 0;
        return result;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return vhdl_parse(filename, "", 0); }

    size_t nread = fread(buf, 1, (size_t)len, f);
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

    vhdl_parse_result_t result = vhdl_parse(filename, src_start, src_len);
    free(buf);
    return result;
}
