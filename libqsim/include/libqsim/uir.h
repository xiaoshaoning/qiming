#ifndef LIBDSIM_UIR_H
#define LIBDSIM_UIR_H

#include "libqsim/value.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Node kinds === */

typedef enum uir_node_kind {
    UIR_DESIGN_UNIT     = 1,
    UIR_PORT            = 2,
    UIR_SIGNAL          = 3,
    UIR_PROCESS         = 4,
    UIR_ASSIGN          = 5,
    UIR_EXPR_BINARY     = 6,
    UIR_EXPR_UNARY      = 7,
    UIR_LITERAL         = 8,
    UIR_IF              = 9,
    UIR_CASE            = 10,
    UIR_CASE_ITEM       = 11,
    UIR_LOOP            = 12,
    UIR_BLOCK           = 13,
    UIR_INSTANCE        = 14,
    UIR_GATE            = 15,
    UIR_GENERATE        = 16,
    UIR_ALWAYS          = 17,
    UIR_INITIAL         = 18,
    UIR_PROCESS_VHDL    = 19,
    UIR_REF             = 20,
    UIR_COND            = 21,
    UIR_SYS_TASK        = 22,
    UIR_FUNC_DEF         = 23,   /* function definition */
    UIR_TASK_DEF         = 24,   /* task definition */
    UIR_FUNC_CALL        = 25,   /* function call expression */
    UIR_TASK_ENABLE      = 26,   /* task enable statement */
    UIR_DELAY            = 27,   /* #delay timing control */
    UIR_LOOP_BACK        = 28,   /* loop continuation (condition + body to re-execute) */
    UIR_WAIT             = 29,   /* wait (condition) statement */
    UIR_EVENT_CTRL       = 30,   /* @(posedge/negedge signal) statement */
    UIR_DISABLE          = 31,   /* disable block/task statement */
    UIR_SYS_FUNC_EXPR    = 32,   /* expression-level system function ($signed, $clog2, etc.) */
    UIR_FORCE            = 33,   /* force signal = value; */
    UIR_RELEASE          = 34,   /* release signal; */
    UIR_EVENT_TRIGGER    = 35,   /* -> event_name; */
    UIR_SPECIFY          = 36,   /* specify block container */
    UIR_UDP              = 37,   /* user-defined primitive definition */
    UIR_EXIT             = 38,   /* VHDL exit loop statement */
    UIR_NEXT             = 39,   /* VHDL next loop statement */
    UIR_RETURN           = 40,   /* VHDL return statement */
    UIR_ALWAYS_COMB      = 41,   /* SystemVerilog always_comb */
    UIR_ALWAYS_FF        = 42,   /* SystemVerilog always_ff */
    UIR_ALWAYS_LATCH     = 43,   /* SystemVerilog always_latch */
    UIR_VHDL_ASSERT      = 44,   /* VHDL assert/report statement */
} uir_node_kind_t;

/* === Source location === */

typedef struct uir_loc {
    const char *file;
    uint32_t line;
    uint32_t column;
} uir_loc_t;

/* === Forward declarations === */

typedef struct uir_node uir_node_t;
typedef struct uir_design_unit uir_design_unit_t;
typedef struct uir_port uir_port_t;
typedef struct uir_signal uir_signal_t;
typedef struct uir_process uir_process_t;
typedef struct uir_assign uir_assign_t;
typedef struct uir_expr uir_expr_t;
typedef struct uir_literal uir_literal_t;
typedef struct uir_ref uir_ref_t;
typedef struct uir_cond uir_cond_t;
typedef struct uir_if uir_if_t;
typedef struct uir_case uir_case_t;
typedef struct uir_case_item uir_case_item_t;
typedef struct uir_loop uir_loop_t;
typedef struct uir_block uir_block_t;
typedef struct uir_instance uir_instance_t;
typedef struct uir_gate uir_gate_t;
typedef struct uir_generate uir_generate_t;
typedef struct uir_sys_task uir_sys_task_t;
typedef struct uir_func uir_func_t;
typedef struct uir_func_call uir_func_call_t;
typedef struct uir_delay uir_delay_t;
typedef struct uir_loop_back uir_loop_back_t;
typedef struct uir_wait uir_wait_t;
typedef struct uir_event_ctrl uir_event_ctrl_t;
typedef struct uir_disable uir_disable_t;
typedef struct uir_force uir_force_t;
typedef struct uir_release uir_release_t;
typedef struct uir_sys_func_expr uir_sys_func_expr_t;
typedef struct uir_event_trigger uir_event_trigger_t;
typedef struct uir_specify uir_specify_t;
typedef struct uir_udp uir_udp_t;
typedef struct uir_exit uir_exit_t;
typedef struct uir_next uir_next_t;
typedef struct uir_return uir_return_t;

typedef enum uir_sys_task_kind {
    UIR_SYS_READMEMH = 1,
    UIR_SYS_WRITEMEMH = 2,
    UIR_SYS_DISPLAY = 3,
    UIR_SYS_WRITE = 4,
    UIR_SYS_MONITOR = 5,
    UIR_SYS_STOP = 6,
    UIR_SYS_FINISH = 7,
    UIR_SYS_FATAL = 8,
    UIR_SYS_ERROR = 9,
    UIR_SYS_WARNING = 10,
    UIR_SYS_INFO = 11,
    UIR_SYS_DUMPFILE = 12,
    UIR_SYS_DUMPVARS = 13,
    UIR_SYS_DUMPON = 14,
    UIR_SYS_DUMPOFF = 15,
    UIR_SYS_VALUE_PLUSARGS = 16,
    UIR_SYS_TEST_PLUSARGS = 17,
    UIR_SYS_FWRITE = 18,
    UIR_SYS_FDISPLAY = 19,
    UIR_SYS_FCLOSE = 20,
    UIR_SYS_TIMEFORMAT = 21,
    UIR_SYS_PRINTTIMESCALE = 22,
    UIR_SDF_ANNOTATE = 23,
} uir_sys_task_kind_t;

/* === Common node header === */

struct uir_node {
    uir_node_kind_t kind;
    uir_loc_t loc;
    uint32_t id;
    uir_node_t **fan_in;
    uir_node_t **fan_out;
    size_t fan_in_count;
    size_t fan_out_count;
};

/* === Signal type (used by both ports and signals) === */

typedef enum uir_signal_type {
    UIR_SIG_WIRE,
    UIR_SIG_REG,
    UIR_SIG_TRI,
    UIR_SIG_WAND,
    UIR_SIG_WOR,
    UIR_SIG_TRI0,
    UIR_SIG_TRI1,
    UIR_SIG_TRIAND,
    UIR_SIG_TRIOR,
    UIR_SIG_TRIREG,
    UIR_SIG_SUPPLY0,
    UIR_SIG_SUPPLY1,
    UIR_SIG_UWIRE,
    UIR_SIG_EVENT,
    UIR_SIG_VHDL_SIGNAL,
    UIR_SIG_VHDL_VARIABLE,
    UIR_SIG_LOGIC,
} uir_signal_type_t;

/* === Port === */

typedef enum uir_port_dir {
    UIR_PORT_IN,
    UIR_PORT_OUT,
    UIR_PORT_INOUT,
} uir_port_dir_t;

struct uir_port {
    uir_node_t base;
    char *name;
    uir_port_dir_t direction;
    uint32_t width;
    uint32_t msb;
    uint32_t lsb;
    int is_vector;
    uir_signal_type_t sig_type;   /* UIR_SIG_WIRE or UIR_SIG_REG when declared with type */
    int is_signed;
    qsim_value_t init_value;      /* default initial value for simulation */
    uint32_t array_size;          /* 0 = scalar; N = N-element array (product of all dims) */
    uint32_t array_dims[4];       /* individual unpacked dimensions (array_dim_count entries) */
    size_t array_dim_count;        /* number of unpacked dimensions */
};

/* === Signal === */

struct uir_signal {
    uir_node_t base;
    char *name;
    uir_signal_type_t sig_type;
    uint32_t width;
    uint32_t array_size;   /* 0 = scalar; N = N-element array (product of all dims) */
    uint32_t array_dims[4]; /* individual unpacked dimensions (array_dims_count entries) */
    size_t array_dim_count; /* number of unpacked dimensions */
    qsim_value_t init_value;
    int is_vector;
    int is_signed;
};

/* === Process === */

typedef enum uir_proc_kind {
    UIR_PROC_ALWAYS,
    UIR_PROC_INITIAL,
    UIR_PROC_VHDL,
    UIR_PROC_ALWAYS_COMB,
    UIR_PROC_ALWAYS_FF,
    UIR_PROC_ALWAYS_LATCH,
} uir_proc_kind_t;

typedef struct uir_sensitivity {
    uir_node_t *signal;
    int edge; /* 0 = level, 1 = posedge, -1 = negedge */
} uir_sensitivity_t;

struct uir_process {
    uir_node_t base;
    uir_proc_kind_t proc_kind;
    char *name;
    uir_node_t *body;
    uir_sensitivity_t *sensitivity_list;
    size_t sensitivity_count;
    int auto_sens;
};

/* === Continuous assignment === */

struct uir_assign {
    uir_node_t base;
    uir_node_t *lhs;
    uir_node_t *rhs;
    int delay;
    uir_node_t *delay_value;  /* non-NULL for "assign #N lhs = rhs;" */
};

/* === Delay control (#N stmt, assign #N ...) === */

struct uir_delay {
    uir_node_t base;
    uir_node_t *delay_value;  /* expression evaluated at runtime */
    uir_node_t *body;         /* statement to execute after delay */
    int always_loop;          /* 1 = re-execute this delay when body finishes */
};

/* === Loop back (loop continuation marker) === */

struct uir_loop_back {
    uir_node_t base;
    uir_node_t *condition;    /* loop condition expression */
    uir_node_t *body;         /* loop body to re-execute */
};

/* === Wait statement: wait (condition) statement === */

struct uir_wait {
    uir_node_t base;
    uir_node_t *condition;    /* expression to wait on */
    uir_node_t *body;         /* statement to execute when condition true */
};

/* === Event control: @(posedge/negedge signal) statement === */

struct uir_event_ctrl {
    uir_node_t base;
    char *signal_name;        /* signal to watch */
    int edge;                 /* 0 = any change, 1 = posedge, -1 = negedge */
    uir_node_t *body;         /* statement to execute on event */
};

/* === Disable statement: disable task/block_name; === */

struct uir_disable {
    uir_node_t base;
    char *target_name;        /* name of block or task to disable */
};

/* === Force statement: force signal = value; === */

struct uir_force {
    uir_node_t base;
    uir_node_t *lhs;             /* reference to the target signal */
    uir_node_t *rhs;             /* expression to force */
};

/* === Release statement: release signal; === */

struct uir_release {
    uir_node_t base;
    uir_node_t *target;          /* reference to the target signal */
};

/* === Event trigger: -> event_name; === */

struct uir_event_trigger {
    uir_node_t base;
    char *name;                  /* event name to trigger */
};

/* === VHDL exit statement: exit [loop_label]; === */

struct uir_exit {
    uir_node_t base;
    char *loop_label;            /* NULL for innermost loop */
};

/* === VHDL next statement: next [loop_label]; === */

struct uir_next {
    uir_node_t base;
    char *loop_label;            /* NULL for innermost loop */
};

/* === VHDL return statement: return [expr]; === */

struct uir_return {
    uir_node_t base;
    uir_node_t *expr;            /* NULL for procedure return */
};

/* === Attribute: (* name [= value] *) === */

typedef struct uir_attr {
    char *name;
    char *value;                 /* NULL for flag-only attributes */
} uir_attr_t;

/* === Defparam: defparam hier.path.name = value; === */

typedef struct uir_defparam {
    char *hier_path;             /* e.g. "top.uut.WIDTH" */
    uir_node_t *value;           /* constant expression */
} uir_defparam_t;

/* === Specify block: specify ... endspecify === */

typedef enum uir_path_type {
    UIR_PATH_PARALLEL,   /* (a => b) — one-to-one */
    UIR_PATH_FULL,       /* (a *> b) — one-to-all */
} uir_path_type_t;

/* A single path delay entry inside a specify block. */
typedef struct uir_path_delay {
    char *src;             /* source pin name */
    char *dst;             /* destination pin name */
    char *data_src;        /* data source for data terminal expr (q +: d), NULL otherwise */
    uir_path_type_t type;  /* PARALLEL or FULL */
    int src_edge;          /* 0=none, 1=posedge, -1=negedge */
    int dst_polarity;      /* 0=none, 1=+: same, -1=-: opposite */
    uir_node_t *condition; /* NULL for unconditional; else expression */
    uir_node_t *rise_delay;    /* rise delay expression */
    uir_node_t *fall_delay;    /* fall delay expression */
    uir_node_t *z_delay;       /* full-path only, NULL for parallel */
    uir_node_t *x_delay;       /* full-path only, NULL for parallel */
} uir_path_delay_t;

typedef enum uir_timing_check_kind {
    UIR_TIMING_SETUP       = 1,
    UIR_TIMING_HOLD        = 2,
    UIR_TIMING_SETUPHOLD   = 3,
    UIR_TIMING_WIDTH       = 4,
    UIR_TIMING_PERIOD      = 5,
} uir_timing_check_kind_t;

typedef struct uir_timing_check {
    uir_timing_check_kind_t kind;
    char *data_pin;
    char *ref_pin;
    uir_node_t *limit;       /* constant expression */
    char *notifier;          /* optional notifier register */
} uir_timing_check_t;

/* Specify block — scoped within a module design unit */
struct uir_specify {
    uir_node_t base;
    uir_defparam_t *specparams;   /* specparam declarations */
    size_t specparam_count;
    uir_path_delay_t *paths;      /* path delay entries */
    size_t path_count;
    uir_timing_check_t *timing_checks;  /* timing check entries */
    size_t timing_check_count;
};

/* === UDP (User-Defined Primitive) === */

typedef struct uir_udp_entry {
    char *input_pattern;   /* one char per input: '0','1','x','?','b','-','*' */
    char current_state;    /* '\0' for combinational, else '0','1','?' */
    char output;           /* '0','1','x','-' (no change for sequential) */
} uir_udp_entry_t;

typedef struct uir_udp {
    uir_node_t base;
    int is_sequential;
    uir_udp_entry_t *entries;
    size_t entry_count;
} uir_udp_t;

/* === Expressions === */

typedef enum uir_binary_op {
    UIR_OP_ADD, UIR_OP_SUB, UIR_OP_MUL, UIR_OP_DIV,
    UIR_OP_MOD, UIR_OP_REM, UIR_OP_AND, UIR_OP_OR,  UIR_OP_XOR,
    UIR_OP_NAND, UIR_OP_NOR, UIR_OP_XNOR,
    UIR_OP_EQ,  UIR_OP_NEQ, UIR_OP_LT,  UIR_OP_GT,
    UIR_OP_LE,  UIR_OP_GE,  UIR_OP_SHL, UIR_OP_SHR,
    UIR_OP_SLL, UIR_OP_SRL, UIR_OP_SLA, UIR_OP_SRA,
    UIR_OP_ROL, UIR_OP_ROR,
    UIR_OP_CONCAT, UIR_OP_REPLICATE,
    UIR_OP_CASE_EQ, UIR_OP_CASE_NEQ,
    UIR_OP_POW,
} uir_binary_op_t;

typedef enum uir_unary_op {
    UIR_OP_NEG, UIR_OP_NOT, UIR_OP_ABS,
    UIR_OP_REDUCE_AND, UIR_OP_REDUCE_NAND, UIR_OP_REDUCE_OR,
    UIR_OP_REDUCE_NOR, UIR_OP_REDUCE_XOR, UIR_OP_REDUCE_XNOR,
    UIR_OP_OTHERS, /* (others => expr) aggregate — fill to context width */
} uir_unary_op_t;

struct uir_expr {
    uir_node_t base;
    int is_binary;
    union {
        uir_binary_op_t bin_op;
        uir_unary_op_t un_op;
    } op;
    uir_node_t *operand_a;
    uir_node_t *operand_b;
    int is_signed;
};

/* === Literal === */

struct uir_literal {
    uir_node_t base;
    qsim_bit_vector_t *value;
    int is_signed;
    uint32_t width;
};

/* === Reference (signal name in expressions) === */

struct uir_ref {
    uir_node_t base;
    char *name;
    uir_node_t *index;   /* NULL for scalar ref; non-NULL for array[index] */
    uir_node_t *part_hi; /* NULL for full-signal; non-NULL for part-select hi */
    uir_node_t *part_lo; /* NULL for full-signal; non-NULL for part-select lo */
    uir_node_t **multi_index;  /* NULL for single-index; non-NULL for mem[i][j]... */
    size_t multi_idx_count;     /* number of indices in multi_index */
};

/* === Conditional expression (ternary a ? b : c) === */

struct uir_cond {
    uir_node_t base;
    uir_node_t *condition;
    uir_node_t *then_expr;
    uir_node_t *else_expr;
};

/* === Statements === */

struct uir_if {
    uir_node_t base;
    uir_node_t *condition;
    uir_node_t *then_branch;
    uir_node_t *else_branch;
};

struct uir_case_item {
    uir_node_t base;
    uir_node_t **patterns;
    size_t pattern_count;
    uir_node_t *body;
};

struct uir_case {
    uir_node_t base;
    uir_node_t *expr;
    uir_case_item_t **items;
    size_t item_count;
    uir_node_t *default_item;
    int is_wildcard;
};

struct uir_loop {
    uir_node_t base;
    uir_node_t *init_stmt;
    uir_node_t *condition;
    uir_node_t *step_stmt;
    uir_node_t *body;
};

struct uir_block {
    uir_node_t base;
    char *name;            /* NULL for unnamed blocks; strdup'd for named "begin : name" */
    uir_node_t **stmts;
    size_t stmt_count;
    int is_sequential;
};

/* === Modport (SystemVerilog) === */

typedef struct uir_modport_port {
    char *name;
    uir_port_dir_t direction;
} uir_modport_port_t;

typedef struct uir_modport {
    char *name;
    uir_modport_port_t *ports;
    size_t port_count;
} uir_modport_t;

/* === Package import (SystemVerilog) === */

typedef struct uir_package_import {
    char *pkg_name;
    char *item_name;   /* NULL for wildcard import pkg::* */
} uir_package_import_t;

/* === Instance === */

typedef struct uir_port_connection {
    char *formal_name;
    uir_node_t *actual;
    char *modport_name;  /* non-NULL for .port(modport.signal) connections */
} uir_port_connection_t;

struct uir_instance {
    uir_node_t base;
    char *instance_name;
    char *module_name;
    uir_design_unit_t *bound_to;
    uir_port_connection_t *connections;
    size_t connection_count;
};

/* === Gate === */

typedef enum uir_gate_type {
    UIR_GATE_AND, UIR_GATE_OR, UIR_GATE_XOR,
    UIR_GATE_NAND, UIR_GATE_NOR, UIR_GATE_XNOR,
    UIR_GATE_NOT, UIR_GATE_BUF,
} uir_gate_type_t;

struct uir_gate {
    uir_node_t base;
    uir_gate_type_t gate_type;
    uir_node_t *output;
    uir_node_t **inputs;
    size_t input_count;
};

/* === Generate === */

typedef enum {
    UIR_GEN_LOOP = 0,  /* generate for */
    UIR_GEN_IF,        /* generate if/else */
    UIR_GEN_CASE,      /* generate case */
    UIR_GEN_BLOCK,     /* bare generate ... endgenerate */
} uir_gen_type_t;

struct uir_generate {
    uir_node_t base;
    uir_gen_type_t gen_type;
    char *label;

    /* For LOOP */
    char *genvar_name;
    uir_node_t *for_init;
    uir_node_t *for_cond;
    uir_node_t *for_step;
    int for_direction;      /* 0=Verilog init/cond/step, 1=ascending (to), -1=descending (downto) */

    /* For IF */
    uir_node_t *if_condition;

    /* Body items (instances, signals, processes, assigns, nested generates).
     * body_template owns the memory for these nodes; it is freed after expansion. */
    uir_node_t **body_items;
    size_t body_item_count;
    uir_design_unit_t *body_template;

    /* For IF/ELSE (elaboration-time) — false/else branch */
    uir_node_t **else_body_items;
    size_t else_body_item_count;
    uir_design_unit_t *else_body_template;

    /* For CASE */
    uir_node_t *case_expr;                /* case expression */
    uir_design_unit_t **case_item_templates;  /* body templates per item */
    uir_node_t ***case_item_patterns;     /* patterns per item (arrays of exprs) */
    size_t *case_item_pattern_counts;     /* pattern count per item */
    size_t case_item_count;               /* number of items (excluding default) */
    uir_design_unit_t *case_default_template; /* optional default body template */
};

/* === Function/Task === */

/* Port declaration inside a function/task definition */
typedef struct uir_func_port {
    char *name;
    uir_port_dir_t direction;
    uint32_t width;
} uir_func_port_t;

/* Component port declaration (VHDL component declaration ports) */
typedef struct uir_component_port {
    char *name;
    uir_port_dir_t direction;
    uint32_t width;
} uir_component_port_t;

/* VHDL component declaration: component my_and is port (...); end component; */
typedef struct uir_component {
    char *name;
    uir_component_port_t *ports;
    size_t port_count;
} uir_component_t;

/* VHDL type/subtype declaration kinds */
typedef enum uir_vhdl_type_kind {
    UIR_VHDL_TYPE_ENUM    = 1,  /* type foo is (a, b, c) */
    UIR_VHDL_TYPE_RANGE   = 2,  /* type addr is range 0 to 255 */
    UIR_VHDL_TYPE_SUBTYPE = 3,  /* subtype byte is integer range 0 to 255 */
    UIR_VHDL_TYPE_RECORD = 4,  /* type rec is record ... end record; */
    UIR_VHDL_TYPE_ARRAY  = 5,  /* type arr is array (0 to 15) of std_logic_vector(7 downto 0) */
} uir_vhdl_type_kind_t;

/* VHDL type/subtype declaration */
typedef struct uir_vhdl_record_field {
    char *name;
    uint32_t width;
} uir_vhdl_record_field_t;

typedef struct uir_vhdl_type {
    char *name;
    uir_vhdl_type_kind_t kind;
    uint32_t width;
    /* For ENUM */
    char **enum_literals;
    size_t enum_literal_count;
    /* For RANGE / SUBTYPE with range */
    int64_t range_lo;
    int64_t range_hi;
    int range_dir;              /* 1=TO, -1=DOWNTO */
    /* For SUBTYPE */
    char *base_type_name;
    /* For RECORD */
    uir_vhdl_record_field_t *record_fields;
    size_t record_field_count;
    /* For ARRAY */
    uint32_t element_width;       /* width of each element */
    uint32_t array_size;          /* number of elements */
    uint32_t array_dims[4];       /* individual dimensions */
    size_t array_dim_count;        /* number of unpacked dimensions */
} uir_vhdl_type_t;

/* VHDL constant declaration: constant width : integer := 8; */
typedef struct uir_vhdl_constant {
    char *name;
    uint32_t width;
    uint64_t value;
} uir_vhdl_constant_t;

/* VHDL file variable metadata for TEXTIO support */
typedef struct {
    char *name;          /* file variable name */
    int mode;            /* -1=default, 0=read_mode, 1=write_mode, 2=append_mode */
    char *file_name;     /* file path from "is" clause (strdup'd or NULL) */
} uir_file_meta_t;

/* VHDL configuration block configuration: for arch_name ... end for; */
typedef struct uir_vhdl_config_block {
    char *arch_name;
} uir_vhdl_config_block_t;

/* VHDL alias declaration: alias X is Y; */
typedef struct uir_vhdl_alias {
    char *name;
    char *target;   /* name of the aliased object */
} uir_vhdl_alias_t;

/* VHDL group template/declaration kinds */
typedef enum uir_vhdl_group_kind {
    UIR_VHDL_GROUP_TEMPLATE = 1,  /* group name is (signal, signal); */
    UIR_VHDL_GROUP_DECL    = 2,  /* group name : templ (sig1, sig2); */
} uir_vhdl_group_kind_t;

/* VHDL group template or group declaration */
typedef struct uir_vhdl_group {
    char *name;
    uir_vhdl_group_kind_t kind;
    char *template_name;  /* for GROUP_DECL: name of the group template */
    char **constituents;  /* entity class names or instance names */
    size_t constituent_count;
} uir_vhdl_group_t;

/* VHDL attribute specification: attribute name of target : class is value; */
typedef struct uir_vhdl_attr_spec {
    char *name;
    char *target;
    char *entity_class;
    char *value;
} uir_vhdl_attr_spec_t;

/* ── IEEE numeric_std builtin function kinds ── */

typedef enum uir_numeric_std_builtin {
    UIR_NUMERIC_STD_NONE        = 0,
    UIR_NUMERIC_STD_UNSIGNED    = 1,   /* unsigned(slv) → unsigned */
    UIR_NUMERIC_STD_SIGNED      = 2,   /* signed(slv) → signed */
    UIR_NUMERIC_STD_TO_INTEGER  = 3,   /* to_integer(unsigned/signed) → integer */
    UIR_NUMERIC_STD_TO_UNSIGNED = 4,   /* to_unsigned(int, size) → unsigned */
    UIR_NUMERIC_STD_TO_SIGNED   = 5,   /* to_signed(int, size) → signed */
    UIR_NUMERIC_STD_STD_LOGIC_VECTOR = 6,   /* std_logic_vector(unsigned/signed) → slv */
    UIR_NUMERIC_STD_SHIFT_LEFT  = 7,   /* shift_left(unsigned, integer) → unsigned */
    UIR_NUMERIC_STD_SHIFT_RIGHT = 8,   /* shift_right(unsigned, integer) → unsigned */
} uir_numeric_std_builtin_t;

/* ── VITAL primitive / timing check builtin kinds ── */

typedef enum uir_vital_builtin {
    UIR_VITAL_NONE             = 0,
    /* VITAL_Primitives package */
    UIR_VITAL_AND              = 1,
    UIR_VITAL_OR               = 2,
    UIR_VITAL_XOR              = 3,
    UIR_VITAL_NAND             = 4,
    UIR_VITAL_NOR              = 5,
    UIR_VITAL_XNOR             = 6,
    UIR_VITAL_BUF              = 7,
    UIR_VITAL_INV              = 8,
    UIR_VITAL_IDENT            = 9,
    UIR_VITAL_LEVEL            = 10,
    /* VITAL_Timing package */
    UIR_VITAL_SETUPHOLDCHECK   = 11,
    UIR_VITAL_WIDTHCHECK       = 12,
    UIR_VITAL_PERIODCHECK      = 13,
    UIR_VITAL_RECOVERYCHECK    = 14,
} uir_vital_builtin_t;

/* Function/Task definition.
 * Functions have a return value (return_width > 0, ports are all inputs).
 * Tasks have return_width == 0, ports can be input/output/inout. */
struct uir_func {
    uir_node_t base;
    char *name;
    int is_function;
    int is_automatic;
    uir_func_port_t *ports;
    size_t port_count;
    uir_node_t **locals;    /* UIR_SIGNAL nodes for local reg/integer decls */
    size_t local_count;
    uir_node_t *body;       /* block containing statements */
    uint32_t return_width;  /* 0 for tasks */
    int builtin_kind;       /* UIR_VITAL_* when the function is a builtin, 0 otherwise */
};

/* Function call expression / Task enable statement.
 * For UIR_FUNC_CALL: used in expressions (RHS of assign, etc.)
 * For UIR_TASK_ENABLE: used as a statement. */
struct uir_func_call {
    uir_node_t base;
    char *name;           /* function/task name */
    uir_node_t **args;    /* actual argument expressions */
    size_t arg_count;
};

/* === System task ($readmemh, $writememh, $display, $write, $monitor) === */

struct uir_sys_task {
    uir_node_t base;
    uir_sys_task_kind_t task_kind;
    char *filename;
    char *mem_name;
    char *fmt;             /* format string (display/write/monitor) */
    uir_node_t **args;     /* argument expressions */
    size_t arg_count;
};

/* === System function expression ($signed, $unsigned, $clog2, $time, etc.) === */

typedef enum uir_sys_func_kind {
    UIR_SYS_FUNC_SIGNED = 1,
    UIR_SYS_FUNC_UNSIGNED = 2,
    UIR_SYS_FUNC_CLOG2 = 3,
    UIR_SYS_FUNC_TIME = 4,
    UIR_SYS_FUNC_REALTIME = 5,
    UIR_SYS_FUNC_RANDOM = 6,
    UIR_SYS_FUNC_FOPEN = 7,
} uir_sys_func_kind_t;

struct uir_sys_func_expr {
    uir_node_t base;
    uir_sys_func_kind_t func_kind;
    uir_node_t **args;
    size_t arg_count;
};

/* VHDL assert/report: assert <cond> [report <msg>] [severity <note|warning|error|failure>] */
typedef struct uir_vhdl_assert {
    uir_node_t base;
    uir_node_t *condition;   /* evaluated on eval: 0 means assertion fires */
    uir_node_t *message;     /* report message expression (may be NULL) */
    int severity;            /* 0=note, 1=warning, 2=error, 3=failure */
} uir_vhdl_assert_t;

/* === Design unit (top-level container with arena) === */

struct uir_design_unit {
    uir_node_t base;
    char *name;
    char *language;
    uir_port_t **ports;
    uir_signal_t **signals;
    uir_process_t **processes;
    uir_assign_t **assigns;
    uir_instance_t **instances;
    uir_generate_t **generates;
    uir_func_t **func_tasks;
    uir_defparam_t *defparams;
    uir_defparam_t *params;      /* module parameters (parameter WIDTH = 8) */
    uir_attr_t *attrs;
    uir_specify_t **specifies;
    uir_udp_t *udp;              /* non-NULL only for UDP primitive definitions */
    uir_component_t *components; /* VHDL component declarations (array of structs) */
    uir_vhdl_type_t *vhdl_types; /* VHDL type/subtype declarations (array of structs) */
    uir_vhdl_constant_t *vhdl_constants; /* VHDL constant declarations (array of structs) */
    uir_vhdl_alias_t *vhdl_aliases;      /* VHDL alias declarations (array of structs) */
    size_t vhdl_alias_count;
    uir_vhdl_group_t *vhdl_groups;       /* VHDL group/template declarations (array of structs) */
    size_t vhdl_group_count;
    uir_vhdl_attr_spec_t *vhdl_attr_specs; /* VHDL attribute specifications */
    size_t vhdl_attr_spec_count;
    char *config_entity_name;            /* VHDL configuration: entity this config binds to */
    uir_vhdl_config_block_t *config_blocks; /* block configurations */
    size_t config_block_count;
    uir_modport_t *modports;     /* SystemVerilog modport declarations (inside interfaces) */
    size_t modport_count;
    uir_package_import_t *imports; /* SystemVerilog package import statements */
    size_t import_count;
    uir_node_t **all_nodes;
    size_t port_count;
    size_t signal_count;
    size_t process_count;
    size_t assign_count;
    size_t instance_count;
    size_t generate_count;
    size_t func_task_count;
    size_t defparam_count;
    size_t param_count;          /* number of module parameters */
    size_t attr_count;
    size_t specify_count;
    size_t component_count;
    size_t vhdl_type_count;
    size_t vhdl_constant_count;
    uir_file_meta_t *file_metas;    /* VHDL file variable declarations (TEXTIO) */
    size_t file_meta_count;
    size_t node_count;
    size_t node_capacity;
    int is_interface;            /* 1 if this is a SystemVerilog interface declaration */

    /* VHDL library and use clause storage */
    char **library_names;    /* "ieee", "std", etc. */
    size_t library_count;
    char **use_clauses;      /* "ieee.std_logic_1164.all" format */
    size_t use_count;
};

/* === Construction API === */

uir_design_unit_t *uir_create_design_unit(const char *name, const char *language, uir_loc_t loc);
void uir_destroy_design_unit(uir_design_unit_t *unit);

/* Low-level node allocation (from arena). */
uir_node_t *uir_alloc_node(uir_design_unit_t *unit, uir_node_kind_t kind, size_t struct_size, uir_loc_t loc);

uir_port_t     *uir_add_port(uir_design_unit_t *unit, const char *name,
                              uir_port_dir_t dir, uint32_t msb, uint32_t lsb,
                              uir_signal_type_t sig_type);
uir_signal_t   *uir_add_signal(uir_design_unit_t *unit, const char *name,
                                uir_signal_type_t type, uint32_t width,
                                uint32_t array_size);
uir_process_t  *uir_add_process(uir_design_unit_t *unit, uir_proc_kind_t kind);
uir_assign_t   *uir_add_assign(uir_design_unit_t *unit);
uir_instance_t *uir_add_instance(uir_design_unit_t *unit, const char *inst_name,
                                  const char *module_name);
void uir_add_connection(uir_instance_t *inst, const char *formal_name,
                         uir_node_t *actual);
uir_block_t    *uir_add_block(uir_design_unit_t *unit, int is_sequential);
uir_generate_t *uir_add_generate(uir_design_unit_t *unit, uir_gen_type_t gen_type);
void uir_add_generate_body_item(uir_generate_t *gen, uir_node_t *item);
void uir_add_generate_else_body_item(uir_generate_t *gen, uir_node_t *item);

/* Expression builders */
uir_expr_t *uir_make_binary(uir_design_unit_t *unit, uir_binary_op_t op,
                             uir_node_t *a, uir_node_t *b, uir_loc_t loc);
uir_expr_t *uir_make_unary(uir_design_unit_t *unit, uir_unary_op_t op,
                            uir_node_t *a, uir_loc_t loc);
uir_literal_t *uir_make_literal(uir_design_unit_t *unit,
                                 qsim_bit_vector_t *value, uir_loc_t loc);
uir_node_t *uir_make_ref(uir_design_unit_t *unit, const char *name, uir_loc_t loc);
uir_node_t *uir_make_ref_index(uir_design_unit_t *unit, const char *name,
                                uir_node_t *index, uir_loc_t loc);
uir_node_t *uir_make_ref_part_select(uir_design_unit_t *unit, const char *name,
                                      uir_node_t *hi, uir_node_t *lo, uir_loc_t loc);
uir_node_t *uir_make_ref_multi_index(uir_design_unit_t *unit, const char *name,
                                      uir_node_t **indices, size_t idx_count,
                                      uir_loc_t loc);
uir_node_t *uir_make_sys_task(uir_design_unit_t *unit, uir_sys_task_kind_t kind,
                               const char *filename, const char *mem_name,
                               const char *fmt, uir_node_t **args, size_t arg_count,
                               uir_loc_t loc);

/* Event trigger (-> name;). */
uir_event_trigger_t *uir_make_event_trigger(uir_design_unit_t *unit,
                                              const char *name, uir_loc_t loc);

/* Specify construction */
uir_specify_t *uir_add_specify(uir_design_unit_t *unit);
void uir_add_specpath(uir_specify_t *spec, const uir_path_delay_t *pd);
void uir_add_timing_check(uir_specify_t *spec, const uir_timing_check_t *tc);
void uir_add_specparam(uir_specify_t *spec, const char *name, uir_node_t *value);

/* UDP construction */
uir_udp_t *uir_add_udp(uir_design_unit_t *unit, int is_sequential);
void uir_add_udp_entry(uir_udp_t *udp, const char *input_pattern,
                        const char *state_and_output);

/* Expression-level system function ($signed, $clog2, $time, $random, etc.) */
uir_node_t *uir_make_sys_func_expr(uir_design_unit_t *unit, uir_sys_func_kind_t kind,
                                    uir_node_t **args, size_t arg_count,
                                    uir_loc_t loc);

/* Function/task construction */
uir_func_t *uir_add_func_task(uir_design_unit_t *unit, const char *name,
                               int is_function);
void uir_add_func_port(uir_func_t *ft, const char *name,
                        uir_port_dir_t dir, uint32_t width);
void uir_add_func_local(uir_func_t *ft, uir_node_t *local_sig);
uir_func_call_t *uir_make_func_call(uir_design_unit_t *unit, const char *name,
                                     uir_node_t **args, size_t arg_count,
                                     uir_loc_t loc);

/* VHDL assert/report statement constructor */
uir_vhdl_assert_t *uir_make_vhdl_assert(uir_design_unit_t *unit,
                                          uir_node_t *condition,
                                          uir_node_t *message,
                                          int severity, uir_loc_t loc);

/* SystemVerilog modport construction */
uir_modport_t *uir_add_modport(uir_design_unit_t *unit, const char *name);
void uir_add_modport_port(uir_modport_t *mp, const char *name, uir_port_dir_t dir);

/* SystemVerilog package import */
void uir_add_import(uir_design_unit_t *unit, const char *pkg_name, const char *item_name);

/* VHDL component declaration construction */
uir_component_t *uir_add_component(uir_design_unit_t *unit, const char *name);
void uir_add_component_port(uir_component_t *comp, const char *name,
                             uir_port_dir_t dir, uint32_t width);

/* VHDL type/subtype declaration construction */
uir_vhdl_type_t *uir_add_vhdl_type(uir_design_unit_t *unit, const char *name,
                                    uir_vhdl_type_kind_t kind);
void uir_add_vhdl_type_literal(uir_vhdl_type_t *t, const char *literal);
void uir_set_vhdl_type_range(uir_vhdl_type_t *t, int64_t lo, int64_t hi, int dir);
void uir_set_vhdl_type_base(uir_vhdl_type_t *t, const char *base_name);
void uir_add_vhdl_record_field(uir_vhdl_type_t *t, const char *name, uint32_t width);

/* VHDL constant declaration construction */
uir_vhdl_constant_t *uir_add_vhdl_constant(uir_design_unit_t *unit, const char *name,
                                            uint32_t width, uint64_t value);
void uir_add_vhdl_config_block(uir_design_unit_t *unit, const char *arch_name);
uir_file_meta_t *uir_add_file_meta(uir_design_unit_t *unit, const char *name,
                                     int mode, const char *file_name);

/* VHDL alias declaration construction */
uir_vhdl_alias_t *uir_add_vhdl_alias(uir_design_unit_t *unit, const char *name,
                                      const char *target);

/* VHDL group/template declaration construction */
uir_vhdl_group_t *uir_add_vhdl_group(uir_design_unit_t *unit, const char *name,
                                      uir_vhdl_group_kind_t kind,
                                      const char *template_name);
void uir_add_vhdl_group_constituent(uir_vhdl_group_t *g, const char *name);

/* VHDL attribute specification construction */
uir_vhdl_attr_spec_t *uir_add_vhdl_attr_spec(uir_design_unit_t *unit,
    const char *name, const char *target, const char *entity_class,
    const char *value);

/* Dependency wiring */
void uir_add_fan_in(uir_node_t *node, uir_node_t *driver);
void uir_add_fan_out(uir_node_t *node, uir_node_t *load);

/* Hierarchy */
void uir_add_child(uir_design_unit_t *parent, uir_design_unit_t *child);

/* === Query API === */

uir_node_t *uir_find_signal(uir_design_unit_t *unit, const char *hier_path);
uir_node_t **uir_get_fan_in(uir_node_t *node, size_t *count);
uir_node_t **uir_get_fan_out(uir_node_t *node, size_t *count);

/* Trace driver/load chains (allocates array, caller must free with free()). */
uir_node_t **uir_trace_drivers(uir_node_t *signal, size_t *count, int max_depth);
uir_node_t **uir_trace_loads(uir_node_t *signal, size_t *count, int max_depth);

/* Walk all nodes in design unit. */
void uir_walk(uir_design_unit_t *unit,
              void (*callback)(uir_node_t *node, void *ctx),
              void *ctx);

/* Serialize to JSON (caller must free returned string). */
char *uir_to_json(uir_design_unit_t *unit);

/* Intern a string (owned by the design unit arena). */
const char *uir_intern_string(uir_design_unit_t *unit, const char *str);

/* Clone a node and its subtree into a target design unit, applying a
 * genvar-name→value substitution to every UIR_REF node whose name matches.
 * subst_genvar may be NULL (no substitution). node_to_idx is an optional
 * output parameter set to the new node's index in target_unit->all_nodes.
 * Returns the cloned node (owned by target_unit), or NULL on failure. */
typedef struct {
    const char *genvar_name;  /* genvar identifier to substitute */
    uint64_t    genvar_value; /* constant value to substitute in */
    const char *name_prefix;  /* prepended to instance/signal names (e.g. "gen[0].") */
} uir_subst_t;

uir_node_t *uir_clone_node(uir_design_unit_t *target_unit,
                            uir_node_t *src,
                            const uir_subst_t *subst);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_UIR_H */
