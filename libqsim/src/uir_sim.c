#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"
#include "libqsim/scheduler.h"
#include "libqsim/sim_thread.h"
#include "libqsim/sdf_parse.h"
#include "libqsim/vhdl_pkg_registry.h"
#include "libqsim/vhdl_library.h"
#include "libqsim/value.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Note: platform atomics used instead of <stdatomic.h> for MSVC compat.
 * sim_atomic_load/store wrappers are provided via sim_thread.h. */
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

/* ── Signal entry in the simulation state table ── */

#define MAX_DRIVERS_PER_SIGNAL 8

typedef struct {
    int process_id;          /* cont_assign index, -1 = unused */
    qsim_bit_vector_t *value;
} resolution_driver_t;

typedef struct {
    uir_node_t *node;
    char name[512];
    qsim_bit_vector_t *value;
    qsim_bit_vector_t *prev_value;   /* value before most recent update (for edge detection) */
    int is_signed;
    int net_group;                     /* resolution behavior group */
    resolution_driver_t drivers[MAX_DRIVERS_PER_SIGNAL];
    int driver_count;                  /* number of active driver slots */
    int needs_resolve;                 /* set when any driver writes, cleared after Phase 1.5 */
    uint64_t last_change_time;         /* sim time of last value change (for timing checks) */
} sim_signal_t;

/* Net group constants for sim_signal_t.net_group */
#define NET_GROUP_WIRE    0
#define NET_GROUP_WAND    1
#define NET_GROUP_WOR     2
#define NET_GROUP_TRI0    3
#define NET_GROUP_TRI1    4
#define NET_GROUP_SUPPLY0 5
#define NET_GROUP_SUPPLY1 6
#define NET_GROUP_TRIREG  7
#define NET_GROUP_STD_LOGIC 8

/* Hash table entry for O(1) signal name→index lookup. Open-addressing
 * with linear probing. sig_idx == -1 marks an empty slot. */
typedef struct {
    int sig_idx;
    uint32_t hash;    /* full FNV-1a hash for fast strcmp rejection */
} signal_ht_entry_t;

/* FNV-1a hash — simple, branch-free, good for hierarchical paths. */
static uint32_t sig_ht_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) { h ^= (unsigned char)*name++; h *= 16777619u; }
    return h;
}

static void signal_ht_init(uir_sim_context_t *ctx);
static int  signal_ht_lookup(uir_sim_context_t *ctx, const char *name);
static void signal_ht_insert(uir_sim_context_t *ctx, int sig_idx);

/* ── Simple event queue entry ── */

typedef struct sim_event {
    uint64_t time;
    uint32_t delta;
    uint32_t sig_idx;
    qsim_bit_vector_t *value;
    int is_nba;
    int has_part_select;  /* 1 if this event is a part-select write (NBA) */
    int ps_lo;           /* low bit index of part-select range */
    int ps_hi;           /* high bit index of part-select range */
    int src_pid;         /* process ID that created this event, -1 = non-process */
    int is_stmt_event;   /* 1 = wakeup event carrying a statement */
    uir_node_t *stmt;    /* statement to execute on wakeup */
    int loop_always;     /* 1 = re-execute owner_body after stmt completes */
    uir_node_t *owner_body;  /* process body to re-execute if loop_always */
    int cancelled;           /* 1 = skip this event (block was disabled) */
    char block_hier[256];    /* hierarchical path of containing named block (empty if none) */
    struct sim_event *next;
} sim_event_t;

/* ── Event arena pool ── */
#define EVENT_POOL_BLOCK_SIZE 1024

/* A contiguous block of events in the arena. Blocks are linked so the
 * pool can grow arbitrarily without per-event malloc/free. */
typedef struct event_block {
    struct event_block *next;
    sim_event_t events[EVENT_POOL_BLOCK_SIZE];
} event_block_t;

static sim_event_t *pool_alloc_event(uir_sim_context_t *ctx);
static void        pool_free_event(uir_sim_context_t *ctx, sim_event_t *ev);
static void        pool_destroy(uir_sim_context_t *ctx);

/* ── Waiter for wait/event_control statements ── */

typedef struct {
    uir_node_t *node;       /* UIR_WAIT or UIR_EVENT_CTRL node */
    int *sig_indices;       /* signal indices to watch */
    size_t sig_count;
    int cancelled;          /* 1 = skip this waiter (block was disabled) */
    char block_hier[256];   /* hierarchical path of containing named block */
} sim_waiter_t;

/* ── Continuous assign entry ── */

typedef struct {
    uir_assign_t *assign;
    int *dep_sigs;      /* signal indices that the RHS reads */
    size_t dep_count;
    char prefix[256];   /* hierarchical prefix for scoped signal lookup */
} cont_assign_entry_t;

/* ── Breakpoint entry ── */

typedef struct {
    char *file;
    uint32_t line;
} breakpoint_t;

/* ── Coverage entry ── */

typedef struct {
    char *file;
    uint32_t line;
} coverage_entry_t;

/* ── Function/task frame for simulation ── */

typedef struct {
    uir_func_t *def;          /* function/task definition */
    int return_sig_idx;       /* signal index for return value (-1 for tasks) */
    int *port_sig_indices;    /* signal indices for ports (size = def->port_count, -1 if unset) */
    char prefix[256];         /* prefix for scoped signal lookup during body execution */
    int is_automatic;         /* 1 = automatic (save/restore on call/return) */
    int auto_first_sig_idx;   /* first ctx-signals[] index belonging to this frame */
    int auto_sig_count;       /* number of signal slots to save/restore */
} func_frame_t;

/* ── $monitor entry (tracks last-evaluated values) ── */

typedef struct {
    char *fmt;
    uir_node_t **args;
    size_t arg_count;
    qsim_bit_vector_t **last_vals;  /* one per arg, NULL until first eval */
} monitor_entry_t;

/* ── Port wire for instance port connections ── */

typedef struct {
    int src_sig_idx;    /* signal index that drives */
    int dst_sig_idx;    /* signal index that receives */
    int part_lo;        /* -1 = full signal, >=0 = part-select on parent signal */
    int part_width;     /* valid when part_lo >= 0 */
    uir_port_dir_t dir; /* port direction (IN=parent→child, OUT=child→parent) */
} port_wire_t;

/* ── Path delay entry (elaborated from specify block) ── */

typedef struct {
    int src_sig_idx;      /* source signal index */
    int dst_sig_idx;      /* destination signal index */
    int src_edge;         /* 0=any, 1=posedge, -1=negedge */
    uint64_t rise_delay;  /* 0→1 delay in time units */
    uint64_t fall_delay;  /* 1→0 delay in time units */
    uint64_t z_delay;     /* full-path only: 0→Z transition, 0 for parallel */
    uint64_t x_delay;     /* full-path only: 0→X transition, 0 for parallel */
    uir_node_t *condition; /* NULL for unconditional path delay */
} sim_path_delay_t;

/* ── Timing check entry (elaborated from specify block) ── */

typedef struct {
    int kind;              /* UIR_TIMING_SETUP/HOLD/WIDTH/PERIOD */
    int data_sig_idx;      /* data pin signal index (-1 if none) */
    int ref_sig_idx;       /* reference (clock) pin signal index */
    uint64_t limit;        /* limit in time units */
    uint64_t last_ref_time; /* last ref signal edge time (for $hold/$width/$period) */
    int last_ref_bit;      /* previous ref bit 0 value (for edge detection) */
} sim_timing_check_t;

/* ── UDP instance entry (elaborated from UDP primitive instances) ── */

typedef struct {
    uir_udp_t *udp;           /* UDP primitive definition */
    int output_sig_idx;       /* output signal index */
    int *input_sig_indices;   /* signal indices for inputs, ordered per port list */
    size_t input_count;
    int state_sig_idx;        /* sequential UDP internal state signal (-1 for combinational) */
    char prefix[256];         /* hierarchical prefix for signal lookup */
} udp_instance_entry_t;

/* ── Simulation context ── */

/* Forward declaration — full definition follows uir_sim_context */
typedef struct signal_change_s signal_change_t;

/* Forward declarations for static functions defined later in this file */
static int signal_write_resolved(struct uir_sim_context *ctx, uint32_t sig_idx,
                                  qsim_bit_vector_t *new_val,
                                  qsim_bit_vector_t **old_val_out,
                                  int process_id);

struct uir_sim_context {
    sim_signal_t *signals;
    size_t signal_count;
    size_t signal_cap;

    /* VHDL alias resolution: alias name → target signal index */
    char **alias_names;
    int *alias_targets;
    size_t alias_count;
    size_t alias_cap;

    uir_process_t **processes;
    char (*process_prefixes)[256];  /* hierarchical prefix, 1:1 with processes[] */
    size_t process_count;
    size_t process_cap;

    char current_prefix[256];       /* set before process body execution */
    int current_process_id;         /* set before process body exec, -1 = none */
    uint32_t current_context_width; /* target width for expression evaluation (0 = auto) */
    int current_is_signed;          /* signedness from current expression evaluation */
    uir_design_unit_t **units;
    size_t unit_count;

    cont_assign_entry_t *cont_assigns;
    size_t cont_assign_count;

    port_wire_t *port_wires;
    size_t port_wire_count;
    size_t port_wire_cap;
    int *port_wire_srcs;        /* unique src signal indices from port_wires[] */
    size_t port_wire_src_count; /* length of port_wire_srcs */

    /* ── Stmt event change recording ──
     * Replaces O(signal_count) clone-all with recording in exec_assign
     * so signal changes during stmt event execution are captured directly. */
    int recording_mode;
    signal_change_t *recorded_changes;
    size_t recorded_count;
    size_t recorded_cap;

    sim_event_t *event_head;
    sim_event_t *event_tail;
    size_t event_count;
    uint64_t total_events;  // cumulative events processed

    uint64_t current_time;
    uint32_t current_delta;

    uir_event_callback_t event_cb;
    void *event_cb_user_data;

    qsim_step_callback_t step_cb;
    void *step_cb_user_data;

    uir_sys_display_cb_t display_cb;
    void *display_cb_user_data;

    breakpoint_t *breakpoints;
    size_t breakpoint_count;
    size_t breakpoint_cap;
    int breakpoint_hit;

    coverage_entry_t *coverage;
    size_t coverage_count;
    size_t coverage_cap;

    int initial_eval_done;

    func_frame_t *func_frames;
    size_t func_frame_count;

    /* Pending literal port initializations (collect then flush after port wiring) */
    int *pending_lit_sigs;
    qsim_bit_vector_t **pending_lit_vals;
    size_t pending_lit_count;
    size_t pending_lit_cap;

    /* $monitor tracking */
    monitor_entry_t *monitor;

    /* Delta-cycle iteration limit per time step (prevents hang on combinatorial loops) */
    uint32_t max_deltas_per_time;

    /* Wait/event control waiter list */
    sim_waiter_t *waiters;
    size_t waiter_count;
    size_t waiter_cap;

    /* Disable target (set by disable statement, cleared after processing) */
    char disable_scope[256];      /* hierarchical scope for disable (empty = local) */
    char disable_block_name[64];  /* target block name */

    /* Current named block hierarchical path for event/waiter cancellation */
    char current_block_hier[256];

    /* Signal hash table (O(1) name→index lookup) */
    signal_ht_entry_t *sig_ht;
    size_t sig_ht_cap;
    size_t sig_ht_count;
    int sig_ht_ready;              /* 1 after signal_ht_init called */

    /* Event arena pool blocks */
    event_block_t *ev_pool_blocks;
    sim_event_t   *ev_free_list;

    /* ── Signal graph partitions for parallel delta evaluation ── */
    int *signal_partition;   /* [signal_count] partition_id for each signal */
    int *process_partition;  /* [process_count] partition_id for each process */
    int *ca_partition;       /* [cont_assign_count] partition_id for each CA */
    int partition_count;

    /* ── Thread pool for parallel evaluation ── */
    int thread_count;
    sim_thread_t *workers;          /* [thread_count-1], NULL when not created */
    struct sim_thread_state *threads;  /* [thread_count], per-thread state */
    sim_barrier_t phase_barrier;
    volatile int parallel_phase;  /* set/read via sim_atomic_store/load */
    sim_mutex_t pool_mutex;         /* global event pool fallback mutex */

    /* CA change tracking for parallel Phase 2b */
    int *ca_changed_sigs;              /* signal indices changed by CA eval */
    qsim_bit_vector_t **ca_changed_old_vals; /* old values for edge detection */
    int ca_changed_count;
    int ca_changed_cap;

    uint64_t driver_gen_counter;  /* incremented each delta batch for resolution tracking */

    /* ── System task flags ── */
    int stop_requested;    /* set by $stop, $fatal */
    int finish_requested;  /* set by $finish, $fatal */
    uint64_t rand_state;   /* LCG state for $random */

    /* ── Path delays from specify blocks ── */
    sim_path_delay_t *path_delays;
    size_t path_delay_count;

    /* ── Timing checks from specify blocks ── */
    sim_timing_check_t *timing_checks;
    size_t timing_check_count;

    /* ── UDP instances (evaluated as truth-table lookups on input change) ── */
    udp_instance_entry_t *udp_instances;
    size_t udp_instance_count;

    /* ── VHDL TEXTIO runtime ── */
    struct textio_file_st { FILE *fp; int mode; int is_open; char *filename; } **file_handles;
    struct textio_line_st { char *data; size_t len; size_t pos; size_t cap; } **line_buffers;
};

/* ── Bit-vector helpers ── */

/* Convert bit vector to uint64. Returns 0 if any bit is X or Z. */
int uir_bv_to_u64(const qsim_bit_vector_t *bv, uint64_t *out) {
    uint64_t val = 0;
    uint32_t limit = bv->width < 64 ? bv->width : 64;
    for (uint32_t i = 0; i < limit; i++) {
        qsim_value_t bit = qsim_bit_get(bv, i);
        if (bit.state == QSIM_X || bit.state == QSIM_Z) return 0;
        if (bit.state == QSIM_1) val |= (1ULL << i);
    }
    *out = val;
    return 1;
}

/* Create bit vector from uint64 with given width */
qsim_bit_vector_t *uir_u64_to_bv(uint64_t val, uint32_t width) {
    qsim_bit_vector_t *r = qsim_bit_vector_alloc(width);
    if (!r) return NULL;
    for (uint32_t i = 0; i < width; i++)
        qsim_bit_set(r, i, (val >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
    return r;
}

/* Return an X vector of the given width (result of operation on X inputs) */
static qsim_bit_vector_t *x_bv(uint32_t width) {
    return qsim_bit_vector_from_state(width, QSIM_X);
}

/* Binary arithmetic/logic on bit vectors. For X/Z inputs, result is X. */
static qsim_bit_vector_t *bv_binary_op(const qsim_bit_vector_t *a,
                                        const qsim_bit_vector_t *b,
                                        uir_binary_op_t op,
                                        uint32_t context_width,
                                        int is_signed) {
    uint32_t wa = a->width, wb = b->width;
    /* Shift/rotate result width is the width of the value being shifted */
    uint32_t w;
    switch (op) {
        case UIR_OP_SLL: case UIR_OP_SRL: case UIR_OP_SLA: case UIR_OP_SRA:
        case UIR_OP_ROL: case UIR_OP_ROR:
            w = wa;
            break;
        default:
            /* Comparison operators always produce 1-bit results */
            if (op == UIR_OP_EQ || op == UIR_OP_NEQ || op == UIR_OP_LT ||
                op == UIR_OP_GT || op == UIR_OP_LE || op == UIR_OP_GE) {
                w = 1;
            } else {
                if (op == UIR_OP_CONCAT)
                    w = wa + wb;
                else {
                    w = wa > wb ? wa : wb;
                    if (context_width > 0 && context_width > w)
                        w = context_width;
                }
            }
            break;
    }

    uint64_t va, vb;
    int a_known = uir_bv_to_u64(a, &va);
    int b_known = uir_bv_to_u64(b, &vb);

    /* Replication width = operand_a width × replication count */
    if (op == UIR_OP_REPLICATE) {
        if (!b_known || vb == 0) return x_bv(wa > wb ? wa : wb);
        w = wa * (uint32_t)vb;
        if (w > 65536) w = 65536; /* sanity limit */
    }

    /* For arithmetic/comparison, X in either operand → X result */
    switch (op) {
        case UIR_OP_ADD:
        case UIR_OP_SUB:
        case UIR_OP_MUL:
        case UIR_OP_DIV:
        case UIR_OP_MOD:
        case UIR_OP_LT:
        case UIR_OP_GT:
        case UIR_OP_LE:
        case UIR_OP_GE:
        case UIR_OP_SHL:
        case UIR_OP_SHR:
            if (!a_known || !b_known) return x_bv(w);
            break;
        default:
            break;
    }

    qsim_bit_vector_t *r = qsim_bit_vector_alloc(w);
    if (!r) return NULL;

    switch (op) {
        case UIR_OP_ADD: {
            uint64_t vr = va + vb;
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_SUB: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint64_t vr = va - vb;
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_MUL: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint64_t vr = va * vb;
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_DIV: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint64_t vr = (vb == 0) ? 0 : (va / vb);
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_MOD: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint64_t vr = (vb == 0) ? 0 : (va % vb);
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_AND: {
            for (uint32_t i = 0; i < w; i++) {
                qsim_value_t ba = i < wa ? qsim_bit_get(a, i) : QSIM_VAL_0;
                qsim_value_t bb = i < wb ? qsim_bit_get(b, i) : QSIM_VAL_0;
                /* 0 dominates AND: 0 & anything = 0 */
                if (ba.state == QSIM_0 || bb.state == QSIM_0) {
                    qsim_bit_set(r, i, QSIM_VAL_0);
                } else if (ba.state == QSIM_1 && bb.state == QSIM_1) {
                    qsim_bit_set(r, i, QSIM_VAL_1);
                } else {
                    qsim_bit_set(r, i, QSIM_VAL_X);
                }
            }
            break;
        }
        case UIR_OP_OR: {
            for (uint32_t i = 0; i < w; i++) {
                qsim_value_t ba = i < wa ? qsim_bit_get(a, i) : QSIM_VAL_0;
                qsim_value_t bb = i < wb ? qsim_bit_get(b, i) : QSIM_VAL_0;
                /* 1 dominates OR: 1 | anything = 1 */
                if (ba.state == QSIM_1 || bb.state == QSIM_1) {
                    qsim_bit_set(r, i, QSIM_VAL_1);
                } else if (ba.state == QSIM_0 && bb.state == QSIM_0) {
                    qsim_bit_set(r, i, QSIM_VAL_0);
                } else {
                    qsim_bit_set(r, i, QSIM_VAL_X);
                }
            }
            break;
        }
        case UIR_OP_XOR: {
            for (uint32_t i = 0; i < w; i++) {
                qsim_value_t ba = i < wa ? qsim_bit_get(a, i) : QSIM_VAL_0;
                qsim_value_t bb = i < wb ? qsim_bit_get(b, i) : QSIM_VAL_0;
                if (ba.state == QSIM_X || ba.state == QSIM_Z ||
                    bb.state == QSIM_X || bb.state == QSIM_Z) {
                    qsim_bit_set(r, i, QSIM_VAL_X);
                } else if (ba.state != bb.state) {
                    qsim_bit_set(r, i, QSIM_VAL_1);
                } else {
                    qsim_bit_set(r, i, QSIM_VAL_0);
                }
            }
            break;
        }
        case UIR_OP_EQ: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            qsim_bit_set(r, 0, (va == vb) ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_NEQ: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            qsim_bit_set(r, 0, (va != vb) ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_LT: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            int less;
            if (is_signed)
                less = (int64_t)va < (int64_t)vb;
            else
                less = va < vb;
            qsim_bit_set(r, 0, less ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_GT: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            int greater;
            if (is_signed)
                greater = (int64_t)va > (int64_t)vb;
            else
                greater = va > vb;
            qsim_bit_set(r, 0, greater ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_LE: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            int le;
            if (is_signed)
                le = (int64_t)va <= (int64_t)vb;
            else
                le = va <= vb;
            qsim_bit_set(r, 0, le ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_GE: {
            if (!a_known || !b_known) { qsim_bit_vector_free(r); return x_bv(1); }
            int ge;
            if (is_signed)
                ge = (int64_t)va >= (int64_t)vb;
            else
                ge = va >= vb;
            qsim_bit_set(r, 0, ge ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_SHL: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sl = (uint32_t)vb;
            for (uint32_t i = 0; i < w; i++) {
                if (i >= sl && (i - sl) < wa)
                    qsim_bit_set(r, i, qsim_bit_get(a, i - sl));
                else
                    qsim_bit_set(r, i, QSIM_VAL_0);
            }
            break;
        }
        case UIR_OP_SHR: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sr = (uint32_t)vb;
            for (uint32_t i = 0; i < w; i++) {
                uint32_t src = i + sr;
                qsim_bit_set(r, i, (src < wa) ? qsim_bit_get(a, src) : QSIM_VAL_0);
            }
            break;
        }
        case UIR_OP_SRL: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sr = (uint32_t)vb;
            for (uint32_t i = 0; i < w; i++) {
                uint32_t src = i + sr;
                qsim_bit_set(r, i, (src < wa) ? qsim_bit_get(a, src) : QSIM_VAL_0);
            }
            break;
        }
        case UIR_OP_SLL: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sl = (uint32_t)vb;
            for (uint32_t i = 0; i < w; i++) {
                if (i >= sl && (i - sl) < wa)
                    qsim_bit_set(r, i, qsim_bit_get(a, i - sl));
                else
                    qsim_bit_set(r, i, QSIM_VAL_0);
            }
            break;
        }
        case UIR_OP_SRA: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sa = (uint32_t)vb;
            qsim_value_t sign = qsim_bit_get(a, wa > 0 ? wa - 1 : 0);
            for (uint32_t i = 0; i < w; i++) {
                uint32_t src = i + sa;
                qsim_bit_set(r, i, (src < wa) ? qsim_bit_get(a, src) : sign);
            }
            break;
        }
        case UIR_OP_SLA: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t sla = (uint32_t)vb;
            qsim_value_t fill = qsim_bit_get(a, 0);
            for (uint32_t i = 0; i < w; i++) {
                if (i >= sla && (i - sla) < wa)
                    qsim_bit_set(r, i, qsim_bit_get(a, i - sla));
                else
                    qsim_bit_set(r, i, fill);
            }
            break;
        }
        case UIR_OP_ROL: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t rot = wa > 0 ? ((uint32_t)vb % wa) : 0;
            for (uint32_t i = 0; i < wa; i++) {
                uint32_t src = (i + rot) % wa;
                qsim_bit_set(r, i, qsim_bit_get(a, src));
            }
            for (uint32_t i = wa; i < w; i++)
                qsim_bit_set(r, i, QSIM_VAL_0);
            break;
        }
        case UIR_OP_ROR: {
            if (!b_known) { qsim_bit_vector_free(r); return x_bv(w); }
            uint32_t rot = wa > 0 ? ((uint32_t)vb % wa) : 0;
            for (uint32_t i = 0; i < wa; i++) {
                uint32_t src = (i + wa - rot) % wa;
                qsim_bit_set(r, i, qsim_bit_get(a, src));
            }
            for (uint32_t i = wa; i < w; i++)
                qsim_bit_set(r, i, QSIM_VAL_0);
            break;
        }
        case UIR_OP_CONCAT: {
            for (uint32_t i = 0; i < wa; i++) {
                qsim_bit_set(r, i, qsim_bit_get(a, i));
            }
            for (uint32_t i = 0; i < wb; i++) {
                qsim_bit_set(r, wa + i, qsim_bit_get(b, i));
            }
            break;
        }
        case UIR_OP_REPLICATE: {
            if (!b_known || vb == 0) { qsim_bit_vector_free(r); return x_bv(w); }
            for (uint32_t i = 0; i < vb && i < 4096; i++) {
                for (uint32_t j = 0; j < wa && (i * wa + j) < w; j++) {
                    qsim_bit_set(r, i * wa + j, qsim_bit_get(a, j));
                }
            }
            break;
        }
        default: {
            qsim_bit_vector_free(r);
            return x_bv(w);
        }
    }
    return r;
}

static qsim_bit_vector_t *bv_unary_op(const qsim_bit_vector_t *a, uir_unary_op_t op) {
    uint32_t w = a->width;
    qsim_bit_vector_t *r = qsim_bit_vector_alloc(w);
    if (!r) return NULL;

    switch (op) {
        case UIR_OP_NOT:
            for (uint32_t i = 0; i < w; i++) {
                qsim_value_t bit = qsim_bit_get(a, i);
                if (bit.state == QSIM_X || bit.state == QSIM_Z)
                    qsim_bit_set(r, i, QSIM_VAL_X);
                else if (bit.state == QSIM_1)
                    qsim_bit_set(r, i, QSIM_VAL_0);
                else
                    qsim_bit_set(r, i, QSIM_VAL_1);
            }
            break;
        case UIR_OP_NEG: {
            uint64_t va;
            if (!uir_bv_to_u64(a, &va)) { qsim_bit_vector_free(r); return x_bv(w); }
            uint64_t vr = (~va + 1) & ((1ULL << (w < 64 ? w : 64)) - 1);
            for (uint32_t i = 0; i < w; i++)
                qsim_bit_set(r, i, (vr >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
            break;
        }
        case UIR_OP_REDUCE_AND: {
            /* Result = 1 if all bits are 1; 0 if any bit is 0; X otherwise */
            int has_x = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_0) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_0); }
                if (s == QSIM_X || s == QSIM_Z) has_x = 1;
            }
            qsim_value_t res = has_x ? QSIM_VAL_X : QSIM_VAL_1;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        case UIR_OP_REDUCE_NAND: {
            /* Result = 0 if all bits are 1; 1 if any bit is 0; X otherwise */
            int has_x = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_0) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_1); }
                if (s == QSIM_X || s == QSIM_Z) has_x = 1;
            }
            qsim_value_t res = has_x ? QSIM_VAL_X : QSIM_VAL_0;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        case UIR_OP_REDUCE_OR: {
            /* Result = 1 if any bit is 1; 0 if all bits are 0; X otherwise */
            int has_x = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_1) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_1); }
                if (s == QSIM_X || s == QSIM_Z) has_x = 1;
            }
            qsim_value_t res = has_x ? QSIM_VAL_X : QSIM_VAL_0;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        case UIR_OP_REDUCE_NOR: {
            /* Result = 0 if any bit is 1; 1 if all bits are 0; X otherwise */
            int has_x = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_1) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_0); }
                if (s == QSIM_X || s == QSIM_Z) has_x = 1;
            }
            qsim_value_t res = has_x ? QSIM_VAL_X : QSIM_VAL_1;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        case UIR_OP_REDUCE_XOR: {
            /* Result = 1 if odd number of 1s; 0 if even; X if any X/Z */
            int ones = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_X || s == QSIM_Z) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_X); }
                if (s == QSIM_1) ones++;
            }
            qsim_value_t res = (ones & 1) ? QSIM_VAL_1 : QSIM_VAL_0;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        case UIR_OP_REDUCE_XNOR: {
            /* Result = 0 if odd number of 1s; 1 if even; X if any X/Z */
            int ones = 0;
            for (uint32_t i = 0; i < w; i++) {
                qsim_logic_state_t s = qsim_bit_get(a, i).state;
                if (s == QSIM_X || s == QSIM_Z) { qsim_bit_vector_free(r); return qsim_bit_vector_from_state(1, QSIM_X); }
                if (s == QSIM_1) ones++;
            }
            qsim_value_t res = (ones & 1) ? QSIM_VAL_0 : QSIM_VAL_1;
            qsim_bit_vector_free(r);
            r = qsim_bit_vector_alloc(1);
            if (r) qsim_bit_set(r, 0, res);
            return r;
        }
        default:
            qsim_bit_vector_free(r);
            return x_bv(w);
    }
    return r;
}

/* ── Signal table management ── */

static int add_signal(uir_sim_context_t *ctx, uir_node_t *node, const char *name) {
    if (ctx->signal_count >= ctx->signal_cap) {
        size_t new_cap = ctx->signal_cap ? ctx->signal_cap * 2 : 128;
        sim_signal_t *ns = realloc(ctx->signals, new_cap * sizeof(sim_signal_t));
        if (!ns) return 0;
        ctx->signals = ns;
        ctx->signal_cap = new_cap;
    }
    sim_signal_t *s = &ctx->signals[ctx->signal_count++];
    s->node = node;
    snprintf(s->name, sizeof(s->name), "%s", name);

    /* Initialize signal value — use init_value from UIR node if set,
     * otherwise default to X (uninitialized for std_logic, appropriate for 'U'). */
    uint32_t width = 1;
    s->is_signed = 0;
    qsim_logic_state_t init_state = QSIM_X;
    if (node->kind == UIR_SIGNAL) {
        uir_signal_t *sig = (uir_signal_t *)node;
        width = sig->width;
        if (sig->array_size > 0) width *= sig->array_size;
        s->is_signed = sig->is_signed;
        init_state = sig->init_value.state;
    } else if (node->kind == UIR_PORT) {
        uir_port_t *p = (uir_port_t *)node;
        width = p->width;
        s->is_signed = p->is_signed;
        init_state = p->init_value.state;
    }
    s->value = qsim_bit_vector_from_state(width, init_state);
    s->prev_value = qsim_bit_vector_clone(s->value);
    s->net_group = NET_GROUP_WIRE;
    s->driver_count = 0;
    s->needs_resolve = 0;
    for (int _di = 0; _di < MAX_DRIVERS_PER_SIGNAL; _di++) {
        s->drivers[_di].process_id = -1;
        s->drivers[_di].value = NULL;
    }

    /* Map UIR signal type to net group for resolution */
    if (node->kind == UIR_SIGNAL) {
        uir_signal_t *sig = (uir_signal_t *)node;
        switch (sig->sig_type) {
            case UIR_SIG_WIRE: case UIR_SIG_TRI:  case UIR_SIG_UWIRE:
            case UIR_SIG_EVENT:
            case UIR_SIG_LOGIC:
                break; /* NET_GROUP_WIRE default */
            case UIR_SIG_VHDL_SIGNAL:
                s->net_group = NET_GROUP_STD_LOGIC; break;
            case UIR_SIG_VHDL_VARIABLE:
                break; /* NET_GROUP_WIRE default — variables are process-local */
            case UIR_SIG_WAND: case UIR_SIG_TRIAND:
                s->net_group = NET_GROUP_WAND; break;
            case UIR_SIG_WOR:  case UIR_SIG_TRIOR:
                s->net_group = NET_GROUP_WOR; break;
            case UIR_SIG_TRI0:
                s->net_group = NET_GROUP_TRI0; break;
            case UIR_SIG_TRI1:
                s->net_group = NET_GROUP_TRI1; break;
            case UIR_SIG_SUPPLY0:
                s->net_group = NET_GROUP_SUPPLY0;
                qsim_bit_vector_free(s->value);
                s->value = qsim_bit_vector_from_state(width, QSIM_0);
                break;
            case UIR_SIG_SUPPLY1:
                s->net_group = NET_GROUP_SUPPLY1;
                qsim_bit_vector_free(s->value);
                s->value = qsim_bit_vector_from_state(width, QSIM_1);
                break;
            case UIR_SIG_TRIREG:
                s->net_group = NET_GROUP_TRIREG; break;
            default: break;
        }
    }
    signal_ht_insert(ctx, (int)(ctx->signal_count - 1));
    return 1;
}

static int find_signal_idx(uir_sim_context_t *ctx, const char *name) {
    /* Use hash table if available (O(1)), fall back to linear scan. */
    if (ctx->sig_ht_ready) {
        int idx = signal_ht_lookup(ctx, name);
        if (idx >= 0) return idx;
        /* Hash miss — fall through to linear scan for robustness
         * (e.g., signals added dynamically before ht was rehashed). */
    }
    int best = -1;
    uint32_t best_w = 0;
    for (size_t i = 0; i < ctx->signal_count; i++) {
        if (strcmp(ctx->signals[i].name, name) == 0) {
            uint32_t w = ctx->signals[i].value->width;
            if (best < 0 || w > best_w) { best = (int)i; best_w = w; }
        }
    }
    if (best >= 0) return best;
    /* VHDL alias fallback: check if name matches an alias entry */
    if (ctx->alias_count > 0) {
        for (size_t ai = 0; ai < ctx->alias_count; ai++) {
            if (ctx->alias_names[ai] && strcmp(ctx->alias_names[ai], name) == 0)
                return ctx->alias_targets[ai];
        }
    }
    return -1;
}

/* ── Signal hash table (open-addressing, linear probing) ── */

static void signal_ht_init(uir_sim_context_t *ctx) {
    size_t cap = 64;
    while (cap < ctx->signal_count * 2) cap *= 2;
    ctx->sig_ht = calloc(cap, sizeof(signal_ht_entry_t));
    if (!ctx->sig_ht) return;
    ctx->sig_ht_cap = cap;
    ctx->sig_ht_count = 0;

    /* Mark all slots as empty (sig_idx = -1). calloc zeroes, but -1 is not 0
     * on all representations; explicitly initialize. */
    for (size_t i = 0; i < cap; i++)
        ctx->sig_ht[i].sig_idx = -1;

    for (size_t i = 0; i < ctx->signal_count; i++)
        signal_ht_insert(ctx, (int)i);

    ctx->sig_ht_ready = 1;
}

static int signal_ht_lookup(uir_sim_context_t *ctx, const char *name) {
    if (!ctx->sig_ht || ctx->sig_ht_cap == 0) return -1;
    uint32_t h = sig_ht_hash(name);
    size_t mask = ctx->sig_ht_cap - 1;
    size_t slot = h & mask;
    for (size_t i = 0; i < ctx->sig_ht_cap; i++) {
        signal_ht_entry_t *e = &ctx->sig_ht[(slot + i) & mask];
        if (e->sig_idx < 0) return -1;           /* empty slot → not found */
        if (e->hash == h && strcmp(ctx->signals[e->sig_idx].name, name) == 0)
            return e->sig_idx;
    }
    return -1; /* table full (shouldn't happen with load-factor rehashing) */
}

static void signal_ht_insert(uir_sim_context_t *ctx, int sig_idx) {
    if (!ctx->sig_ht || ctx->sig_ht_cap == 0) return;
    /* Rehash if load > 2/3 */
    if (ctx->sig_ht_count * 3 > ctx->sig_ht_cap * 2) {
        size_t old_cap = ctx->sig_ht_cap;
        signal_ht_entry_t *old_tab = ctx->sig_ht;
        size_t new_cap = old_cap * 2;
        ctx->sig_ht = calloc(new_cap, sizeof(signal_ht_entry_t));
        if (!ctx->sig_ht) { ctx->sig_ht = old_tab; return; }
        ctx->sig_ht_cap = new_cap;
        ctx->sig_ht_count = 0;
        for (size_t i = 0; i < new_cap; i++) ctx->sig_ht[i].sig_idx = -1;
        /* Rehash all old entries */
        for (size_t i = 0; i < old_cap; i++)
            if (old_tab[i].sig_idx >= 0)
                signal_ht_insert(ctx, old_tab[i].sig_idx);
        free(old_tab);
    }

    const char *name = ctx->signals[sig_idx].name;
    uint32_t h = sig_ht_hash(name);
    size_t mask = ctx->sig_ht_cap - 1;
    size_t slot = h & mask;
    for (size_t i = 0; i < ctx->sig_ht_cap; i++) {
        signal_ht_entry_t *e = &ctx->sig_ht[(slot + i) & mask];
        if (e->sig_idx < 0) {
            e->sig_idx = sig_idx;
            e->hash = h;
            ctx->sig_ht_count++;
            return;
        }
        /* Duplicate name: keep the signal with larger width (e.g., prefer a 5-bit
         * reg over a 1-bit implicit wire created by forward reference). */
        if (e->hash == h && strcmp(ctx->signals[e->sig_idx].name, name) == 0) {
            uint32_t cur_w = ctx->signals[e->sig_idx].value->width;
            uint32_t new_w = ctx->signals[sig_idx].value->width;
            if (new_w > cur_w)
                e->sig_idx = sig_idx;
            return;
        }
    }
}

/* Collect signals from a unit (recursively via instances) */
static void collect_signals(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                             const char *prefix, int depth) {
    if (depth > 64) return;

    char path[512];

    for (size_t i = 0; i < unit->port_count; i++) {
        if (prefix[0])
            snprintf(path, sizeof(path), "%s.%s", prefix, unit->ports[i]->name);
        else
            snprintf(path, sizeof(path), "%s", unit->ports[i]->name);
        add_signal(ctx, (uir_node_t *)unit->ports[i], path);
    }
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (prefix[0])
            snprintf(path, sizeof(path), "%s.%s", prefix, unit->signals[i]->name);
        else
            snprintf(path, sizeof(path), "%s", unit->signals[i]->name);
        add_signal(ctx, (uir_node_t *)unit->signals[i], path);
    }

    /* Recurse into bound instances */
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;
        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);
        collect_signals(ctx, inst->bound_to, child_prefix, depth + 1);
    }
}

/* Collect processes from all units, with hierarchical prefix */
static void add_processes(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                           const char *prefix) {
    for (size_t i = 0; i < unit->process_count; i++) {
        if (ctx->process_count >= ctx->process_cap) {
            size_t new_cap = ctx->process_cap ? ctx->process_cap * 2 : 32;
            uir_process_t **np = realloc(ctx->processes, new_cap * sizeof(uir_process_t *));
            char (*pp)[256] = realloc(ctx->process_prefixes, new_cap * 256);
            if (!np || !pp) return;
            ctx->processes = np;
            ctx->process_prefixes = pp;
            ctx->process_cap = new_cap;
        }
        ctx->processes[ctx->process_count] = unit->processes[i];
        strncpy(ctx->process_prefixes[ctx->process_count], prefix, 255);
        ctx->process_prefixes[ctx->process_count][255] = '\0';
        ctx->process_count++;
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
        add_processes(ctx, inst->bound_to, child_prefix);
    }
}

/* ── Recursive ref finder for dependency tracking ── */

static void find_refs_in_node(uir_node_t *node, uir_sim_context_t *ctx,
                                int **sigs, size_t *count, size_t *cap) {
    if (!node) return;
    int idx;
    qsim_bit_vector_t *val;
    switch (node->kind) {
        case UIR_REF: {
            uir_ref_t *ref = (uir_ref_t *)node;
            /* Traverse array index and part-select expressions */
            if (ref->index)
                find_refs_in_node(ref->index, ctx, sigs, count, cap);
            if (ref->part_hi)
                find_refs_in_node(ref->part_hi, ctx, sigs, count, cap);
            if (ref->part_lo)
                find_refs_in_node(ref->part_lo, ctx, sigs, count, cap);
            for (size_t _mi = 0; _mi < ref->multi_idx_count; _mi++)
                find_refs_in_node(ref->multi_index[_mi], ctx, sigs, count, cap);
            idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                idx = find_signal_idx(ctx, prefixed);
            }
            if (idx < 0) idx = find_signal_idx(ctx, ref->name);
            if (idx >= 0) {
                /* Deduplicate */
                for (size_t i = 0; i < *count; i++)
                    if ((*sigs)[i] == idx) break;
                if (*count == 0 || (*sigs)[*count - 1] != idx) {
                    if (*count >= *cap) {
                        *cap = *cap ? *cap * 2 : 8;
                        *sigs = realloc(*sigs, *cap * sizeof(int));
                        if (!*sigs) { *count = 0; *cap = 0; return; }
                    }
                    (*sigs)[(*count)++] = idx;
                }
            }
            break;
        }
        case UIR_EXPR_BINARY:
        case UIR_EXPR_UNARY: {
            uir_expr_t *expr = (uir_expr_t *)node;
            find_refs_in_node(expr->operand_a, ctx, sigs, count, cap);
            find_refs_in_node(expr->operand_b, ctx, sigs, count, cap);
            break;
        }
        case UIR_COND: {
            uir_cond_t *cond = (uir_cond_t *)node;
            find_refs_in_node(cond->condition, ctx, sigs, count, cap);
            find_refs_in_node(cond->then_expr, ctx, sigs, count, cap);
            find_refs_in_node(cond->else_expr, ctx, sigs, count, cap);
            break;
        }
        case UIR_LITERAL: break;
        case UIR_BLOCK: {
            uir_block_t *block = (uir_block_t *)node;
            for (size_t i = 0; i < block->stmt_count; i++)
                find_refs_in_node(block->stmts[i], ctx, sigs, count, cap);
            break;
        }
        case UIR_ASSIGN: {
            uir_assign_t *a = (uir_assign_t *)node;
            find_refs_in_node(a->lhs, ctx, sigs, count, cap);
            find_refs_in_node(a->rhs, ctx, sigs, count, cap);
            break;
        }
        case UIR_IF: {
            uir_if_t *ifn = (uir_if_t *)node;
            find_refs_in_node(ifn->condition, ctx, sigs, count, cap);
            find_refs_in_node(ifn->then_branch, ctx, sigs, count, cap);
            find_refs_in_node(ifn->else_branch, ctx, sigs, count, cap);
            break;
        }
        case UIR_CASE: {
            uir_case_t *c = (uir_case_t *)node;
            find_refs_in_node(c->expr, ctx, sigs, count, cap);
            for (size_t i = 0; i < c->item_count; i++) {
                uir_case_item_t *item = c->items[i];
                for (size_t j = 0; j < item->pattern_count; j++)
                    find_refs_in_node(item->patterns[j], ctx, sigs, count, cap);
                find_refs_in_node(item->body, ctx, sigs, count, cap);
            }
            find_refs_in_node(c->default_item, ctx, sigs, count, cap);
            break;
        }
        case UIR_CASE_ITEM: {
            uir_case_item_t *item = (uir_case_item_t *)node;
            for (size_t j = 0; j < item->pattern_count; j++)
                find_refs_in_node(item->patterns[j], ctx, sigs, count, cap);
            find_refs_in_node(item->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_FUNC_CALL:
        case UIR_TASK_ENABLE: {
            uir_func_call_t *fc = (uir_func_call_t *)node;
            for (size_t i = 0; i < fc->arg_count; i++)
                find_refs_in_node(fc->args[i], ctx, sigs, count, cap);
            break;
        }
        case UIR_FUNC_DEF:
        case UIR_TASK_DEF: {
            uir_func_t *ft = (uir_func_t *)node;
            find_refs_in_node(ft->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_SYS_TASK: {
            uir_sys_task_t *t = (uir_sys_task_t *)node;
            for (size_t i = 0; i < t->arg_count; i++)
                find_refs_in_node(t->args[i], ctx, sigs, count, cap);
            break;
        }
        case UIR_SYS_FUNC_EXPR: {
            uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)node;
            for (size_t i = 0; i < sf->arg_count; i++)
                find_refs_in_node(sf->args[i], ctx, sigs, count, cap);
            break;
        }
        case UIR_DELAY: {
            uir_delay_t *d = (uir_delay_t *)node;
            find_refs_in_node(d->delay_value, ctx, sigs, count, cap);
            find_refs_in_node(d->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_LOOP: {
            uir_loop_t *l = (uir_loop_t *)node;
            find_refs_in_node(l->init_stmt, ctx, sigs, count, cap);
            find_refs_in_node(l->condition, ctx, sigs, count, cap);
            find_refs_in_node(l->step_stmt, ctx, sigs, count, cap);
            find_refs_in_node(l->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_LOOP_BACK: {
            uir_loop_back_t *lb = (uir_loop_back_t *)node;
            find_refs_in_node(lb->condition, ctx, sigs, count, cap);
            /* UIR_LOOP_BACK.body points back to the loop's augmented block,
             * forming a cycle. Skip body to avoid infinite recursion. */
            break;
        }
        case UIR_WAIT: {
            uir_wait_t *w = (uir_wait_t *)node;
            find_refs_in_node(w->condition, ctx, sigs, count, cap);
            find_refs_in_node(w->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_EVENT_CTRL: {
            uir_event_ctrl_t *ec = (uir_event_ctrl_t *)node;
            if (ec->signal_name) {
                int idx = find_signal_idx(ctx, ec->signal_name);
                if (idx >= 0) {
                    if (*count >= *cap) {
                        *cap = *cap ? *cap * 2 : 8;
                        *sigs = realloc(*sigs, *cap * sizeof(int));
                        if (!*sigs) { *count = 0; *cap = 0; return; }
                    }
                    (*sigs)[(*count)++] = idx;
                }
            }
            find_refs_in_node(ec->body, ctx, sigs, count, cap);
            break;
        }
        case UIR_DISABLE:
            /* No signal references in disable statement */
            break;
        case UIR_EXIT:
        case UIR_NEXT:
        case UIR_RETURN:
            /* No signal references in these statements */
            break;
        case UIR_FORCE: {
            uir_force_t *f = (uir_force_t *)node;
            find_refs_in_node(f->lhs, ctx, sigs, count, cap);
            find_refs_in_node(f->rhs, ctx, sigs, count, cap);
            break;
        }
        case UIR_RELEASE: {
            uir_release_t *r = (uir_release_t *)node;
            find_refs_in_node(r->target, ctx, sigs, count, cap);
            break;
        }
        default: break;
    }
}

/* Collect continuous assigns from all units */

/* Forward declarations */
static void schedule_event(uir_sim_context_t *ctx, uint64_t time, uint32_t delta,
                            uint32_t sig_idx, qsim_bit_vector_t *value, int is_nba,
                            int ps_lo, int ps_hi);
static qsim_bit_vector_t *eval_expr(uir_sim_context_t *ctx, uir_node_t *node);
static void exec_stmt(uir_sim_context_t *ctx, uir_node_t *stmt);

/* Forward declarations for TEXTIO (TEXTIO functions are defined later) */
static int textio_endfile(uir_sim_context_t *ctx, uir_func_call_t *tc);

/* Forward declarations for UDP evaluation (defined later in the file) */
static int signal_write_resolved(uir_sim_context_t *ctx, uint32_t sig_idx,
                                  qsim_bit_vector_t *new_val,
                                  qsim_bit_vector_t **old_val_out,
                                  int process_id);
static void check_and_trigger(uir_sim_context_t *ctx, uint32_t sig_idx,
                               const qsim_bit_vector_t *old_val,
                               const qsim_bit_vector_t *new_val);
static void evaluate_sequential_udp(uir_sim_context_t *ctx, udp_instance_entry_t *entry,
                                     const qsim_bit_vector_t *old_clock,
                                     const qsim_bit_vector_t *new_clock);

/* ── IEEE numeric_std builtin dispatch ── */

static int match_numeric_std_builtin(const char *name) {
    if (!name) return UIR_NUMERIC_STD_NONE;
    if (strcmp(name, "unsigned") == 0) return UIR_NUMERIC_STD_UNSIGNED;
    if (strcmp(name, "signed") == 0) return UIR_NUMERIC_STD_SIGNED;
    if (strcmp(name, "to_integer") == 0) return UIR_NUMERIC_STD_TO_INTEGER;
    if (strcmp(name, "to_unsigned") == 0) return UIR_NUMERIC_STD_TO_UNSIGNED;
    if (strcmp(name, "to_signed") == 0) return UIR_NUMERIC_STD_TO_SIGNED;
    if (strcmp(name, "std_logic_vector") == 0) return UIR_NUMERIC_STD_STD_LOGIC_VECTOR;
    if (strcmp(name, "shift_left") == 0) return UIR_NUMERIC_STD_SHIFT_LEFT;
    if (strcmp(name, "shift_right") == 0) return UIR_NUMERIC_STD_SHIFT_RIGHT;
    return UIR_NUMERIC_STD_NONE;
}

/* Evaluate a numeric_std builtin function (type conversion or query). */
static qsim_bit_vector_t *numeric_std_eval_func(uir_sim_context_t *ctx, int kind, uir_func_call_t *fc) {
    /* All these builtins require at least one argument */
    if (fc->arg_count < 1 || !fc->args[0]) return NULL;

    qsim_bit_vector_t *arg_val = eval_expr(ctx, fc->args[0]);
    if (!arg_val) return NULL;

    switch (kind) {
        case UIR_NUMERIC_STD_UNSIGNED:
        case UIR_NUMERIC_STD_SIGNED:
        case UIR_NUMERIC_STD_STD_LOGIC_VECTOR:
            /* Type re-interpretation: same bits, different type label.
             * Return the bit vector as-is. */
            return arg_val;

        case UIR_NUMERIC_STD_TO_INTEGER: {
            /* Convert bit vector to integer (32-bit).
             * Zero-extend if narrower, truncate if wider. */
            uint32_t w = arg_val->width;
            qsim_bit_vector_t *r = qsim_bit_vector_from_state(32, QSIM_0);
            uint32_t copy = w < 32 ? w : 32;
            for (uint32_t i = 0; i < copy; i++)
                r->bits[i] = arg_val->bits[i];
            qsim_bit_vector_free(arg_val);
            return r;
        }

        case UIR_NUMERIC_STD_TO_UNSIGNED:
        case UIR_NUMERIC_STD_TO_SIGNED: {
            /* to_unsigned(value, size) / to_signed(value, size):
             * Convert integer bit vector to specified width.
             * First arg is value, second arg (if present) is size as literal.
             * If no second arg, default to 32. */
            uint32_t width = 32;
            if (fc->arg_count >= 2 && fc->args[1]) {
                qsim_bit_vector_t *size_val = eval_expr(ctx, fc->args[1]);
                if (size_val) {
                    /* Extract width from the literal bits of the second arg.
                     * The second arg is typically a literal number. */
                    uint32_t w = 0;
                    for (uint32_t i = 0; i < size_val->width && i < 32; i++) {
                        if (size_val->bits[i].state == QSIM_1)
                            w |= (1u << i);
                    }
                    if (w > 0 && w <= 1024) width = w;
                    qsim_bit_vector_free(size_val);
                }
            }
            qsim_bit_vector_t *r = qsim_bit_vector_from_state(width, QSIM_0);
            uint32_t copy = arg_val->width < width ? arg_val->width : width;
            for (uint32_t i = 0; i < copy; i++)
                r->bits[i] = arg_val->bits[i];
            qsim_bit_vector_free(arg_val);
            return r;
        }

        case UIR_NUMERIC_STD_SHIFT_LEFT:
        case UIR_NUMERIC_STD_SHIFT_RIGHT: {
            /* shift_left(arg, count) / shift_right(arg, count)
             * Shift bit vector by count positions, fill with '0'.
             * Result has same width as arg. */
            uint32_t arg_width = arg_val->width;
            uint32_t shift = 0;
            if (fc->arg_count >= 2 && fc->args[1]) {
                qsim_bit_vector_t *count_val = eval_expr(ctx, fc->args[1]);
                if (count_val) {
                    for (uint32_t i = 0; i < count_val->width && i < 32; i++) {
                        if (count_val->bits[i].state == QSIM_1)
                            shift |= (1u << i);
                        else if (count_val->bits[i].state != QSIM_0) {
                            /* X/Z shift count: return all X */
                            shift = UINT32_MAX;
                            qsim_bit_vector_free(count_val);
                            break;
                        }
                    }
                    if (shift != UINT32_MAX)
                        qsim_bit_vector_free(count_val);
                }
            }
            if (shift >= arg_width) shift = arg_width;
            int shift_right = (kind == UIR_NUMERIC_STD_SHIFT_RIGHT);
            qsim_bit_vector_t *r = qsim_bit_vector_from_state(arg_width, QSIM_0);
            if (shift != UINT32_MAX) {
                for (uint32_t i = 0; i < arg_width; i++) {
                    uint32_t src = shift_right ? (i + shift) : (i >= shift ? i - shift : UINT32_MAX);
                    if (src < arg_width) {
                        r->bits[i] = arg_val->bits[src];
                    } else {
                        /* Fill with '0' (already set from qsim_bit_vector_from_state) */
                    }
                }
            } else {
                /* X/Z shift count: all bits become X */
                for (uint32_t i = 0; i < arg_width; i++)
                    r->bits[i].state = QSIM_X;
            }
            qsim_bit_vector_free(arg_val);
            return r;
        }

        default:
            qsim_bit_vector_free(arg_val);
            return NULL;
    }
}

/* ── VITAL primitive / timing check builtin dispatch ── */

static int match_vital_builtin(const char *name) {
    if (!name) return UIR_VITAL_NONE;
    if (strcmp(name, "vitaland") == 0) return UIR_VITAL_AND;
    if (strcmp(name, "vitalor") == 0) return UIR_VITAL_OR;
    if (strcmp(name, "vitalxor") == 0) return UIR_VITAL_XOR;
    if (strcmp(name, "vitalnand") == 0) return UIR_VITAL_NAND;
    if (strcmp(name, "vitalnor") == 0) return UIR_VITAL_NOR;
    if (strcmp(name, "vitalxnor") == 0) return UIR_VITAL_XNOR;
    if (strcmp(name, "vitalbuf") == 0) return UIR_VITAL_BUF;
    if (strcmp(name, "vitalinv") == 0) return UIR_VITAL_INV;
    if (strcmp(name, "vitalident") == 0) return UIR_VITAL_IDENT;
    if (strcmp(name, "vitallevel") == 0) return UIR_VITAL_LEVEL;
    if (strcmp(name, "vitalsetupholdcheck") == 0) return UIR_VITAL_SETUPHOLDCHECK;
    if (strcmp(name, "vitalwidthcheck") == 0) return UIR_VITAL_WIDTHCHECK;
    if (strcmp(name, "vitalperiodcheck") == 0) return UIR_VITAL_PERIODCHECK;
    if (strcmp(name, "vitalrecoveryremovalcheck") == 0) return UIR_VITAL_RECOVERYCHECK;
    return UIR_VITAL_NONE;
}

/* Evaluate a VITAL primitive function (called from UIR_FUNC_CALL dispatch).
 * Last arg is the Result signal ref; all preceding args are data inputs. */
static qsim_bit_vector_t *vital_eval_func(uir_sim_context_t *ctx, int kind, uir_func_call_t *fc) {
    size_t data_count = fc->arg_count > 0 ? fc->arg_count - 1 : 0;

    /* Find result signal index from last arg */
    int result_sig_idx = -1;
    if (fc->arg_count > 0 && fc->args[fc->arg_count - 1] &&
        fc->args[fc->arg_count - 1]->kind == UIR_REF) {
        uir_ref_t *ref = (uir_ref_t *)fc->args[fc->arg_count - 1];
        result_sig_idx = find_signal_idx(ctx, ref->name);
    }

    /* Concatenate all data args into one flat vector */
    qsim_bit_vector_t *data = NULL;
    uint32_t total_bits = 0;

    for (size_t i = 0; i < data_count; i++) {
        qsim_bit_vector_t *val = eval_expr(ctx, fc->args[i]);
        if (!val) continue;
        if (!data) {
            data = qsim_bit_vector_clone(val);
            total_bits = val->width;
        } else {
            uint32_t old_w = total_bits;
            qsim_bit_vector_t *tmp = qsim_bit_vector_alloc(old_w + val->width);
            for (uint32_t b = 0; b < old_w; b++)
                qsim_bit_set(tmp, b, qsim_bit_get(data, b));
            for (uint32_t b = 0; b < val->width; b++)
                qsim_bit_set(tmp, old_w + b, qsim_bit_get(val, b));
            qsim_bit_vector_free(data);
            data = tmp;
            total_bits = old_w + val->width;
        }
        qsim_bit_vector_free(val);
    }

    if (!data) data = qsim_bit_vector_from_state(1, QSIM_X);

    /* VitalLevel: convert to 0/1, no boolean reduction */
    if (kind == UIR_VITAL_LEVEL) {
        qsim_value_t v = qsim_bit_get(data, 0);
        qsim_bit_vector_t *r = qsim_bit_vector_alloc(1);
        qsim_bit_set(r, 0, (v.state == QSIM_1) ? QSIM_VAL_1 : QSIM_VAL_0);
        if (result_sig_idx >= 0)
            schedule_event(ctx, ctx->current_time, ctx->current_delta + 1,
                           (uint32_t)result_sig_idx, qsim_bit_vector_clone(r), 0, -1, -1);
        qsim_bit_vector_free(data);
        return r;
    }

    /* Boolean reduction across all data bits */
    qsim_logic_state_t result;
    switch (kind) {
    case UIR_VITAL_BUF:
    case UIR_VITAL_IDENT:
        result = qsim_bit_get(data, 0).state;
        break;
    case UIR_VITAL_INV: {
        qsim_logic_state_t s = qsim_bit_get(data, 0).state;
        result = (s == QSIM_1) ? QSIM_0 : (s == QSIM_0) ? QSIM_1 : QSIM_X;
        break;
    }
    case UIR_VITAL_AND:
    case UIR_VITAL_NAND:
        result = QSIM_1;
        for (uint32_t b = 0; b < total_bits; b++) {
            qsim_logic_state_t s = qsim_bit_get(data, b).state;
            if (s == QSIM_0) { result = QSIM_0; break; }
            if (s == QSIM_X || s == QSIM_Z) result = QSIM_X;
        }
        if (kind == UIR_VITAL_NAND) {
            if (result == QSIM_1) result = QSIM_0;
            else if (result == QSIM_0) result = QSIM_1;
            else result = QSIM_X;
        }
        break;
    case UIR_VITAL_OR:
    case UIR_VITAL_NOR:
        result = QSIM_0;
        for (uint32_t b = 0; b < total_bits; b++) {
            qsim_logic_state_t s = qsim_bit_get(data, b).state;
            if (s == QSIM_1) { result = QSIM_1; break; }
            if (s == QSIM_X || s == QSIM_Z) result = QSIM_X;
        }
        if (kind == UIR_VITAL_NOR) {
            if (result == QSIM_1) result = QSIM_0;
            else if (result == QSIM_0) result = QSIM_1;
            else result = QSIM_X;
        }
        break;
    case UIR_VITAL_XOR:
    case UIR_VITAL_XNOR: {
        int parity = 0, has_x = 0;
        for (uint32_t b = 0; b < total_bits; b++) {
            qsim_logic_state_t s = qsim_bit_get(data, b).state;
            if (s == QSIM_1) parity = !parity;
            else if (s == QSIM_X || s == QSIM_Z) has_x = 1;
        }
        result = has_x ? QSIM_X : (parity ? QSIM_1 : QSIM_0);
        if (kind == UIR_VITAL_XNOR && !has_x)
            result = parity ? QSIM_0 : QSIM_1;
        break;
    }
    default:
        result = QSIM_X;
        break;
    }

    qsim_bit_vector_t *r = qsim_bit_vector_alloc(1);
    qsim_bit_set(r, 0, (qsim_value_t){result, QSIM_STRENGTH_STRONG});
    qsim_bit_vector_free(data);

    if (result_sig_idx >= 0)
        schedule_event(ctx, ctx->current_time, ctx->current_delta + 1,
                       (uint32_t)result_sig_idx, qsim_bit_vector_clone(r), 0, -1, -1);

    return r;
}

/* Schedule a value on the last-arg signal of a VITAL timing-check procedure */
static void vital_write_result(uir_sim_context_t *ctx, uir_func_call_t *tc, qsim_bit_vector_t *result) {
    if (!result || tc->arg_count == 0) return;
    uir_node_t *last = tc->args[tc->arg_count - 1];
    if (last && last->kind == UIR_REF) {
        int sig_idx = find_signal_idx(ctx, ((uir_ref_t *)last)->name);
        if (sig_idx >= 0)
            schedule_event(ctx, ctx->current_time, ctx->current_delta + 1,
                           (uint32_t)sig_idx, result, 0, -1, -1);
        else
            qsim_bit_vector_free(result);
    } else {
        qsim_bit_vector_free(result);
    }
}

/* Evaluate a VITAL timing-check procedure (called from UIR_TASK_ENABLE dispatch).
   Currently all timing checks are stubs that write X to the violation output. */
static void vital_eval_task(uir_sim_context_t *ctx, int kind, uir_func_call_t *tc) {
    (void)kind;
    /* For now, write QSIM_X to the last argument (violation output signal) */
    qsim_bit_vector_t *x_val = qsim_bit_vector_from_state(1, QSIM_X);
    vital_write_result(ctx, tc, x_val);
}

/* ── UDP truth table evaluation ── */

/* Match a signal value against a UDP table character */
static int udp_char_match(qsim_value_t sig_val, char table_char) {
    switch (table_char) {
        case '0': return sig_val.state == QSIM_0;
        case '1': return sig_val.state == QSIM_1;
        case 'x': case 'X': return sig_val.state == QSIM_X;
        case '?': return (sig_val.state == QSIM_0 || sig_val.state == QSIM_1 || sig_val.state == QSIM_X);
        case 'b': case 'B': return (sig_val.state == QSIM_0 || sig_val.state == QSIM_1);
        case '-': return 1; /* no change (sequential only) */
        default:  return 0;
    }
}

/* Match an edge specification against old/new values.
 * Supports 4-char format "(ab)" and single-char shorthands r/f/p/n/*. */
static int udp_edge_match(const char *edge_chars, qsim_value_t old_val, qsim_value_t new_val) {
    if (!edge_chars || !edge_chars[0]) return 0;
    if (edge_chars[1] == '\0') {
        switch (edge_chars[0]) {
            case 'r': return (old_val.state != QSIM_1) && (new_val.state == QSIM_1);
            case 'f': return (old_val.state != QSIM_0) && (new_val.state == QSIM_0);
            case 'p': return (old_val.state == QSIM_0 && (new_val.state == QSIM_1 || new_val.state == QSIM_X))
                          || (old_val.state == QSIM_X && new_val.state == QSIM_1);
            case 'n': return (old_val.state == QSIM_1 && (new_val.state == QSIM_0 || new_val.state == QSIM_X))
                          || (old_val.state == QSIM_X && new_val.state == QSIM_0);
            case '*': return old_val.state != new_val.state;
        }
        return 0;
    }
    if (edge_chars[0] == '(' && edge_chars[3] == ')') {
        char old_c = edge_chars[1];
        char new_c = edge_chars[2];
        return udp_char_match(old_val, old_c) && udp_char_match(new_val, new_c);
    }
    return 0;
}

/* Evaluate a combinational UDP instance: read current input values, match
 * against the truth table, and update the output signal if a match is found. */
static void evaluate_combinational_udp(uir_sim_context_t *ctx, udp_instance_entry_t *entry) {
    uir_udp_t *udp = entry->udp;
    if (!udp || udp->entry_count == 0) return;

    size_t n_inputs = entry->input_count;
    char input_chars[64];
    if (n_inputs > 63) n_inputs = 63;

    for (size_t i = 0; i < n_inputs; i++) {
        int sig_idx = entry->input_sig_indices[i];
        if (sig_idx < 0) return;
        qsim_bit_vector_t *val = ctx->signals[sig_idx].value;
        if (!val || val->width == 0) return;
        qsim_value_t bit = qsim_bit_get(val, 0);
        if (bit.state == QSIM_1) input_chars[i] = '1';
        else if (bit.state == QSIM_0) input_chars[i] = '0';
        else input_chars[i] = 'x';
    }
    input_chars[n_inputs] = '\0';

    /* Scan truth table for level-sensitive match */
    size_t pattern_offset = 0; /* no edge prefix for combinational */
    for (size_t ei = 0; ei < udp->entry_count; ei++) {
        const char *pat = udp->entries[ei].input_pattern;
        if (!pat) continue;

        int match = 1;
        const char *p = pat + pattern_offset;
        for (size_t i = 0; i < n_inputs; i++) {
            if (!p[i]) { match = 0; break; }
            if (!udp_char_match(qsim_bit_get(ctx->signals[entry->input_sig_indices[i]].value, 0), p[i])) {
                match = 0; break;
            }
        }
        if (!match) continue;

        char output_char = udp->entries[ei].output;
        qsim_value_t new_val;
        new_val.state = (output_char == '1') ? QSIM_1 :
                        (output_char == '0') ? QSIM_0 : QSIM_X;
        new_val.strength = QSIM_STRENGTH_STRONG;

        if (entry->output_sig_idx >= 0) {
            qsim_bit_vector_t *bv = qsim_bit_vector_alloc(1);
            if (bv) {
                qsim_bit_set(bv, 0, new_val);
                qsim_bit_vector_t *old_out = NULL;
                signal_write_resolved(ctx, (uint32_t)entry->output_sig_idx, bv, &old_out, -1);
                if (old_out) {
                    check_and_trigger(ctx, (uint32_t)entry->output_sig_idx, old_out,
                                      ctx->signals[entry->output_sig_idx].value);
                    qsim_bit_vector_free(old_out);
                }
                qsim_bit_vector_free(bv);
            }
        }
        return;
    }
}

/* Evaluate a sequential UDP instance: check for edge match on first input
 * (clock), then match remaining input levels and current state against the
 * truth table, and update the output if a matching entry is found. */
static void evaluate_sequential_udp(uir_sim_context_t *ctx, udp_instance_entry_t *entry,
                                     const qsim_bit_vector_t *old_clock,
                                     const qsim_bit_vector_t *new_clock) {
    uir_udp_t *udp = entry->udp;
    if (!udp || udp->entry_count == 0) return;
    if (entry->input_count == 0) return;

    /* Get old/new value of first input (clock) for edge detection */
    qsim_value_t old_clk = { QSIM_0, QSIM_STRENGTH_STRONG };
    qsim_value_t new_clk = { QSIM_0, QSIM_STRENGTH_STRONG };
    if (old_clock && old_clock->width > 0) old_clk = qsim_bit_get(old_clock, 0);
    if (new_clock && new_clock->width > 0) new_clk = qsim_bit_get(new_clock, 0);

    /* Read current state from output signal */
    qsim_value_t cur_state = { QSIM_X, QSIM_STRENGTH_STRONG };
    int state_sig_idx = (entry->state_sig_idx >= 0) ? entry->state_sig_idx : entry->output_sig_idx;
    if (state_sig_idx >= 0 && ctx->signals[state_sig_idx].value &&
        ctx->signals[state_sig_idx].value->width > 0)
        cur_state = qsim_bit_get(ctx->signals[state_sig_idx].value, 0);

    size_t n_inputs = entry->input_count;
    size_t n_level_inputs = (n_inputs > 0) ? n_inputs - 1 : 0;

    /* Read current values of non-clock inputs (inputs 1..N-1) */
    char input_chars[64];
    for (size_t i = 0; i < n_level_inputs && i < 63; i++) {
        int sig_idx = entry->input_sig_indices[i + 1];
        if (sig_idx < 0) return;
        qsim_bit_vector_t *val = ctx->signals[sig_idx].value;
        if (!val || val->width == 0) return;
        qsim_value_t bit = qsim_bit_get(val, 0);
        if (bit.state == QSIM_1) input_chars[i] = '1';
        else if (bit.state == QSIM_0) input_chars[i] = '0';
        else input_chars[i] = 'x';
    }
    input_chars[n_level_inputs] = '\0';

    /* Scan truth table */
    for (size_t ei = 0; ei < udp->entry_count; ei++) {
        const char *pat = udp->entries[ei].input_pattern;
        if (!pat || !pat[0]) continue;

        /* Parse edge spec from pattern beginning */
        const char *p = pat;
        int edge_matched = 0;
        const char *level_start = pat;

        if (p[0] == '(' && p[3] == ')') {
            /* 4-char format: "(ab)" */
            edge_matched = udp_edge_match(p, old_clk, new_clk);
            level_start = p + 4;
        } else if (strchr("rRfFpPnN*", p[0])) {
            /* Single-char shorthand */
            char edge_buf[2] = { p[0], '\0' };
            edge_matched = udp_edge_match(edge_buf, old_clk, new_clk);
            level_start = p + 1;
        } else {
            /* No edge spec — should not happen in sequential UDP but handle */
            edge_matched = 1; /* treat as level-sensitive */
        }

        if (!edge_matched) continue;

        /* Match remaining input levels (inputs 1..N-1) against pattern chars */
        int level_match = 1;
        for (size_t i = 0; i < n_level_inputs; i++) {
            char pat_char = level_start[i];
            if (!pat_char) { level_match = 0; break; }
            if (!udp_char_match(qsim_bit_get(ctx->signals[entry->input_sig_indices[i + 1]].value, 0), pat_char)) {
                level_match = 0; break;
            }
        }
        if (!level_match) continue;

        /* Match current state */
        char state_char = udp->entries[ei].current_state;
        if (state_char && state_char != '?') {
            if (!udp_char_match(cur_state, state_char))
                continue;
        }

        /* All conditions met — update output */
        char output_char = udp->entries[ei].output;
        if (output_char == '-') return; /* no change */

        qsim_value_t new_val;
        new_val.state = (output_char == '1') ? QSIM_1 :
                        (output_char == '0') ? QSIM_0 : QSIM_X;
        new_val.strength = QSIM_STRENGTH_STRONG;

        if (entry->output_sig_idx >= 0) {
            qsim_bit_vector_t *bv = qsim_bit_vector_alloc(1);
            if (bv) {
                qsim_bit_set(bv, 0, new_val);
                qsim_bit_vector_t *old_out = NULL;
                signal_write_resolved(ctx, (uint32_t)entry->output_sig_idx, bv, &old_out, -1);
                if (old_out) {
                    check_and_trigger(ctx, (uint32_t)entry->output_sig_idx, old_out,
                                      ctx->signals[entry->output_sig_idx].value);
                    qsim_bit_vector_free(old_out);
                }
                qsim_bit_vector_free(bv);
            }
        }
        return; /* first match wins (standard UDP behavior) */
    }
}

/* ── UDP instance setup ── */

static void add_udp_instances(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                               const char *prefix) {
    char saved_prefix[520] = "";
    if (prefix[0]) {
        strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);
        saved_prefix[sizeof(saved_prefix) - 1] = '\0';
        strncpy(ctx->current_prefix, prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
    }

    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;
        uir_design_unit_t *udp_unit = inst->bound_to;
        if (!udp_unit->udp) continue;

        uir_udp_t *udp = udp_unit->udp;

        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);

        /* Count inputs (ports that are not output/inout) */
        size_t input_count = 0;
        int output_idx = -1;
        for (size_t p = 0; p < udp_unit->port_count; p++) {
            if (udp_unit->ports[p]->direction == UIR_PORT_IN)
                input_count++;
            else if (udp_unit->ports[p]->direction == UIR_PORT_OUT)
                ; /* will find below */
        }

        int *input_indices = calloc(input_count, sizeof(int));
        if (!input_indices) continue;

        size_t in_idx = 0;
        for (size_t p = 0; p < udp_unit->port_count; p++) {
            uir_port_t *port = udp_unit->ports[p];
            char child_path[512];
            snprintf(child_path, sizeof(child_path), "%s.%s", child_prefix, port->name);

            if (port->direction == UIR_PORT_IN) {
                input_indices[in_idx++] = find_signal_idx(ctx, child_path);
            } else if (port->direction == UIR_PORT_OUT) {
                output_idx = find_signal_idx(ctx, child_path);
            }
        }

        size_t n = ctx->udp_instance_count;
        udp_instance_entry_t *ne = realloc(ctx->udp_instances,
            (n + 1) * sizeof(udp_instance_entry_t));
        if (!ne) { free(input_indices); continue; }
        ctx->udp_instances = ne;
        ctx->udp_instances[n].udp = udp;
        ctx->udp_instances[n].output_sig_idx = output_idx;
        ctx->udp_instances[n].input_sig_indices = input_indices;
        ctx->udp_instances[n].input_count = input_count;
        ctx->udp_instances[n].state_sig_idx = udp->is_sequential ? output_idx : -1;
        snprintf(ctx->udp_instances[n].prefix, sizeof(ctx->udp_instances[n].prefix), "%s", child_prefix);
        ctx->udp_instance_count = n + 1;
    }

    /* Recurse into regular sub-modules (skip UDP instances) */
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;
        if (inst->bound_to->udp) continue;
        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);
        add_udp_instances(ctx, inst->bound_to, child_prefix);
    }

    if (prefix[0]) {
        strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
    }
}

static void add_cont_assigns(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                              const char *prefix) {
    char saved_prefix[520] = "";
    if (prefix[0]) {
        strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);
        saved_prefix[sizeof(saved_prefix) - 1] = '\0';
        strncpy(ctx->current_prefix, prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
    }
    for (size_t i = 0; i < unit->assign_count; i++) {
        size_t n = ctx->cont_assign_count;
        cont_assign_entry_t *na = realloc(ctx->cont_assigns,
            (n + 1) * sizeof(cont_assign_entry_t));
        if (!na) return;
        ctx->cont_assigns = na;
        cont_assign_entry_t *entry = &ctx->cont_assigns[n];
        entry->assign = unit->assigns[i];
        entry->dep_sigs = NULL;
        entry->dep_count = 0;
        snprintf(entry->prefix, sizeof(entry->prefix), "%s", prefix);
        size_t cap = 0;
        find_refs_in_node((uir_node_t *)entry->assign->rhs, ctx,
                          &entry->dep_sigs, &entry->dep_count, &cap);
        /* Also scan LHS index/part-select for dependencies (e.g., mem[addr] depends on addr) */
        uir_node_t *lhs = (uir_node_t *)entry->assign->lhs;
        if (lhs && lhs->kind == UIR_REF) {
            uir_ref_t *lhs_ref = (uir_ref_t *)lhs;
            if (lhs_ref->index)
                find_refs_in_node(lhs_ref->index, ctx,
                                  &entry->dep_sigs, &entry->dep_count, &cap);
            if (lhs_ref->part_hi)
                find_refs_in_node(lhs_ref->part_hi, ctx,
                                  &entry->dep_sigs, &entry->dep_count, &cap);
            if (lhs_ref->part_lo)
                find_refs_in_node(lhs_ref->part_lo, ctx,
                                  &entry->dep_sigs, &entry->dep_count, &cap);
            for (size_t _mi = 0; _mi < lhs_ref->multi_idx_count; _mi++)
                find_refs_in_node(lhs_ref->multi_index[_mi], ctx,
                                  &entry->dep_sigs, &entry->dep_count, &cap);
        }
        ctx->cont_assign_count = n + 1;
    }
    if (prefix[0]) {
        strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
    }
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;
        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);
        add_cont_assigns(ctx, inst->bound_to, child_prefix);
    }
}

/* ── Path delay elaboration from specify blocks ── */

/* Evaluate a delay expression to a uint64 constant.
 * Handles literals and specparam references (recursively). */
static uint64_t eval_path_delay(uir_specify_t *spec, uir_node_t *expr) {
    if (!expr) return 0;
    if (expr->kind == UIR_LITERAL) {
        uir_literal_t *lit = (uir_literal_t *)expr;
        uint64_t val = 0;
        for (uint32_t i = 0; i < lit->value->width && i < 64; i++) {
            if (qsim_bit_get(lit->value, i).state == QSIM_1)
                val |= (1ULL << i);
        }
        return val;
    }
    if (expr->kind == UIR_REF) {
        uir_ref_t *ref = (uir_ref_t *)expr;
        for (size_t i = 0; i < spec->specparam_count; i++) {
            if (strcmp(spec->specparams[i].hier_path, ref->name) == 0)
                return eval_path_delay(spec, spec->specparams[i].value);
        }
        return 0;
    }
    if (expr->kind == UIR_EXPR_BINARY) {
        uir_expr_t *e = (uir_expr_t *)expr;
        uint64_t a = eval_path_delay(spec, e->operand_a);
        uint64_t b = eval_path_delay(spec, e->operand_b);
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
    if (expr->kind == UIR_EXPR_UNARY) {
        uir_expr_t *e = (uir_expr_t *)expr;
        uint64_t a = eval_path_delay(spec, e->operand_a);
        switch (e->op.un_op) {
            case UIR_OP_NEG: return (uint64_t)(-(int64_t)a);
            case UIR_OP_NOT: return ~a;
            default: return 0;
        }
    }
    return 0;
}

static void add_path_delays(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                             const char *prefix) {
    char saved_prefix[520] = "";
    if (prefix[0]) {
        strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);
        saved_prefix[sizeof(saved_prefix) - 1] = '\0';
        strncpy(ctx->current_prefix, prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
    }
    for (size_t i = 0; i < unit->specify_count; i++) {
        uir_specify_t *spec = unit->specifies[i];
        if (!spec) continue;
        for (size_t j = 0; j < spec->path_count; j++) {
            uir_path_delay_t *pd = &spec->paths[j];
            /* Resolve src signal index */
            char src_name[520];
            if (prefix[0])
                snprintf(src_name, sizeof(src_name), "%s.%s", prefix, pd->src);
            else
                snprintf(src_name, sizeof(src_name), "%s", pd->src);
            int src_idx = find_signal_idx(ctx, src_name);
            if (src_idx < 0) continue;

            /* Resolve dst signal index */
            char dst_name[520];
            if (prefix[0])
                snprintf(dst_name, sizeof(dst_name), "%s.%s", prefix, pd->dst);
            else
                snprintf(dst_name, sizeof(dst_name), "%s", pd->dst);
            int dst_idx = find_signal_idx(ctx, dst_name);
            if (dst_idx < 0) continue;

            size_t n = ctx->path_delay_count;
            sim_path_delay_t *np = realloc(ctx->path_delays,
                (n + 1) * sizeof(sim_path_delay_t));
            if (!np) continue;
            ctx->path_delays = np;
            ctx->path_delays[n].src_sig_idx = src_idx;
            ctx->path_delays[n].dst_sig_idx = dst_idx;
            ctx->path_delays[n].src_edge = pd->src_edge;
            ctx->path_delays[n].rise_delay = eval_path_delay(spec, pd->rise_delay);
            ctx->path_delays[n].fall_delay = eval_path_delay(spec, pd->fall_delay);
            ctx->path_delays[n].z_delay = eval_path_delay(spec, pd->z_delay);
            ctx->path_delays[n].x_delay = eval_path_delay(spec, pd->x_delay);
            ctx->path_delays[n].condition = pd->condition;
            ctx->path_delay_count = n + 1;
        }
    }
    if (prefix[0]) {
        strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);
        ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
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
        add_path_delays(ctx, inst->bound_to, child_prefix);
    }
}

/* Register timing checks from specify blocks into the simulation context.
 * For Phase 4c, timing checks produce a stub warning via display_cb
 * when the reference signal changes. */
static void add_timing_checks(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                               const char *prefix) {
    for (size_t i = 0; i < unit->specify_count; i++) {
        uir_specify_t *spec = unit->specifies[i];
        if (!spec) continue;
        for (size_t j = 0; j < spec->timing_check_count; j++) {
            uir_timing_check_t *tc = &spec->timing_checks[j];

            /* Resolve data pin to signal index */
            int data_idx = -1;
            if (tc->data_pin) {
                char name[520];
                if (prefix[0])
                    snprintf(name, sizeof(name), "%s.%s", prefix, tc->data_pin);
                else
                    snprintf(name, sizeof(name), "%s", tc->data_pin);
                data_idx = find_signal_idx(ctx, name);
            }

            /* Resolve ref pin to signal index */
            int ref_idx = -1;
            if (tc->ref_pin) {
                char name[520];
                if (prefix[0])
                    snprintf(name, sizeof(name), "%s.%s", prefix, tc->ref_pin);
                else
                    snprintf(name, sizeof(name), "%s", tc->ref_pin);
                ref_idx = find_signal_idx(ctx, name);
            }
            if (ref_idx < 0) continue;

            size_t n = ctx->timing_check_count;
            sim_timing_check_t *nt = realloc(ctx->timing_checks,
                (n + 1) * sizeof(sim_timing_check_t));
            if (!nt) continue;
            ctx->timing_checks = nt;
            ctx->timing_checks[n].kind = tc->kind;
            ctx->timing_checks[n].data_sig_idx = data_idx;
            ctx->timing_checks[n].ref_sig_idx = ref_idx;
            ctx->timing_checks[n].limit = eval_path_delay(spec, tc->limit);
            ctx->timing_checks[n].last_ref_time = 0;
            ctx->timing_checks[n].last_ref_bit = -1;
            ctx->timing_check_count = n + 1;
        }
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
        add_timing_checks(ctx, inst->bound_to, child_prefix);
    }
}

/* ── SDF annotation support ── */

/* Recursively walk a statement tree looking for $sdf_annotate system tasks
 * and collect their filenames. */
static void find_sdf_annotate_tasks(uir_node_t *node, const char ***filenames,
                                     size_t *count, size_t *cap) {
    if (!node) return;
    switch (node->kind) {
        case UIR_SYS_TASK: {
            uir_sys_task_t *t = (uir_sys_task_t *)node;
            if (t->task_kind == UIR_SDF_ANNOTATE && t->filename) {
                if (*count >= *cap) {
                    *cap = *cap ? *cap * 2 : 4;
                    const char **nf = realloc((void *)*filenames, *cap * sizeof(char *));
                    if (!nf) return;
                    *filenames = nf;
                }
                (*filenames)[(*count)++] = t->filename;
            }
            break;
        }
        case UIR_BLOCK: {
            uir_block_t *b = (uir_block_t *)node;
            for (size_t i = 0; i < b->stmt_count; i++)
                find_sdf_annotate_tasks(b->stmts[i], filenames, count, cap);
            break;
        }
        case UIR_IF: {
            uir_if_t *i = (uir_if_t *)node;
            find_sdf_annotate_tasks(i->then_branch, filenames, count, cap);
            find_sdf_annotate_tasks(i->else_branch, filenames, count, cap);
            break;
        }
        case UIR_DELAY: {
            uir_delay_t *d = (uir_delay_t *)node;
            find_sdf_annotate_tasks(d->body, filenames, count, cap);
            break;
        }
        case UIR_LOOP_BACK: {
            /* UIR_LOOP_BACK.body points back to the loop's augmented block,
             * forming a cycle. The body is already visited via UIR_LOOP's body,
             * so skip it here to avoid infinite recursion. */
            break;
        }
        case UIR_EVENT_CTRL: {
            uir_event_ctrl_t *ec = (uir_event_ctrl_t *)node;
            find_sdf_annotate_tasks(ec->body, filenames, count, cap);
            break;
        }
        case UIR_WAIT: {
            uir_wait_t *w = (uir_wait_t *)node;
            find_sdf_annotate_tasks(w->body, filenames, count, cap);
            break;
        }
        case UIR_CASE: {
            uir_case_t *c = (uir_case_t *)node;
            for (size_t i = 0; i < c->item_count; i++)
                find_sdf_annotate_tasks((uir_node_t *)c->items[i], filenames, count, cap);
            if (c->default_item)
                find_sdf_annotate_tasks(c->default_item, filenames, count, cap);
            break;
        }
        case UIR_CASE_ITEM: {
            uir_case_item_t *ci = (uir_case_item_t *)node;
            find_sdf_annotate_tasks(ci->body, filenames, count, cap);
            break;
        }
        case UIR_LOOP: {
            uir_loop_t *l = (uir_loop_t *)node;
            find_sdf_annotate_tasks(l->init_stmt, filenames, count, cap);
            find_sdf_annotate_tasks(l->body, filenames, count, cap);
            break;
        }
        default:
            break;
    }
}

/* Apply SDF annotations from $sdf_annotate tasks found in all processes.
 * Parses SDF files and overrides matching path delays in ctx. */
static void apply_sdf_annotations(uir_sim_context_t *ctx) {
    const char **filenames = NULL;
    size_t filename_count = 0, filename_cap = 0;

    /* Collect filenames from all $sdf_annotate tasks */
    for (size_t u = 0; u < ctx->unit_count; u++) {
        uir_design_unit_t *unit = ctx->units[u];
        if (!unit) continue;
        for (size_t p = 0; p < unit->process_count; p++) {
            if (unit->processes[p] && unit->processes[p]->body)
                find_sdf_annotate_tasks(unit->processes[p]->body,
                                        &filenames, &filename_count, &filename_cap);
        }
    }

    for (size_t i = 0; i < filename_count; i++) {
        sdf_file_t *sdf = sdf_parse_file(filenames[i]);
        if (!sdf) {
            if (ctx->display_cb) {
                char warn[1024];
                snprintf(warn, sizeof(warn),
                         "Warning: could not open or parse SDF file '%s'\n", filenames[i]);
                ctx->display_cb(ctx, warn, ctx->display_cb_user_data);
            }
            continue;
        }

        for (size_t c = 0; c < sdf->cell_count; c++) {
            sdf_cell_t *cell = &sdf->cells[c];
            if (!cell->instance) continue;

            /* Normalize instance path: replace / with . */
            char norm[1024];
            size_t nlen = 0;
            for (char *sp = cell->instance; *sp && nlen < sizeof(norm) - 1; sp++)
                norm[nlen++] = (*sp == '/') ? '.' : *sp;
            norm[nlen] = '\0';
            if (nlen == 0) continue;

            for (size_t j = 0; j < cell->iopath_count; j++) {
                sdf_iopath_t *io = &cell->iopaths[j];
                int found = 0;

                /* Try full path first (e.g. "top.u1.a"), then strip leading
                 * top module name (e.g. "u1.a"), then bare pin name (e.g. "a"). */
                for (int try_prefix = 0; try_prefix < 3 && !found; try_prefix++) {
                    const char *prefix = norm;
                    char stripped[1024];

                    if (try_prefix == 1) {
                        const char *dot = strchr(norm, '.');
                        if (!dot) continue;
                        strncpy(stripped, dot + 1, sizeof(stripped) - 1);
                        stripped[sizeof(stripped) - 1] = '\0';
                        prefix = stripped;
                    } else if (try_prefix == 2) {
                        /* Bare pin name — no instance prefix */
                        int src_idx = find_signal_idx(ctx, io->src_pin);
                        int dst_idx = find_signal_idx(ctx, io->dst_pin);
                        if (src_idx < 0 || dst_idx < 0) continue;
                        for (size_t p = 0; p < ctx->path_delay_count; p++) {
                            if (ctx->path_delays[p].src_sig_idx == src_idx &&
                                ctx->path_delays[p].dst_sig_idx == dst_idx) {
                                ctx->path_delays[p].rise_delay = io->rise_delay;
                                ctx->path_delays[p].fall_delay = io->fall_delay;
                                found = 1;
                            }
                        }
                        break;
                    }

                    char src_name[1152], dst_name[1152];
                    snprintf(src_name, sizeof(src_name), "%s.%s", prefix, io->src_pin);
                    snprintf(dst_name, sizeof(dst_name), "%s.%s", prefix, io->dst_pin);

                    int src_idx = find_signal_idx(ctx, src_name);
                    int dst_idx = find_signal_idx(ctx, dst_name);
                    if (src_idx < 0 || dst_idx < 0) continue;

                    for (size_t p = 0; p < ctx->path_delay_count; p++) {
                        if (ctx->path_delays[p].src_sig_idx == src_idx &&
                            ctx->path_delays[p].dst_sig_idx == dst_idx) {
                            ctx->path_delays[p].rise_delay = io->rise_delay;
                            ctx->path_delays[p].fall_delay = io->fall_delay;
                            found = 1;
                        }
                    }
                }
            }
        }

        sdf_file_free(sdf);
    }

    free((void *)filenames);
}

/* ── Port connection wiring ── */

static void add_port_wires(uir_sim_context_t *ctx, uir_design_unit_t *unit,
                           const char *prefix) {
    for (size_t i = 0; i < unit->instance_count; i++) {
        uir_instance_t *inst = unit->instances[i];
        if (!inst->bound_to) continue;

        /* Build child prefix (same scheme as collect_signals) */
        char child_prefix[512];
        if (prefix[0])
            snprintf(child_prefix, sizeof(child_prefix), "%s.%s", prefix, inst->instance_name);
        else
            snprintf(child_prefix, sizeof(child_prefix), "%s", inst->instance_name);

        for (size_t j = 0; j < inst->connection_count; j++) {
            uir_port_connection_t *conn = &inst->connections[j];
            if (!conn->actual) continue;

            /* Handle constant literal port connections (e.g. .acc_0(32'b0)) */
            if (conn->actual->kind != UIR_REF) {
                if (conn->actual->kind == UIR_LITERAL) {
                    uir_literal_t *lit = (uir_literal_t *)conn->actual;
                    uir_port_dir_t dir = UIR_PORT_IN;
                    for (size_t k = 0; k < inst->bound_to->port_count; k++) {
                        if (strcmp(inst->bound_to->ports[k]->name, conn->formal_name) == 0) {
                            dir = inst->bound_to->ports[k]->direction;
                            break;
                        }
                    }
                    if (dir == UIR_PORT_IN) {
                        char child_path[512];
                        snprintf(child_path, sizeof(child_path), "%s.%s", child_prefix, conn->formal_name);
                        int child_idx = find_signal_idx(ctx, child_path);
                        if (child_idx >= 0) {
                            /* Save for deferred init (after all port wires exist) */
                            if (ctx->pending_lit_count >= ctx->pending_lit_cap) {
                                size_t nc = ctx->pending_lit_cap ? ctx->pending_lit_cap * 2 : 16;
                                int *ns = realloc(ctx->pending_lit_sigs, nc * sizeof(int));
                                qsim_bit_vector_t **nv = realloc(ctx->pending_lit_vals,
                                    nc * sizeof(qsim_bit_vector_t *));
                                if (!ns || !nv) break;
                                ctx->pending_lit_sigs = ns;
                                ctx->pending_lit_vals = nv;
                                ctx->pending_lit_cap = nc;
                            }
                            ctx->pending_lit_sigs[ctx->pending_lit_count] = child_idx;
                            ctx->pending_lit_vals[ctx->pending_lit_count] = qsim_bit_vector_clone(lit->value);
                            ctx->pending_lit_count++;
                        }
                    }
                }
                continue;
            }
            uir_ref_t *ref = (uir_ref_t *)conn->actual;

            /* Find parent signal index (prepend prefix for hierarchical lookup) */
            char parent_path[520];
            const char *parent_name;
            if (prefix[0]) {
                snprintf(parent_path, sizeof(parent_path), "%s.%s", prefix, ref->name);
                parent_name = parent_path;
            } else {
                parent_name = ref->name;
            }
            int parent_idx = find_signal_idx(ctx, parent_name);
            if (parent_idx < 0) continue;

            /* Find child port signal index */
            char child_path[512];
            snprintf(child_path, sizeof(child_path), "%s.%s", child_prefix, conn->formal_name);
            int child_idx = find_signal_idx(ctx, child_path);
            if (child_idx < 0) continue;

            /* Determine direction from child's port */
            uir_port_dir_t dir = UIR_PORT_IN;
            for (size_t k = 0; k < inst->bound_to->port_count; k++) {
                if (strcmp(inst->bound_to->ports[k]->name, conn->formal_name) == 0) {
                    dir = inst->bound_to->ports[k]->direction;
                    break;
                }
            }

            /* Evaluate part-select bounds, if any */
            int part_lo = -1, part_width = 0;
            if (ref->part_hi && ref->part_lo &&
                ref->part_hi->kind == UIR_LITERAL && ref->part_lo->kind == UIR_LITERAL) {
                uir_literal_t *hi_lit = (uir_literal_t *)ref->part_hi;
                uir_literal_t *lo_lit = (uir_literal_t *)ref->part_lo;
                uint32_t hi = 0, lo = 0;
                for (uint32_t bi = 0; bi < hi_lit->value->width && bi < 32; bi++)
                    if (hi_lit->value->bits[bi].state == QSIM_1) hi |= (1u << bi);
                for (uint32_t bi = 0; bi < lo_lit->value->width && bi < 32; bi++)
                    if (lo_lit->value->bits[bi].state == QSIM_1) lo |= (1u << bi);
                part_lo = (int)((hi >= lo) ? lo : hi);
                part_width = (int)((hi >= lo) ? (hi - lo + 1) : (lo - hi + 1));
            }

            /* Grow port_wires array */
            if (ctx->port_wire_count >= ctx->port_wire_cap) {
                size_t nc = ctx->port_wire_cap ? ctx->port_wire_cap * 2 : 16;
                port_wire_t *nw = realloc(ctx->port_wires, nc * sizeof(port_wire_t));
                if (!nw) break;
                ctx->port_wires = nw;
                ctx->port_wire_cap = nc;
            }

            if (dir == UIR_PORT_IN) {
                /* Input: parent drives child */
                ctx->port_wires[ctx->port_wire_count].src_sig_idx = parent_idx;
                ctx->port_wires[ctx->port_wire_count].dst_sig_idx = child_idx;
                ctx->port_wires[ctx->port_wire_count].part_lo = part_lo;
                ctx->port_wires[ctx->port_wire_count].part_width = part_width;
                ctx->port_wires[ctx->port_wire_count].dir = dir;
                ctx->port_wire_count++;
            } else if (dir == UIR_PORT_OUT) {
                /* Output: child drives parent */
                ctx->port_wires[ctx->port_wire_count].src_sig_idx = child_idx;
                ctx->port_wires[ctx->port_wire_count].dst_sig_idx = parent_idx;
                ctx->port_wires[ctx->port_wire_count].part_lo = part_lo;
                ctx->port_wires[ctx->port_wire_count].part_width = part_width;
                ctx->port_wires[ctx->port_wire_count].dir = dir;
                ctx->port_wire_count++;
            }
            /* inout: add both directions (omitted for now) */
        }

        /* Recurse into nested instances */
        add_port_wires(ctx, inst->bound_to, child_prefix);
    }
}

/* ── Array-indexed write helper ──
 * For array-indexed LHS (ref->index != NULL), clone the full array value
 * and write only the targeted element. Returns a new bit vector that the
 * caller owns (must free), or NULL if not array-indexed (caller should
 * write the value directly).
 */
static qsim_bit_vector_t *apply_part_select_write(uir_sim_context_t *ctx,
                                                    uir_ref_t *ref, int lhs_idx,
                                                    qsim_bit_vector_t *src_val) {
    if (!ref->part_hi || !ref->part_lo) return NULL;
    qsim_bit_vector_t *hi_val = eval_expr(ctx, ref->part_hi);
    qsim_bit_vector_t *lo_val = eval_expr(ctx, ref->part_lo);
    if (!hi_val || !lo_val) {
        if (hi_val) qsim_bit_vector_free(hi_val);
        if (lo_val) qsim_bit_vector_free(lo_val);
        return NULL;
    }
    uint32_t hi = 0, lo = 0;
    for (uint32_t i = 0; i < hi_val->width && i < 32; i++) {
        if (qsim_bit_get(hi_val, i).state == QSIM_1) hi |= (1u << i);
    }
    for (uint32_t i = 0; i < lo_val->width && i < 32; i++) {
        if (qsim_bit_get(lo_val, i).state == QSIM_1) lo |= (1u << i);
    }
    qsim_bit_vector_free(hi_val);
    qsim_bit_vector_free(lo_val);

    uint32_t part_w = (hi >= lo) ? (hi - lo + 1) : (lo - hi + 1);
    uint32_t start_bit = (hi >= lo) ? lo : hi;

    qsim_bit_vector_t *modified = qsim_bit_vector_clone(ctx->signals[lhs_idx].value);
    if (!modified) return NULL;

    uint32_t write_w = src_val->width < part_w ? src_val->width : part_w;
    for (uint32_t i = 0; i < write_w; i++) {
        if (start_bit + i < modified->width)
            qsim_bit_set(modified, start_bit + i, qsim_bit_get(src_val, i));
    }
    return modified;
}

static qsim_bit_vector_t *apply_array_write(uir_sim_context_t *ctx,
                                              uir_ref_t *ref, int lhs_idx,
                                              qsim_bit_vector_t *elem_val) {
    if (!ref->index) return NULL;
    uir_signal_t *sig = (uir_signal_t *)ctx->signals[lhs_idx].node;
    if (sig->base.kind != UIR_SIGNAL || sig->array_size == 0) return NULL;
    uint32_t elem_w = sig->width;

    qsim_bit_vector_t *index_val = eval_expr(ctx, ref->index);
    if (!index_val) return NULL;
    uint32_t elem_idx = 0;
    int index_known = 1;
    for (uint32_t i = 0; i < index_val->width && i < 32; i++) {
        qsim_value_t b = qsim_bit_get(index_val, i);
        if (b.state == QSIM_1) elem_idx |= (1u << i);
        else if (b.state != QSIM_0) { index_known = 0; break; }
    }
    qsim_bit_vector_free(index_val);

    qsim_bit_vector_t *modified = qsim_bit_vector_clone(ctx->signals[lhs_idx].value);
    if (!modified) return NULL;

    /* Resize element value to element width */
    uint32_t write_w = elem_val->width < elem_w ? elem_val->width : elem_w;

    if (!index_known) {
        /* X/Z index: write X to whole element */
        for (uint32_t i = 0; i < elem_w && i < modified->width; i++)
            qsim_bit_set(modified, i, QSIM_VAL_X);
    } else {
        uint32_t bit_off = elem_idx * elem_w;
        for (uint32_t i = 0; i < write_w; i++) {
            if (bit_off + i < modified->width)
                qsim_bit_set(modified, bit_off + i, qsim_bit_get(elem_val, i));
        }
        /* Zero remaining bits in element if elem_val is narrower */
        for (uint32_t i = write_w; i < elem_w; i++) {
            if (bit_off + i < modified->width)
                qsim_bit_set(modified, bit_off + i, QSIM_VAL_0);
        }
    }
    return modified;
}

/* ── Event arena pool (avoids per-event malloc/free) ── */

static sim_event_t *pool_alloc_event(uir_sim_context_t *ctx) {
    if (ctx->ev_free_list) {
        sim_event_t *ev = ctx->ev_free_list;
        ctx->ev_free_list = ev->next;
        memset(ev, 0, sizeof(sim_event_t));
        return ev;
    }
    /* Allocate a new block and distribute all events onto the free list */
    event_block_t *block = calloc(1, sizeof(event_block_t));
    if (!block) return NULL;
    block->next = ctx->ev_pool_blocks;
    ctx->ev_pool_blocks = block;
    for (size_t i = 0; i < EVENT_POOL_BLOCK_SIZE; i++) {
        block->events[i].next = ctx->ev_free_list;
        ctx->ev_free_list = &block->events[i];
    }
    /* Pop the first one */
    sim_event_t *ev = ctx->ev_free_list;
    ctx->ev_free_list = ev->next;
    memset(ev, 0, sizeof(sim_event_t));
    return ev;
}

static void pool_free_event(uir_sim_context_t *ctx, sim_event_t *ev) {
    qsim_bit_vector_free(ev->value);
    memset(ev, 0, sizeof(sim_event_t));
    ev->next = ctx->ev_free_list;
    ctx->ev_free_list = ev;
}

static void pool_destroy(uir_sim_context_t *ctx) {
    event_block_t *b = ctx->ev_pool_blocks;
    while (b) {
        event_block_t *next = b->next;
        free(b);
        b = next;
    }
    ctx->ev_pool_blocks = NULL;
    ctx->ev_free_list = NULL;
}

/* ── Signal change tracking for two-phase delta cycle ── */

typedef struct signal_change_s {
    int sig_idx;
    qsim_bit_vector_t *old_val;
} signal_change_t;

/* ── Per-thread state for parallel evaluation ── */

/* Thread-local current thread state for parallel Phase 2 event scheduling.
 * Set before exec_stmt in process_partition_triggers; checked by schedule_event
 * to route events to the per-thread pending list instead of the global queue. */
#ifdef _MSC_VER
static __declspec(thread) struct sim_thread_state *tls_ts = NULL;
#else
static __thread struct sim_thread_state *tls_ts = NULL;
#endif

typedef struct sim_thread_state {
    int id;                     /* 0 = leader (main thread), 1..N-1 = workers */
    uir_sim_context_t *ctx;
    int partition_first;        /* first partition index owned by this thread */
    int partition_count;        /* number of partitions owned by this thread */

    /* Per-thread event free list (avoids pool_mutex contention) */
    struct sim_event *local_free_list;

    /* Pending events from parallel phases (thread-local, no locking) */
    struct sim_event *pending_head;
    struct sim_event *pending_tail;
    size_t pending_count;

    /* Work batch for Phase 1 (pre-allocated across delta cycles) */
    struct sim_event **work_batch;
    size_t work_count;
    size_t work_cap;

    /* Local changes discovered */
    signal_change_t *changes;
    size_t change_count;
    size_t change_cap;

    /* Process indices owned by this thread (subset of ctx->processes[]) */
    int *proc_indices;
    size_t proc_count;

    /* CA indices owned by this thread (subset of ctx->cont_assigns[]) */
    int *ca_indices;
    size_t ca_count;
} sim_thread_state_t;

/* Free event to per-thread local free list instead of global pool.
 * Used during parallel Phase 1 cleanup to recycle events back to
 * the thread that will next allocate during Phase 2b. */
static void pool_free_event_thread(sim_thread_state_t *ts, sim_event_t *ev) {
    qsim_bit_vector_free(ev->value);
    memset(ev, 0, sizeof(sim_event_t));
    ev->next = ts->local_free_list;
    ts->local_free_list = ev;
}

/* ── WCC graph construction helpers ── */

/* Path-compressed find for union-find */
static int uf_find(int *parent, int x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];  /* path halving */
        x = parent[x];
    }
    return x;
}

/* Union by rank */
static void uf_union(int *parent, int *rank, int a, int b) {
    int ra = uf_find(parent, a);
    int rb = uf_find(parent, b);
    if (ra == rb) return;
    if (rank[ra] < rank[rb])       parent[ra] = rb;
    else if (rank[ra] > rank[rb])  parent[rb] = ra;
    else { parent[rb] = ra; rank[ra]++; }
}

/* Walk a UIR node tree and collect signal indices of ASSIGN LHS references
 * (signals written to via blocking or non-blocking assignment). Similar to
 * find_refs_in_node but targets write destinations only. */
static void find_writes_in_node(uir_node_t *node, uir_sim_context_t *ctx,
                                 int **sigs, size_t *count, size_t *cap) {
    if (!node) return;
    switch (node->kind) {
        case UIR_ASSIGN: {
            uir_assign_t *a = (uir_assign_t *)node;
            if (a->lhs && a->lhs->kind == UIR_REF) {
                uir_ref_t *ref = (uir_ref_t *)a->lhs;
                int idx = -1;
                if (ctx->current_prefix[0]) {
                    char prefixed[520];
                    snprintf(prefixed, sizeof(prefixed), "%s.%s",
                             ctx->current_prefix, ref->name);
                    idx = find_signal_idx(ctx, prefixed);
                }
                if (idx < 0) idx = find_signal_idx(ctx, ref->name);
                if (idx >= 0) {
                    if (*count >= *cap) {
                        *cap = *cap ? *cap * 2 : 16;
                        *sigs = realloc(*sigs, *cap * sizeof(int));
                    }
                    if (*sigs) (*sigs)[(*count)++] = idx;
                }
            }
            return;  /* don't recurse into RHS */
        }
        case UIR_FORCE: {
            uir_force_t *f = (uir_force_t *)node;
            if (f->lhs && f->lhs->kind == UIR_REF) {
                uir_ref_t *ref = (uir_ref_t *)f->lhs;
                int idx = -1;
                if (ctx->current_prefix[0]) {
                    char prefixed[520];
                    snprintf(prefixed, sizeof(prefixed), "%s.%s",
                             ctx->current_prefix, ref->name);
                    idx = find_signal_idx(ctx, prefixed);
                }
                if (idx < 0) idx = find_signal_idx(ctx, ref->name);
                if (idx >= 0) {
                    if (*count >= *cap) {
                        *cap = *cap ? *cap * 2 : 16;
                        *sigs = realloc(*sigs, *cap * sizeof(int));
                    }
                    if (*sigs) (*sigs)[(*count)++] = idx;
                }
            }
            return;
        }
        case UIR_RELEASE: {
            uir_release_t *r = (uir_release_t *)node;
            if (r->target && r->target->kind == UIR_REF) {
                uir_ref_t *ref = (uir_ref_t *)r->target;
                int idx = -1;
                if (ctx->current_prefix[0]) {
                    char prefixed[520];
                    snprintf(prefixed, sizeof(prefixed), "%s.%s",
                             ctx->current_prefix, ref->name);
                    idx = find_signal_idx(ctx, prefixed);
                }
                if (idx < 0) idx = find_signal_idx(ctx, ref->name);
                if (idx >= 0) {
                    if (*count >= *cap) {
                        *cap = *cap ? *cap * 2 : 16;
                        *sigs = realloc(*sigs, *cap * sizeof(int));
                    }
                    if (*sigs) (*sigs)[(*count)++] = idx;
                }
            }
            return;
        }
        case UIR_BLOCK: {
            uir_block_t *b = (uir_block_t *)node;
            for (size_t i = 0; i < b->stmt_count; i++)
                find_writes_in_node(b->stmts[i], ctx, sigs, count, cap);
            return;
        }
        case UIR_LOOP: {
            uir_loop_t *l = (uir_loop_t *)node;
            if (l->init_stmt) find_writes_in_node(l->init_stmt, ctx, sigs, count, cap);
            if (l->body) find_writes_in_node(l->body, ctx, sigs, count, cap);
            return;
        }
        case UIR_LOOP_BACK: {
            /* UIR_LOOP_BACK.body points back to the loop's augmented block,
             * forming a cycle. The body is already visited via UIR_LOOP's body,
             * so skip it here to avoid infinite recursion. */
            return;
        }
        case UIR_IF: {
            uir_if_t *i = (uir_if_t *)node;
            if (i->then_branch) find_writes_in_node(i->then_branch, ctx, sigs, count, cap);
            if (i->else_branch) find_writes_in_node(i->else_branch, ctx, sigs, count, cap);
            return;
        }
        case UIR_CASE: {
            uir_case_t *c = (uir_case_t *)node;
            for (size_t i = 0; i < c->item_count; i++) {
                if (c->items[i])
                    find_writes_in_node((uir_node_t *)c->items[i], ctx, sigs, count, cap);
            }
            return;
        }
        case UIR_CASE_ITEM: {
            uir_case_item_t *ci = (uir_case_item_t *)node;
            if (ci->body) find_writes_in_node(ci->body, ctx, sigs, count, cap);
            return;
        }
        case UIR_DELAY: {
            uir_delay_t *d = (uir_delay_t *)node;
            if (d->body) find_writes_in_node(d->body, ctx, sigs, count, cap);
            return;
        }
        case UIR_WAIT: {
            uir_wait_t *w = (uir_wait_t *)node;
            if (w->body) find_writes_in_node(w->body, ctx, sigs, count, cap);
            return;
        }
        default:
            return;
    }
}

/* Get signal name from UIR_SIGNAL or UIR_PORT node (same struct offset). */
static const char *sig_name(uir_node_t *node) {
    return ((uir_signal_t *)node)->name;
}

/* Build signal graph partitions (weakly-connected components) for
 * thread-parallel delta evaluation. Called at end of uir_sim_create. */
static void build_signal_partitions(uir_sim_context_t *ctx) {
    if (ctx->signal_count == 0) {
        ctx->partition_count = 0;
        return;
    }

    /* Edge type 1: Port wires (structural connectivity src↔dst) */
    int *parent = malloc(ctx->signal_count * sizeof(int));
    int *rank   = calloc(ctx->signal_count, sizeof(int));
    if (!parent || !rank) { free(parent); free(rank); return; }
    for (size_t i = 0; i < ctx->signal_count; i++) parent[i] = (int)i;

    for (size_t w = 0; w < ctx->port_wire_count; w++) {
        uf_union(parent, rank,
                 ctx->port_wires[w].src_sig_idx,
                 ctx->port_wires[w].dst_sig_idx);
    }

    /* Edge type 2: Continuous assigns (dataflow dep_sigs↔lhs) */
    for (size_t i = 0; i < ctx->cont_assign_count; i++) {
        cont_assign_entry_t *ca = &ctx->cont_assigns[i];
        uir_assign_t *a = ca->assign;
        if (!a || !a->lhs || a->lhs->kind != UIR_REF) continue;
        uir_ref_t *ref = (uir_ref_t *)a->lhs;
        int lhs_idx = -1;
        if (ca->prefix[0]) {
            char prefixed[520];
            snprintf(prefixed, sizeof(prefixed), "%s.%s", ca->prefix, ref->name);
            lhs_idx = find_signal_idx(ctx, prefixed);
        }
        if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
        if (lhs_idx < 0) continue;
        for (size_t d = 0; d < ca->dep_count; d++)
            uf_union(parent, rank, ca->dep_sigs[d], lhs_idx);
    }

    /* Edge type 3: Process body edges (sensitivity ↔ write targets) */
    for (size_t p = 0; p < ctx->process_count; p++) {
        uir_process_t *proc = ctx->processes[p];
        if (!proc) continue;

        /* Collect write targets via static analysis */
        int *writes = NULL;
        size_t write_count = 0, write_cap = 0;
        if (ctx->process_prefixes[p][0]) {
            strncpy(ctx->current_prefix, ctx->process_prefixes[p],
                    sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }
        if (proc->body) find_writes_in_node(proc->body, ctx, &writes, &write_count, &write_cap);
        if (ctx->process_prefixes[p][0])
            ctx->current_prefix[0] = '\0';

        if (proc->sensitivity_count > 0 && write_count > 0) {
            /* Union each sensitivity signal with each write target */
            for (size_t s = 0; s < proc->sensitivity_count; s++) {
                int sens_idx = -1;
                if (ctx->process_prefixes[p][0]) {
                    char prefixed[520];
                    if (proc->sensitivity_list[s].signal &&
                        (proc->sensitivity_list[s].signal->kind == UIR_SIGNAL ||
                         proc->sensitivity_list[s].signal->kind == UIR_PORT)) {
                        uir_node_t *sig = proc->sensitivity_list[s].signal;
                        snprintf(prefixed, sizeof(prefixed), "%s.%s",
                                 ctx->process_prefixes[p], sig_name(sig));
                        sens_idx = find_signal_idx(ctx, prefixed);
                    }
                }
                if (sens_idx < 0 && proc->sensitivity_list[s].signal &&
                    (proc->sensitivity_list[s].signal->kind == UIR_SIGNAL ||
                     proc->sensitivity_list[s].signal->kind == UIR_PORT)) {
                    sens_idx = find_signal_idx(ctx,
                        ((uir_signal_t *)proc->sensitivity_list[s].signal)->name);
                }
                if (sens_idx >= 0) {
                    for (size_t w = 0; w < write_count; w++)
                        uf_union(parent, rank, sens_idx, writes[w]);
                }
            }
        }
        free(writes);
    }

    /* Compress roots to contiguous partition IDs */
    ctx->signal_partition = malloc(ctx->signal_count * sizeof(int));
    int *root_to_part = calloc(ctx->signal_count, sizeof(int));
    ctx->partition_count = 0;
    if (!ctx->signal_partition || !root_to_part) {
        free(ctx->signal_partition); ctx->signal_partition = NULL;
        free(root_to_part);
        free(parent); free(rank);
        ctx->partition_count = 0;
        return;
    }
    for (size_t i = 0; i < ctx->signal_count; i++) {
        int root = uf_find(parent, (int)i);
        if (root_to_part[root] == 0 && (i == 0 || root != uf_find(parent, 0)))
            root_to_part[root] = -1; /* mark as unassigned for non-root-0 */
    }

    /* Re-scan: assign partition IDs properly */
    memset(root_to_part, -1, ctx->signal_count * sizeof(int));
    int part_id = 0;
    for (size_t i = 0; i < ctx->signal_count; i++) {
        int root = uf_find(parent, (int)i);
        if (root_to_part[root] < 0)
            root_to_part[root] = part_id++;
        ctx->signal_partition[i] = root_to_part[root];
    }
    ctx->partition_count = part_id;

    /* Assign processes to partitions */
    if (ctx->process_count > 0) {
        ctx->process_partition = malloc(ctx->process_count * sizeof(int));
        if (ctx->process_partition) {
            for (size_t p = 0; p < ctx->process_count; p++) {
                uir_process_t *proc = ctx->processes[p];
                ctx->process_partition[p] = 0; /* default to partition 0 */
                if (proc && proc->sensitivity_count > 0) {
                    for (size_t s = 0; s < proc->sensitivity_count; s++) {
                        if (!proc->sensitivity_list[s].signal) continue;
                        int sig_idx = -1;
                        if (proc->sensitivity_list[s].signal->kind == UIR_SIGNAL ||
                            proc->sensitivity_list[s].signal->kind == UIR_PORT) {
                            sig_idx = find_signal_idx(ctx,
                                ((uir_signal_t *)proc->sensitivity_list[s].signal)->name);
                        }
                        if (sig_idx >= 0) {
                            ctx->process_partition[p] = ctx->signal_partition[sig_idx];
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Assign CAs to partitions (by LHS signal) */
    if (ctx->cont_assign_count > 0) {
        ctx->ca_partition = calloc(ctx->cont_assign_count, sizeof(int));
        if (ctx->ca_partition) {
            for (size_t i = 0; i < ctx->cont_assign_count; i++) {
                cont_assign_entry_t *ca = &ctx->cont_assigns[i];
                uir_assign_t *a = ca->assign;
                if (!a || !a->lhs || a->lhs->kind != UIR_REF) continue;
                uir_ref_t *ref = (uir_ref_t *)a->lhs;
                int lhs_idx = -1;
                if (ca->prefix[0]) {
                    char prefixed[520];
                    snprintf(prefixed, sizeof(prefixed), "%s.%s", ca->prefix, ref->name);
                    lhs_idx = find_signal_idx(ctx, prefixed);
                }
                if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
                if (lhs_idx >= 0)
                    ctx->ca_partition[i] = ctx->signal_partition[lhs_idx];
            }
        }
    }

    free(parent);
    free(rank);
    free(root_to_part);
}

/* ── Event queue ── */

static void schedule_event(uir_sim_context_t *ctx, uint64_t time, uint32_t delta,
                            uint32_t sig_idx, qsim_bit_vector_t *value, int is_nba,
                            int ps_lo, int ps_hi) {
    /* NBA last-assignment-wins: if an NBA event for the same signal at the same
     * (time, delta) already exists from the SAME process, replace its value rather
     * than queuing a new event.  This implements VHDL "last signal assignment wins
     * within a process" semantics while preserving multi-driver resolution (events
     * from different processes must NOT replace each other). */
    if (is_nba && ctx->current_process_id >= 0) {
        if (tls_ts) {
            for (sim_event_t *e = tls_ts->pending_head; e; e = e->next) {
                if (e->time == time && e->delta == delta &&
                    e->sig_idx == sig_idx && e->is_nba &&
                    e->src_pid == ctx->current_process_id &&
                    e->ps_lo == ps_lo && e->ps_hi == ps_hi) {
                    qsim_bit_vector_free(e->value);
                    e->value = value;
                    e->has_part_select = (ps_lo >= 0 && ps_hi >= 0);
                    e->ps_lo = ps_lo;
                    e->ps_hi = ps_hi;
                    return;
                }
            }
        } else {
            for (sim_event_t *e = ctx->event_head; e; e = e->next) {
                if (e->time == time && e->delta == delta &&
                    e->sig_idx == sig_idx && e->is_nba &&
                    e->src_pid == ctx->current_process_id &&
                    e->ps_lo == ps_lo && e->ps_hi == ps_hi) {
                    qsim_bit_vector_free(e->value);
                    e->value = value;
                    e->has_part_select = (ps_lo >= 0 && ps_hi >= 0);
                    e->ps_lo = ps_lo;
                    e->ps_hi = ps_hi;
                    return;
                }
            }
        }
    }

    /* Parallel mode: route to per-thread pending list (lock-free) */
    if (tls_ts) {
        sim_event_t *ev = tls_ts->local_free_list;
        if (ev) {
            tls_ts->local_free_list = ev->next;
        } else {
            sim_mutex_lock(&ctx->pool_mutex);
            ev = pool_alloc_event(ctx);
            sim_mutex_unlock(&ctx->pool_mutex);
        }
        if (!ev) return;
        ev->time = time;
        ev->delta = delta;
        ev->sig_idx = sig_idx;
        ev->value = value;
        ev->is_nba = is_nba;
        ev->src_pid = ctx->current_process_id;
        ev->has_part_select = (ps_lo >= 0 && ps_hi >= 0);
        ev->ps_lo = ps_lo;
        ev->ps_hi = ps_hi;
        ev->is_stmt_event = 0;
        ev->stmt = NULL;
        ev->loop_always = 0;
        ev->owner_body = NULL;
        ev->next = NULL;
        /* Append to per-thread pending list (maintains creation order) */
        if (tls_ts->pending_tail) {
            tls_ts->pending_tail->next = ev;
            tls_ts->pending_tail = ev;
        } else {
            tls_ts->pending_head = tls_ts->pending_tail = ev;
        }
        tls_ts->pending_count++;
        return;
    }
    /* Non-parallel path: allocate from global pool, insert into global queue */
    sim_event_t *ev = pool_alloc_event(ctx);
    if (!ev) return;
    ev->time = time;
    ev->delta = delta;
    ev->sig_idx = sig_idx;
    ev->value = value;
    ev->is_nba = is_nba;
    ev->src_pid = ctx->current_process_id;
    ev->has_part_select = (ps_lo >= 0 && ps_hi >= 0);
    ev->ps_lo = ps_lo;
    ev->ps_hi = ps_hi;
    ev->is_stmt_event = 0;

    /* Insert in sorted order by (time, delta) */
    if (!ctx->event_head) {
        ctx->event_head = ev;
        ctx->event_tail = ev;
    } else {
        sim_event_t *prev = NULL, *cur = ctx->event_head;
        while (cur && (cur->time < time || (cur->time == time && cur->delta <= delta))) {
            prev = cur;
            cur = cur->next;
        }
        if (prev) {
            ev->next = prev->next;
            prev->next = ev;
        } else {
            ev->next = ctx->event_head;
            ctx->event_head = ev;
        }
        if (!ev->next) ctx->event_tail = ev;
    }

    ctx->event_count++;
}

static sim_event_t *pop_event(uir_sim_context_t *ctx) {
    if (!ctx->event_head) return NULL;
    sim_event_t *ev = ctx->event_head;
    ctx->event_head = ev->next;
    if (!ctx->event_head) ctx->event_tail = NULL;
    ctx->event_count--;
    return ev;
}

static void schedule_stmt_event(uir_sim_context_t *ctx, uint64_t time,
                                 uir_node_t *stmt, int loop_always,
                                 uir_node_t *owner_body,
                                 const char *block_hier) {
    sim_event_t *ev = pool_alloc_event(ctx);
    if (!ev) return;
    ev->time = time;
    ev->delta = 0;
    ev->is_stmt_event = 1;
    ev->stmt = stmt;
    ev->loop_always = loop_always;
    ev->owner_body = owner_body;
    if (block_hier && block_hier[0]) {
        strncpy(ev->block_hier, block_hier, sizeof(ev->block_hier) - 1);
        ev->block_hier[sizeof(ev->block_hier) - 1] = '\0';
    }

    /* Insert in sorted order by (time, delta), same as schedule_event */
    if (!ctx->event_head) {
        ctx->event_head = ev;
        ctx->event_tail = ev;
    } else {
        sim_event_t *prev = NULL, *cur = ctx->event_head;
        while (cur && (cur->time < time || (cur->time == time && cur->delta <= 0))) {
            /* Stmt events go after signal events at the same (time, delta)
             * so signal values settle before the stmt executes. */
            if (cur->time == time && cur->delta == 0 && !cur->is_stmt_event)
                { prev = cur; cur = cur->next; continue; }
            if (cur->time > time || (cur->time == time && cur->delta > 0)) break;
            prev = cur;
            cur = cur->next;
        }
        if (prev) {
            ev->next = prev->next;
            prev->next = ev;
        } else {
            ev->next = ctx->event_head;
            ctx->event_head = ev;
        }
        if (!ev->next) ctx->event_tail = ev;
    }
    ctx->event_count++;
}

/* ── Expression evaluation ── */

/* Forward declaration: called from eval_expr for built-in VHDL functions */
static qsim_bit_vector_t *eval_vhdl_builtin_func(
    uir_sim_context_t *ctx, uir_func_call_t *fc,
    const vhdl_builtin_func_t *builtin);

/* Evaluate a qualified function call like ieee.numeric_std.to_integer(a).
 * Splits the dotted name on the last '.', extracts the bare function name,
 * and dispatches through existing builtin paths. Returns NULL if the name
 * has no dot or no builtin matches. */
static qsim_bit_vector_t *eval_qualified_func_call(
    uir_sim_context_t *ctx, uir_func_call_t *fc)
{
    if (!fc->name) return NULL;

    /* Only handle dotted names */
    const char *last_dot = strrchr(fc->name, '.');
    if (!last_dot) return NULL;

    const char *func_name = last_dot + 1;
    size_t prefix_len = (size_t)(last_dot - fc->name);

    /* Check prefix belongs to an IEEE builtin package */
    if (prefix_len >= 4 && strncmp(fc->name, "ieee", 4) == 0) {
        /* Try numeric_std builtin */
        { int nk = match_numeric_std_builtin(func_name); if (nk) return numeric_std_eval_func(ctx, nk, fc); }
        /* Try VITAL builtin */
        { int vk = match_vital_builtin(func_name); if (vk) return vital_eval_func(ctx, vk, fc); }
        /* Try built-in function (rising_edge, falling_edge) */
        { const vhdl_builtin_func_t *bi = vhdl_lookup_builtin_func(func_name);
          if (bi) return eval_vhdl_builtin_func(ctx, fc, bi); }
    }

    return NULL;
}

static qsim_bit_vector_t *eval_expr(uir_sim_context_t *ctx, uir_node_t *node) {
    if (!node) return qsim_bit_vector_from_state(1, QSIM_X);

    switch (node->kind) {
        case UIR_LITERAL: {
            uir_literal_t *lit = (uir_literal_t *)node;
            return qsim_bit_vector_clone(lit->value);
        }
        case UIR_REF: {
            uir_ref_t *ref = (uir_ref_t *)node;
            int idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                idx = find_signal_idx(ctx, prefixed);
            }
            if (idx < 0) idx = find_signal_idx(ctx, ref->name);
            if (idx < 0) {
                /* Not a signal — check if this is a module parameter reference */
                if (ref->name) {
                    for (size_t ui = 0; ui < ctx->unit_count; ui++) {
                        uir_design_unit_t *u = ctx->units[ui];
                        for (size_t pi = 0; pi < u->param_count; pi++) {
                            if (u->params[pi].hier_path &&
                                strcmp(u->params[pi].hier_path, ref->name) == 0) {
                                return eval_expr(ctx, u->params[pi].value);
                            }
                        }
                    }
                    /* Check for numeric_std builtin type conversion: name(expr) pattern */
                    { int nk = match_numeric_std_builtin(ref->name); if (nk && ref->part_hi && ref->part_lo) {
                        uir_func_call_t fc;
                        memset(&fc, 0, sizeof(fc));
                        fc.base.kind = UIR_FUNC_CALL;
                        fc.name = ref->name;
                        fc.args = &ref->part_hi;
                        fc.arg_count = 1;
                        return numeric_std_eval_func(ctx, nk, &fc);
                    }}
                }
                return qsim_bit_vector_from_state(1, QSIM_X);
            }
            ctx->current_is_signed = ctx->signals[idx].is_signed;

            if (ref->part_hi && ref->part_lo) {
                /* Part-select read: extract bits [hi:lo] from signal value */
                qsim_bit_vector_t *hi_val = eval_expr(ctx, ref->part_hi);
                qsim_bit_vector_t *lo_val = eval_expr(ctx, ref->part_lo);
                if (!hi_val || !lo_val) {
                    if (hi_val) qsim_bit_vector_free(hi_val);
                    if (lo_val) qsim_bit_vector_free(lo_val);
                    return qsim_bit_vector_from_state(1, QSIM_X);
                }
                uint32_t hi = 0, lo = 0;
                for (uint32_t i = 0; i < hi_val->width && i < 32; i++)
                    if (qsim_bit_get(hi_val, i).state == QSIM_1) hi |= (1u << i);
                for (uint32_t i = 0; i < lo_val->width && i < 32; i++)
                    if (qsim_bit_get(lo_val, i).state == QSIM_1) lo |= (1u << i);
                qsim_bit_vector_free(hi_val);
                qsim_bit_vector_free(lo_val);
                uint32_t start_bit = (hi >= lo) ? lo : hi;
                uint32_t part_w = (hi >= lo) ? (hi - lo + 1) : (lo - hi + 1);
                qsim_bit_vector_t *full = ctx->signals[idx].value;
                qsim_bit_vector_t *result = qsim_bit_vector_alloc(part_w);
                if (!result) return qsim_bit_vector_from_state(part_w, QSIM_X);
                for (uint32_t i = 0; i < part_w && (start_bit + i) < full->width; i++)
                    qsim_bit_set(result, i, qsim_bit_get(full, start_bit + i));
                return result;
            }

            if (ref->multi_idx_count > 0) {
                /* Multi-dimensional array read: mem[i][j] */
                uir_node_t *sig_node = ctx->signals[idx].node;
                uint32_t elem_width = 1;
                uint32_t array_dims[4] = {0};
                size_t array_dim_count = 0;
                if (sig_node->kind == UIR_SIGNAL) {
                    uir_signal_t *sig = (uir_signal_t *)sig_node;
                    elem_width = sig->width;
                    array_dim_count = sig->array_dim_count;
                    for (size_t d = 0; d < array_dim_count && d < 4; d++)
                        array_dims[d] = sig->array_dims[d];
                }
                size_t nidxs = ref->multi_idx_count < array_dim_count ? ref->multi_idx_count : array_dim_count;
                uint32_t bit_off = 0;
                int idx_unknown = 0;
                for (size_t mi = 0; mi < nidxs; mi++) {
                    qsim_bit_vector_t *idx_val = eval_expr(ctx, ref->multi_index[mi]);
                    if (!idx_val) { idx_unknown = 1; break; }
                    uint32_t idx = 0;
                    for (uint32_t b = 0; b < idx_val->width && b < 32; b++) {
                        qsim_value_t bv = qsim_bit_get(idx_val, b);
                        if (bv.state == QSIM_1) idx |= (1u << b);
                        else if (bv.state != QSIM_0) { idx_unknown = 1; break; }
                    }
                    qsim_bit_vector_free(idx_val);
                    if (idx_unknown) break;
                    /* stride = product of remaining dims * elem_width */
                    uint32_t stride = elem_width;
                    for (size_t k = mi + 1; k < array_dim_count; k++)
                        stride *= array_dims[k];
                    bit_off += idx * stride;
                }
                if (idx_unknown)
                    return qsim_bit_vector_from_state(elem_width, QSIM_X);
                qsim_bit_vector_t *full = ctx->signals[idx].value;
                qsim_bit_vector_t *result = qsim_bit_vector_alloc(elem_width);
                if (!result) return qsim_bit_vector_from_state(elem_width, QSIM_X);
                for (uint32_t i = 0; i < elem_width && (bit_off + i) < full->width; i++)
                    qsim_bit_set(result, i, qsim_bit_get(full, bit_off + i));
                return result;
            }

            if (ref->index) {
                /* Indexed read: array element access or bit-select */
                uir_node_t *sig_node = ctx->signals[idx].node;
                int is_array = 0;
                uint32_t elem_width = 1;
                if (sig_node->kind == UIR_SIGNAL) {
                    uir_signal_t *sig = (uir_signal_t *)sig_node;
                    elem_width = sig->width;
                    is_array = (sig->array_size > 0);
                } else if (sig_node->kind == UIR_PORT) {
                    elem_width = ((uir_port_t *)sig_node)->width;
                }
                uint32_t result_width = is_array ? elem_width : 1;
                qsim_bit_vector_t *index_val = eval_expr(ctx, ref->index);
                if (!index_val) return qsim_bit_vector_from_state(result_width, QSIM_X);
                uint32_t elem_idx = 0;
                for (uint32_t i = 0; i < index_val->width && i < 32; i++) {
                    qsim_value_t b = qsim_bit_get(index_val, i);
                    if (b.state == QSIM_1) elem_idx |= (1u << i);
                    else if (b.state != QSIM_0)
                        { qsim_bit_vector_free(index_val); return qsim_bit_vector_from_state(result_width, QSIM_X); }
                }
                qsim_bit_vector_free(index_val);
                /* Bounds check: protect against out-of-range index */
                if (is_array && sig_node->kind == UIR_SIGNAL) {
                    uir_signal_t *sig = (uir_signal_t *)sig_node;
                    if (elem_idx >= sig->array_size)
                        return qsim_bit_vector_from_state(result_width, QSIM_X);
                }
                uint32_t bit_off = is_array ? (elem_idx * elem_width) : elem_idx;
                qsim_bit_vector_t *full = ctx->signals[idx].value;
                qsim_bit_vector_t *result = qsim_bit_vector_alloc(result_width);
                if (!result) return qsim_bit_vector_from_state(result_width, QSIM_X);
                for (uint32_t i = 0; i < result_width && (bit_off + i) < full->width; i++)
                    qsim_bit_set(result, i, qsim_bit_get(full, bit_off + i));
                return result;
            }
            return qsim_bit_vector_clone(ctx->signals[idx].value);
        }
        case UIR_EXPR_BINARY: {
            uir_expr_t *expr = (uir_expr_t *)node;
            int a_signed = ctx->current_is_signed;
            qsim_bit_vector_t *a = eval_expr(ctx, expr->operand_a);
            int b_signed = ctx->current_is_signed;
            qsim_bit_vector_t *b = eval_expr(ctx, expr->operand_b);
            if (!a || !b) {
                if (a) qsim_bit_vector_free(a);
                if (b) qsim_bit_vector_free(b);
                return qsim_bit_vector_from_state(1, QSIM_X);
            }
            int signed_comp = a_signed || b_signed;
            qsim_bit_vector_t *r = bv_binary_op(a, b, expr->op.bin_op, ctx->current_context_width, signed_comp);
            ctx->current_is_signed = signed_comp;
            qsim_bit_vector_free(a);
            qsim_bit_vector_free(b);
            return r;
        }
        case UIR_EXPR_UNARY: {
            uir_expr_t *expr = (uir_expr_t *)node;
            if (expr->op.un_op == UIR_OP_OTHERS) {
                /* (others => expr): fill to current_context_width */
                qsim_bit_vector_t *a = eval_expr(ctx, expr->operand_a);
                if (!a) return qsim_bit_vector_from_state(1, QSIM_X);
                uint32_t target_w = ctx->current_context_width;
                if (target_w == 0 || target_w == a->width) return a;
                qsim_bit_vector_t *r = qsim_bit_vector_alloc(target_w);
                if (!r) { qsim_bit_vector_free(a); return qsim_bit_vector_from_state(1, QSIM_X); }
                for (uint32_t i = 0; i < target_w; i++)
                    qsim_bit_set(r, i, qsim_bit_get(a, i % a->width));
                qsim_bit_vector_free(a);
                return r;
            }
            qsim_bit_vector_t *a = eval_expr(ctx, expr->operand_a);
            if (!a) return qsim_bit_vector_from_state(1, QSIM_X);
            qsim_bit_vector_t *r = bv_unary_op(a, expr->op.un_op);
            qsim_bit_vector_free(a);
            return r;
        }
        case UIR_COND: {
            uir_cond_t *cond = (uir_cond_t *)node;
            qsim_bit_vector_t *c = eval_expr(ctx, cond->condition);
            qsim_bit_vector_t *t = eval_expr(ctx, cond->then_expr);
            qsim_bit_vector_t *e = eval_expr(ctx, cond->else_expr);
            qsim_bit_vector_t *r;
            if (c && t && e && c->width > 0 &&
                qsim_bit_get(c, 0).state == QSIM_1) {
                r = t; qsim_bit_vector_free(c); qsim_bit_vector_free(e);
            } else if (c && t && e && c->width > 0 &&
                       qsim_bit_get(c, 0).state == QSIM_0) {
                r = e; qsim_bit_vector_free(c); qsim_bit_vector_free(t);
            } else {
                /* X/Z condition: return X of max width */
                uint32_t w = 1;
                if (t && e) w = t->width > e->width ? t->width : e->width;
                else if (t) w = t->width;
                r = qsim_bit_vector_from_state(w, QSIM_X);
                if (c) qsim_bit_vector_free(c);
                if (t) qsim_bit_vector_free(t);
                if (e) qsim_bit_vector_free(e);
            }
            return r;
        }
        case UIR_FUNC_CALL: {
            uir_func_call_t *fc = (uir_func_call_t *)node;

            /* Check for qualified name (lib.pkg.func) — dispatch early */
            { qsim_bit_vector_t *qr = eval_qualified_func_call(ctx, fc); if (qr) return qr; }
            /* Check for numeric_std builtin function */
            { int nk = match_numeric_std_builtin(fc->name); if (nk) return numeric_std_eval_func(ctx, nk, fc); }
            /* Check for VITAL builtin function */
            { int vk = match_vital_builtin(fc->name); if (vk) return vital_eval_func(ctx, vk, fc); }
            /* Check for built-in VHDL function (rising_edge, falling_edge) */
            {
                const vhdl_builtin_func_t *builtin = vhdl_lookup_builtin_func(fc->name);
                if (builtin)
                    return eval_vhdl_builtin_func(ctx, fc, builtin);
            }
            /* Check for TEXTIO builtin functions */
            if (strcmp(fc->name, "endfile") == 0) {
                int eof = textio_endfile(ctx, fc);
                qsim_bit_vector_t *r = qsim_bit_vector_alloc(1);
                qsim_bit_set(r, 0, eof ? QSIM_VAL_1 : QSIM_VAL_0);
                return r;
            }
            /* Find matching function frame */
            func_frame_t *frame = NULL;
            for (size_t i = 0; i < ctx->func_frame_count; i++) {
                if (strcmp(ctx->func_frames[i].def->name, fc->name) == 0) {
                    frame = &ctx->func_frames[i];
                    break;
                }
            }
            if (!frame || !frame->def->is_function)
                return qsim_bit_vector_from_state(1, QSIM_X);

            uir_func_t *ft = frame->def;

            /* Save current prefix */
            char saved_prefix[256] = "";
            strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);

            /* Save automatic frame state and re-initialize to X */
            qsim_bit_vector_t **auto_saved = NULL;
            if (frame->is_automatic && frame->auto_sig_count > 0) {
                auto_saved = calloc((size_t)frame->auto_sig_count, sizeof(qsim_bit_vector_t *));
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count)
                        auto_saved[sai] = qsim_bit_vector_clone(ctx->signals[sidx].value);
                }
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count) {
                        uint32_t w = ctx->signals[sidx].value->width;
                        qsim_bit_vector_free(ctx->signals[sidx].value);
                        ctx->signals[sidx].value = qsim_bit_vector_from_state(w, QSIM_X);
                    }
                }
            }

            /* Evaluate args and write to port signals */
            size_t arg_count = fc->arg_count < ft->port_count ? fc->arg_count : ft->port_count;
            for (size_t i = 0; i < arg_count; i++) {
                qsim_bit_vector_t *val = eval_expr(ctx, fc->args[i]);
                if (val) {
                    int port_idx = frame->port_sig_indices[i];
                    if (port_idx >= 0) {
                        qsim_bit_vector_free(ctx->signals[port_idx].value);
                        ctx->signals[port_idx].value = val;
                    } else {
                        qsim_bit_vector_free(val);
                    }
                }
            }

            /* Set function prefix and execute body */
            strncpy(ctx->current_prefix, frame->prefix, sizeof(ctx->current_prefix) - 1);
            exec_stmt(ctx, ft->body);

            /* Read return value BEFORE restoring automatic state */
            qsim_bit_vector_t *result = NULL;
            if (frame->return_sig_idx >= 0)
                result = qsim_bit_vector_clone(ctx->signals[frame->return_sig_idx].value);

            /* Restore automatic frame state */
            if (auto_saved) {
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count && auto_saved[sai]) {
                        qsim_bit_vector_free(ctx->signals[sidx].value);
                        ctx->signals[sidx].value = auto_saved[sai];
                    }
                }
                free(auto_saved);
            }

            /* Restore prefix */
            strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);

            if (result) return result;
            return qsim_bit_vector_from_state(1, QSIM_X);
        }
        case UIR_SYS_FUNC_EXPR: {
            uir_sys_func_expr_t *sf = (uir_sys_func_expr_t *)node;
            switch (sf->func_kind) {
                case UIR_SYS_FUNC_SIGNED:
                    ctx->current_is_signed = 1;
                    if (sf->arg_count > 0 && sf->args[0])
                        return eval_expr(ctx, sf->args[0]);
                    return qsim_bit_vector_from_state(1, QSIM_X);
                case UIR_SYS_FUNC_UNSIGNED:
                    ctx->current_is_signed = 0;
                    if (sf->arg_count > 0 && sf->args[0])
                        return eval_expr(ctx, sf->args[0]);
                    return qsim_bit_vector_from_state(1, QSIM_X);
                case UIR_SYS_FUNC_CLOG2: {
                    if (sf->arg_count < 1 || !sf->args[0])
                        return qsim_bit_vector_from_state(32, QSIM_X);
                    qsim_bit_vector_t *val = eval_expr(ctx, sf->args[0]);
                    if (!val) return qsim_bit_vector_from_state(32, QSIM_X);
                    uint64_t v = 0;
                    for (uint32_t i = 0; i < val->width && i < 64; i++) {
                        qsim_value_t b = qsim_bit_get(val, i);
                        if (b.state == QSIM_X || b.state == QSIM_Z) {
                            qsim_bit_vector_free(val);
                            return qsim_bit_vector_from_state(32, QSIM_X);
                        }
                        if (b.state == QSIM_1) v |= (1ULL << i);
                    }
                    qsim_bit_vector_free(val);
                    if (v <= 1) {
                        qsim_bit_vector_t *r = qsim_bit_vector_alloc(32);
                        if (r) for (int i = 0; i < 32; i++) qsim_bit_set(r, i, QSIM_VAL_0);
                        return r;
                    }
                    v--;
                    uint32_t result = 0;
                    while (v > 0) { v >>= 1; result++; }
                    {
                        qsim_bit_vector_t *r = qsim_bit_vector_alloc(32);
                        if (r) for (uint32_t i = 0; i < 32; i++)
                            qsim_bit_set(r, i, (result >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                        return r;
                    }
                }
                case UIR_SYS_FUNC_TIME:
                case UIR_SYS_FUNC_REALTIME: {
                    qsim_bit_vector_t *r = qsim_bit_vector_alloc(64);
                    if (r) for (uint32_t i = 0; i < 64; i++)
                        qsim_bit_set(r, i, (ctx->current_time >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                    return r;
                }
                case UIR_SYS_FUNC_RANDOM: {
                    if (sf->arg_count > 0 && sf->args[0]) {
                        qsim_bit_vector_t *seed_val = eval_expr(ctx, sf->args[0]);
                        if (seed_val) {
                            uint64_t s = 0;
                            for (uint32_t i = 0; i < seed_val->width && i < 64; i++) {
                                qsim_value_t b = qsim_bit_get(seed_val, i);
                                if (b.state == QSIM_1) s |= (1ULL << i);
                            }
                            qsim_bit_vector_free(seed_val);
                            ctx->rand_state = s;
                        }
                    }
                    ctx->rand_state = ctx->rand_state * 1103515245ULL + 12345ULL;
                    uint32_t rv = (uint32_t)((ctx->rand_state >> 16) & 0x7FFFFFFF);
                    qsim_bit_vector_t *r = qsim_bit_vector_alloc(32);
                    if (r) for (uint32_t i = 0; i < 32; i++)
                        qsim_bit_set(r, i, (rv >> i) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                    return r;
                }
                default:
                    return qsim_bit_vector_from_state(32, QSIM_X);
            }
        }
        default:
            return qsim_bit_vector_from_state(1, QSIM_X);
    }
}

/* ── Sensitivity checking ── */

static int edge_match(int edge_type, qsim_value_t old_val, qsim_value_t new_val) {
    /* IEEE 1364-2005 §9.3.2.2: posedge = 0→1, X→1, Z→1 (not 1→1) */
    if (edge_type == 1)  /* posedge */ return new_val.state == QSIM_1 && old_val.state != QSIM_1;
    /* negedge = 1→0, X→0, Z→0 (not 0→0) */
    if (edge_type == -1) /* negedge */ return new_val.state == QSIM_0 && old_val.state != QSIM_0;
    return old_val.state != new_val.state; /* level-sensitive: any change */
}

/* ── Wait/event-control waiter helpers ── */

/* Add a waiter for a UIR_WAIT or UIR_EVENT_CTRL node.
 * Discovers signal dependencies from the node and registers them. */
static void add_waiter(uir_sim_context_t *ctx, uir_node_t *node,
                        const char *block_hier) {
    int *sigs = NULL;
    size_t count = 0, cap = 0;
    if (node->kind == UIR_WAIT) {
        find_refs_in_node(((uir_wait_t *)node)->condition, ctx, &sigs, &count, &cap);
    } else if (node->kind == UIR_EVENT_CTRL) {
        uir_event_ctrl_t *ec = (uir_event_ctrl_t *)node;
        int idx = find_signal_idx(ctx, ec->signal_name);
        if (idx >= 0) {
            if (count >= cap) { cap = cap ? cap * 2 : 8; sigs = realloc(sigs, cap * sizeof(int)); }
            if (sigs) sigs[count++] = idx;
        }
    }
    if (count == 0) { free(sigs); return; }

    if (ctx->waiter_count >= ctx->waiter_cap) {
        size_t nc = ctx->waiter_cap ? ctx->waiter_cap * 2 : 8;
        sim_waiter_t *n = realloc(ctx->waiters, nc * sizeof(sim_waiter_t));
        if (!n) { free(sigs); return; }
        ctx->waiters = n;
        ctx->waiter_cap = nc;
    }
    ctx->waiters[ctx->waiter_count].node = node;
    ctx->waiters[ctx->waiter_count].sig_indices = sigs;
    ctx->waiters[ctx->waiter_count].sig_count = count;
    ctx->waiters[ctx->waiter_count].cancelled = 0;
    if (block_hier && block_hier[0]) {
        strncpy(ctx->waiters[ctx->waiter_count].block_hier, block_hier,
                sizeof(ctx->waiters[ctx->waiter_count].block_hier) - 1);
        ctx->waiters[ctx->waiter_count].block_hier[
            sizeof(ctx->waiters[ctx->waiter_count].block_hier) - 1] = '\0';
    } else {
        ctx->waiters[ctx->waiter_count].block_hier[0] = '\0';
    }
    ctx->waiter_count++;
}

/* Check all waiters when a signal changes. If a waiter's condition is met,
 * execute its body and remove it. Called after signal value updates. */
static void check_waiters_on_change(uir_sim_context_t *ctx, uint32_t changed_sig_idx,
                                     const qsim_bit_vector_t *old_val,
                                     const qsim_bit_vector_t *new_val) {
    size_t w = 0;
    while (w < ctx->waiter_count) {
        sim_waiter_t *waiter = &ctx->waiters[w];
        if (waiter->cancelled) {
            free(waiter->sig_indices);
            size_t last = ctx->waiter_count - 1;
            if (w < last)
                ctx->waiters[w] = ctx->waiters[last];
            ctx->waiter_count--;
            continue;
        }
        int relevant = 0;
        for (size_t s = 0; s < waiter->sig_count; s++) {
            if (waiter->sig_indices[s] == (int)changed_sig_idx) {
                relevant = 1; break;
            }
        }
        if (!relevant) { w++; continue; }

        int fire = 0;
        if (waiter->node->kind == UIR_WAIT) {
            uir_wait_t *wt = (uir_wait_t *)waiter->node;
            qsim_bit_vector_t *cond_val = eval_expr(ctx, wt->condition);
            if (cond_val) {
                for (uint32_t b = 0; b < cond_val->width; b++)
                    if (qsim_bit_get(cond_val, b).state == QSIM_1) { fire = 1; break; }
                qsim_bit_vector_free(cond_val);
            }
        } else if (waiter->node->kind == UIR_EVENT_CTRL) {
            uir_event_ctrl_t *ec = (uir_event_ctrl_t *)waiter->node;
            if (ec->edge == 0) {
                fire = 1; /* Any change on the watched signal fires */
            } else {
                qsim_value_t old_first = {QSIM_0, QSIM_STRENGTH_STRONG};
                qsim_value_t new_first = {QSIM_0, QSIM_STRENGTH_STRONG};
                if (old_val && old_val->width > 0) old_first = qsim_bit_get(old_val, 0);
                if (new_val && new_val->width > 0) new_first = qsim_bit_get(new_val, 0);
                fire = edge_match(ec->edge, old_first, new_first);
            }
        }

        if (fire) {
            if (waiter->node->kind == UIR_WAIT)
                exec_stmt(ctx, ((uir_wait_t *)waiter->node)->body);
            else if (waiter->node->kind == UIR_EVENT_CTRL)
                exec_stmt(ctx, ((uir_event_ctrl_t *)waiter->node)->body);
            /* Remove waiter */
            free(waiter->sig_indices);
            if (w < ctx->waiter_count - 1)
                ctx->waiters[w] = ctx->waiters[ctx->waiter_count - 1];
            ctx->waiter_count--;
            /* Don't increment w — we moved a waiter into this slot */
        } else {
            w++;
        }
    }
}

/* Forward declarations for statement executors */
static void exec_stmt(uir_sim_context_t *ctx, uir_node_t *stmt);

static void check_and_trigger_impl(uir_sim_context_t *ctx, uint32_t sig_idx,
                                    const qsim_bit_vector_t *old_val,
                                    const qsim_bit_vector_t *new_val,
                                    int depth, int exec_processes);

static void check_and_trigger(uir_sim_context_t *ctx, uint32_t sig_idx,
                               const qsim_bit_vector_t *old_val,
                               const qsim_bit_vector_t *new_val) {
    check_and_trigger_impl(ctx, sig_idx, old_val, new_val, 0, 1);
}

/* CA-only wrapper: evaluates continuous assigns but skips process triggering.
 * Used in parallel mode where process execution is done by process_partition_triggers. */
static void check_and_trigger_ca_only(uir_sim_context_t *ctx, uint32_t sig_idx,
                                       const qsim_bit_vector_t *old_val,
                                       const qsim_bit_vector_t *new_val) {
    check_and_trigger_impl(ctx, sig_idx, old_val, new_val, 0, 0);
}

static void check_and_trigger_impl(uir_sim_context_t *ctx, uint32_t sig_idx,
                                    const qsim_bit_vector_t *old_val,
                                    const qsim_bit_vector_t *new_val,
                                    int depth, int exec_processes) {
    /* Safety limit for cascaded continuous assigns */
    if (depth > 16) return;

    uir_node_t *sig_node = ctx->signals[sig_idx].node;

    /* ── Phase 1: Evaluate continuous assigns ──
     * For delay=0 (Verilog continuous assign): update target signal
     * directly (not via event queue) so processes see up-to-date values
     * in the same delta.
     * For delay>0 (VHDL concurrent signal assignment): schedule through
     * the event queue so the update takes effect in the next delta. */
    for (size_t i = 0; i < ctx->cont_assign_count; i++) {
        cont_assign_entry_t *entry = &ctx->cont_assigns[i];
        int depends = 0;
        for (size_t d = 0; d < entry->dep_count; d++) {
            if (entry->dep_sigs[d] == (int)sig_idx) { depends = 1; break; }
        }
        if (!depends) continue;

        uir_assign_t *a = entry->assign;
        uint32_t saved_cw = ctx->current_context_width;
        char saved_pfx[520] = "";
        if (entry->prefix[0]) {
            strncpy(saved_pfx, ctx->current_prefix, sizeof(saved_pfx) - 1);
            saved_pfx[sizeof(saved_pfx) - 1] = '\0';
            strncpy(ctx->current_prefix, entry->prefix, sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }
        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lhs_idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lhs_idx = find_signal_idx(ctx, prefixed);
            }
            if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
            if (lhs_idx >= 0)
                ctx->current_context_width = ctx->signals[lhs_idx].value->width;
        }
        qsim_bit_vector_t *rhs_val = eval_expr(ctx, a->rhs);
        ctx->current_context_width = saved_cw;
        if (!rhs_val) {
            if (entry->prefix[0])
                strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
            continue;
        }

        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lhs_idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lhs_idx = find_signal_idx(ctx, prefixed);
            }
            if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
            if (lhs_idx >= 0) {
                uint32_t target_w = ctx->signals[lhs_idx].value->width;
                qsim_bit_vector_t *final_val = rhs_val;
                if (rhs_val->width != target_w) {
                    final_val = qsim_bit_vector_alloc(target_w);
                    if (final_val) {
                        for (uint32_t b = 0; b < target_w; b++) {
                            if (b < rhs_val->width)
                                qsim_bit_set(final_val, b, qsim_bit_get(rhs_val, b));
                            else
                                qsim_bit_set(final_val, b, QSIM_VAL_0);
                        }
                    }
                }

                qsim_bit_vector_t *to_apply = final_val ? final_val : rhs_val;

                /* Handle array-indexed LHS: write only the targeted element */
                qsim_bit_vector_t *arr_result = apply_array_write(ctx, ref, lhs_idx, to_apply);
                if (arr_result) {
                    if (final_val && final_val != rhs_val) qsim_bit_vector_free(final_val);
                    final_val = arr_result;
                    to_apply = arr_result;
                }

                /* Handle part-select LHS: write only the selected bits */
                qsim_bit_vector_t *ps_result = apply_part_select_write(ctx, ref, lhs_idx, to_apply);
                if (ps_result) {
                    if (final_val && final_val != rhs_val) qsim_bit_vector_free(final_val);
                    final_val = ps_result;
                    to_apply = ps_result;
                }

                if (a->delay > 0) {
                    /* VHDL: immediate write so Phase 2 process execution sees the
                     * updated CSA output in the same delta. */
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lhs_idx, to_apply, &old_ca, -1);
                    if (ca_changed && ctx->event_cb) {
                        ctx->event_cb(ctx, ctx->signals[lhs_idx].name,
                                      ctx->signals[lhs_idx].value,
                                      ctx->event_cb_user_data);
                    }
                    if (ca_changed && (uint32_t)lhs_idx != sig_idx) {
                        if (exec_processes) {
                            check_and_trigger_impl(ctx, (uint32_t)lhs_idx, old_ca,
                                                   ctx->signals[lhs_idx].value, depth + 1, 1);
                        } else {
                            if (ctx->ca_changed_count >= ctx->ca_changed_cap) {
                                size_t nc = ctx->ca_changed_cap ? ctx->ca_changed_cap * 2 : 64;
                                int *ns = realloc(ctx->ca_changed_sigs, nc * sizeof(int));
                                qsim_bit_vector_t **no = realloc(ctx->ca_changed_old_vals,
                                                                  nc * sizeof(qsim_bit_vector_t *));
                                if (ns && no) {
                                    ctx->ca_changed_sigs = ns;
                                    ctx->ca_changed_old_vals = no;
                                    ctx->ca_changed_cap = (int)nc;
                                }
                            }
                            if (ctx->ca_changed_count < ctx->ca_changed_cap) {
                                ctx->ca_changed_sigs[ctx->ca_changed_count] = (int)lhs_idx;
                                ctx->ca_changed_old_vals[ctx->ca_changed_count] = old_ca;
                                ctx->ca_changed_count++;
                            } else {
                                qsim_bit_vector_free(old_ca);
                            }
                            check_and_trigger_impl(ctx, (uint32_t)lhs_idx, NULL,
                                                   ctx->signals[lhs_idx].value, depth + 1, 0);
                        }
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                } else if (a->delay_value) {
                    /* Verilog: inertial delay assign #N lhs = rhs;
                     * Evaluate the delay expression and schedule the update. */
                    qsim_bit_vector_t *dval = eval_expr(ctx, a->delay_value);
                    uint64_t n = 0;
                    if (dval) {
                        for (uint32_t i = 0; i < dval->width && i < 64; i++)
                            if (qsim_bit_get(dval, i).state == QSIM_1) n |= (1ULL << i);
                        qsim_bit_vector_free(dval);
                    }
                    uint64_t target_time = ctx->current_time + n;

                    /* Inertial filtering: remove any existing events for the
                     * same signal at times earlier than target_time (pulse
                     * narrower than the propagation delay is filtered out). */
                    sim_event_t *prev = NULL, *cur = ctx->event_head;
                    while (cur) {
                        if (cur->sig_idx == (uint32_t)lhs_idx &&
                            cur->time < target_time && !cur->is_stmt_event) {
                            sim_event_t *victim = cur;
                            if (prev) prev->next = cur->next;
                            else ctx->event_head = cur->next;
                            cur = cur->next;
                            if (!cur) ctx->event_tail = prev;
                            ctx->event_count--;
                            pool_free_event(ctx, victim);
                        } else {
                            prev = cur;
                            cur = cur->next;
                        }
                    }

                    schedule_event(ctx, target_time, 0,
                                   (uint32_t)lhs_idx, qsim_bit_vector_clone(to_apply), 0, -1, -1);
                } else {
                    /* Verilog: direct update (zero delay) — use resolution for net types */
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lhs_idx, to_apply, &old_ca, (int)i);
                    if (ca_changed && ctx->event_cb) {
                        ctx->event_cb(ctx, ctx->signals[lhs_idx].name,
                                      ctx->signals[lhs_idx].value,
                                      ctx->event_cb_user_data);
                    }

                    if (ca_changed && (uint32_t)lhs_idx != sig_idx) {
                        if (exec_processes) {
                            check_and_trigger_impl(ctx, (uint32_t)lhs_idx, old_ca,
                                                   ctx->signals[lhs_idx].value, depth + 1, 1);
                        } else {
                            /* Record CA-generated change for parallel Phase 2b process triggering */
                            if (ctx->ca_changed_count >= ctx->ca_changed_cap) {
                                size_t nc = ctx->ca_changed_cap ? ctx->ca_changed_cap * 2 : 64;
                                int *ns = realloc(ctx->ca_changed_sigs, nc * sizeof(int));
                                qsim_bit_vector_t **no = realloc(ctx->ca_changed_old_vals,
                                                                  nc * sizeof(qsim_bit_vector_t *));
                                if (ns && no) {
                                    ctx->ca_changed_sigs = ns;
                                    ctx->ca_changed_old_vals = no;
                                    ctx->ca_changed_cap = (int)nc;
                                }
                            }
                            if (ctx->ca_changed_count < ctx->ca_changed_cap) {
                                ctx->ca_changed_sigs[ctx->ca_changed_count] = (int)lhs_idx;
                                ctx->ca_changed_old_vals[ctx->ca_changed_count] = old_ca;
                                ctx->ca_changed_count++;
                            } else {
                                qsim_bit_vector_free(old_ca);
                            }
                            /* Recursive CA-only for cascaded continuous assigns */
                            check_and_trigger_impl(ctx, (uint32_t)lhs_idx, NULL,
                                                   ctx->signals[lhs_idx].value, depth + 1, 0);
                        }
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                }

                if (final_val && final_val != rhs_val)
                    qsim_bit_vector_free(final_val);
            }
        }
        qsim_bit_vector_free(rhs_val);
        if (entry->prefix[0])
            strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
    }

    /* ── Phase 1.5: UDP instance evaluation ──
     * When a signal that is a UDP instance input changes, evaluate the
     * truth table and update the output inline (same delta cycle). */
    for (size_t ui = 0; ui < ctx->udp_instance_count; ui++) {
        udp_instance_entry_t *ue = &ctx->udp_instances[ui];
        /* Check if changed signal is an input of this UDP instance */
        int is_input = 0;
        for (size_t k = 0; k < ue->input_count; k++) {
            if (ue->input_sig_indices[k] == (int)sig_idx) { is_input = 1; break; }
        }
        if (!is_input) continue;

        if (ue->udp->is_sequential) {
            /* Sequential UDPs: only evaluate on first input (clock) edge */
            if ((int)sig_idx == ue->input_sig_indices[0])
                evaluate_sequential_udp(ctx, ue, old_val, new_val);
        } else {
            evaluate_combinational_udp(ctx, ue);
        }
    }

    if (exec_processes) {
        /* ── Phase 2: Trigger processes sensitive to this signal ──
         * Before executing, save a snapshot of all port-wire source signal
         * values so we can detect side-effect changes from blocking assigns
         * inside process bodies and propagate them through port wires. */
        qsim_bit_vector_t **pw_saved = NULL;
        if (ctx->port_wire_count > 0 && ctx->port_wire_srcs && ctx->port_wire_src_count > 0) {
            pw_saved = malloc(ctx->port_wire_src_count * sizeof(qsim_bit_vector_t *));
            if (pw_saved) {
                for (size_t i = 0; i < ctx->port_wire_src_count; i++)
                    pw_saved[i] = qsim_bit_vector_clone(ctx->signals[ctx->port_wire_srcs[i]].value);
            }
        }
        for (size_t p = 0; p < ctx->process_count; p++) {
            uir_process_t *proc = ctx->processes[p];
            /* Refresh new_val: a previous iteration's exec_stmt may have
             * freed ctx->signals[sig_idx].value and allocated a new one
             * via exec_assign (blocking assign), making the old pointer
             * dangling. Re-read the current signal value. */
            new_val = ctx->signals[sig_idx].value;
            for (size_t s = 0; s < proc->sensitivity_count; s++) {
                if (proc->sensitivity_list[s].signal != sig_node) {
                    continue;
                }

                /* Check edge condition */
                if (proc->sensitivity_list[s].edge == 0) {
                    /* Level-sensitive: any bit change triggers */
                    int any_change = 0;
                    uint32_t sw = old_val->width < new_val->width
                        ? old_val->width : new_val->width;
                    for (uint32_t b = 0; b < sw; b++) {
                        if (qsim_bit_get(old_val, b).state != qsim_bit_get(new_val, b).state) {
                            any_change = 1; break;
                        }
                    }
                    if (any_change) {
                        strncpy(ctx->current_prefix, ctx->process_prefixes[p], 255);
                        ctx->current_process_id = (int)p;
                        exec_stmt(ctx, proc->body);
                        ctx->current_process_id = -1;
                        ctx->current_prefix[0] = '\0';
                        break;
                    }
                } else {
                    /* Edge-sensitive: check only bit 0 (standard Verilog behavior) */
                    qsim_value_t old_first = {QSIM_0, QSIM_STRENGTH_STRONG};
                    qsim_value_t new_first = {QSIM_0, QSIM_STRENGTH_STRONG};
                    if (old_val && old_val->width > 0) old_first = qsim_bit_get(old_val, 0);
                    if (new_val && new_val->width > 0) new_first = qsim_bit_get(new_val, 0);

                    if (edge_match(proc->sensitivity_list[s].edge, old_first, new_first)) {
                        strncpy(ctx->current_prefix, ctx->process_prefixes[p], 255);
                        ctx->current_process_id = (int)p;
                        exec_stmt(ctx, proc->body);
                        ctx->current_process_id = -1;
                        ctx->current_prefix[0] = '\0';
                        break;
                    }
                }
            }
        }

        /* Phase 2b: Detect side-effect signal changes from process body
         * execution and propagate through port connection wires. Without this,
         * blocking assigns inside processes (e.g. always @(a) y = a) update
         * signals directly but never trigger port wire propagation, so
         * output port changes never reach the parent. */
        if (pw_saved) {
            for (size_t w = 0; w < ctx->port_wire_count; w++) {
                uint32_t src_idx = (uint32_t)ctx->port_wires[w].src_sig_idx;
                if (src_idx == sig_idx) continue; /* already handled in Phase 3 */
                /* Look up saved value for this port wire source signal */
                qsim_bit_vector_t *old = NULL;
                for (size_t si = 0; si < ctx->port_wire_src_count; si++) {
                    if (ctx->port_wire_srcs[si] == (int)src_idx) {
                        old = pw_saved[si]; break;
                    }
                }
                if (!old) continue;
                uint32_t cmp_w = old->width < ctx->signals[src_idx].value->width
                    ? old->width : ctx->signals[src_idx].value->width;
                int pw_changed = 0;
                for (uint32_t b = 0; b < cmp_w; b++) {
                    if (qsim_bit_get(old, b).state !=
                        qsim_bit_get(ctx->signals[src_idx].value, b).state) {
                        pw_changed = 1; break;
                    }
                }
                if (pw_changed) {
                    uint32_t dst_idx = (uint32_t)ctx->port_wires[w].dst_sig_idx;
                    qsim_bit_vector_t *val = qsim_bit_vector_clone(ctx->signals[src_idx].value);
                    schedule_event(ctx, ctx->current_time, ctx->current_delta,
                                   dst_idx, val, 0, -1, -1);
                }
            }
            for (size_t i = 0; i < ctx->port_wire_src_count; i++)
                qsim_bit_vector_free(pw_saved[i]);
            free(pw_saved);
        }
    }

    /* Refresh: process body execution (Phase 2 above) may have
     * freed ctx->signals[sig_idx].value via exec_assign when the
     * process body assigns to the same signal that triggered this
     * call, making the cached new_val pointer dangling. */
    new_val = ctx->signals[sig_idx].value;

    /* Propagate through port connection wires (only if values differ) */
    for (size_t w = 0; w < ctx->port_wire_count; w++) {
        if ((uint32_t)ctx->port_wires[w].src_sig_idx == sig_idx) {
            uint32_t dst_idx = (uint32_t)ctx->port_wires[w].dst_sig_idx;
            int part_lo = ctx->port_wires[w].part_lo;
            int part_width = ctx->port_wires[w].part_width;
            uir_port_dir_t dir = ctx->port_wires[w].dir;
            qsim_bit_vector_t *src_val = ctx->signals[sig_idx].value;
            qsim_bit_vector_t *dst_val = ctx->signals[dst_idx].value;

            /* Determine if values differ, accounting for part-selects */
            int differs = 0;
            if (part_lo >= 0 && dir == UIR_PORT_IN) {
                /* Input with part-select: compare selected parent bits vs child */
                for (uint32_t b = 0; b < (uint32_t)part_width; b++) {
                    qsim_value_t sv = qsim_bit_get(src_val, (uint32_t)part_lo + b);
                    qsim_value_t dv = qsim_bit_get(dst_val, b);
                    if (sv.state != dv.state) { differs = 1; break; }
                }
            } else if (part_lo >= 0 && dir == UIR_PORT_OUT) {
                /* Output with part-select: compare child vs parent bits at offset */
                for (uint32_t b = 0; b < (uint32_t)part_width; b++) {
                    qsim_value_t sv = qsim_bit_get(src_val, b);
                    qsim_value_t dv = qsim_bit_get(dst_val, (uint32_t)part_lo + b);
                    if (sv.state != dv.state) { differs = 1; break; }
                }
            } else {
                /* No part-select: full signal comparison */
                uint32_t cmp_w = src_val->width < dst_val->width
                    ? src_val->width : dst_val->width;
                for (uint32_t b = 0; b < cmp_w; b++) {
                    if (qsim_bit_get(src_val, b).state != qsim_bit_get(dst_val, b).state) {
                        differs = 1; break;
                    }
                }
            }

            if (differs) {
                qsim_bit_vector_t *val;
                if (part_lo >= 0 && dir == UIR_PORT_IN) {
                    /* Input: extract selected bits from parent signal */
                    val = qsim_bit_vector_alloc((uint32_t)part_width);
                    for (uint32_t b = 0; b < (uint32_t)part_width; b++)
                        qsim_bit_set(val, b, qsim_bit_get(src_val, (uint32_t)part_lo + b));
                } else if (part_lo >= 0 && dir == UIR_PORT_OUT) {
                    /* Output: merge child value into parent at bit offset */
                    val = qsim_bit_vector_clone(dst_val);
                    for (uint32_t b = 0; b < (uint32_t)part_width; b++)
                        qsim_bit_set(val, (uint32_t)part_lo + b, qsim_bit_get(src_val, b));
                } else {
                    val = qsim_bit_vector_clone(src_val);
                }
                schedule_event(ctx, ctx->current_time, ctx->current_delta,
                               dst_idx, val, 0, -1, -1);
            }
        }
    }

    /* ── Phase 4: Path delay propagation ──
     * When a signal that is a path delay source changes, schedule a
     * transport delay event on the destination. */
    for (size_t p = 0; p < ctx->path_delay_count; p++) {
        if (ctx->path_delays[p].src_sig_idx != (int)sig_idx) continue;

        /* Check edge condition */
        if (ctx->path_delays[p].src_edge != 0) {
            qsim_value_t old_first = {QSIM_0, QSIM_STRENGTH_STRONG};
            qsim_value_t new_first = {QSIM_0, QSIM_STRENGTH_STRONG};
            if (old_val && old_val->width > 0) old_first = qsim_bit_get(old_val, 0);
            if (new_val && new_val->width > 0) new_first = qsim_bit_get(new_val, 0);
            if (!edge_match(ctx->path_delays[p].src_edge, old_first, new_first))
                continue;
        }

        /* Check condition (if any) — evaluate against current signal values */
        if (ctx->path_delays[p].condition) {
            qsim_bit_vector_t *cond_val = eval_expr(ctx, ctx->path_delays[p].condition);
            int cond_true = cond_val && cond_val->width > 0 &&
                            qsim_bit_get(cond_val, 0).state == QSIM_1;
            qsim_bit_vector_free(cond_val);
            if (!cond_true) continue;
        }

        /* Determine delay based on transition of the first bit (bit 0).
         * IEEE 1364-2005: rise on 0→1, fall on 1→0. Use z_delay on 0→Z
         * or 1→Z transitions (full paths), x_delay for →X. */
        uint64_t delay = ctx->path_delays[p].rise_delay;
        if (old_val && new_val && old_val->width > 0 && new_val->width > 0) {
            qsim_value_t old0 = qsim_bit_get(old_val, 0);
            qsim_value_t new0 = qsim_bit_get(new_val, 0);
            if (old0.state == QSIM_1 && new0.state == QSIM_0) {
                delay = ctx->path_delays[p].fall_delay;
            } else if (new0.state == QSIM_Z && ctx->path_delays[p].z_delay > 0) {
                delay = ctx->path_delays[p].z_delay;
            } else if (new0.state == QSIM_X && ctx->path_delays[p].x_delay > 0) {
                delay = ctx->path_delays[p].x_delay;
            } else if (old0.state != QSIM_1 && old0.state != QSIM_0 &&
                       new0.state != QSIM_1 && new0.state != QSIM_0) {
                /* Both X/Z → use min(rise, fall) */
                uint64_t r = ctx->path_delays[p].rise_delay;
                uint64_t f = ctx->path_delays[p].fall_delay;
                delay = (r < f) ? r : f;
            }
        }

        uint64_t target_time = ctx->current_time + delay;
        qsim_bit_vector_t *val = qsim_bit_vector_clone(ctx->signals[sig_idx].value);
        schedule_event(ctx, target_time, 0,
                       (uint32_t)ctx->path_delays[p].dst_sig_idx, val, 0, -1, -1);
    }

    /* ── Phase 5: Timing check monitoring ──
     * Implements $setup, $hold, $width, $period runtime checks.
     * $setup: on ref edge, verify data was stable for limit time before edge.
     * $hold: record ref edge time; on data change, verify time since ref edge >= limit.
     * $width: on each ref edge, measure pulse width since previous edge.
     * $period: on posedge of ref, measure period since previous posedge. */
    if (old_val && new_val) {
        int old_bit = (old_val->width > 0) ? old_val->bits[0].state : -1;
        int new_bit = (new_val->width > 0) ? new_val->bits[0].state : -1;
        int has_posedge = (old_bit == QSIM_0 && new_bit == QSIM_1);
        int has_negedge = (old_bit == QSIM_1 && new_bit == QSIM_0);
        int has_any_edge = has_posedge || has_negedge;

        if (has_any_edge) {
            for (size_t t = 0; t < ctx->timing_check_count; t++) {
                sim_timing_check_t *tc = &ctx->timing_checks[t];
                if (tc->ref_sig_idx != (int)sig_idx) continue;
                if (!ctx->display_cb) continue;

                char buf[256];
                switch (tc->kind) {
                case UIR_TIMING_SETUP:
                    if (tc->data_sig_idx >= 0) {
                        uint64_t data_last = ctx->signals[tc->data_sig_idx].last_change_time;
                        if (data_last > 0 && ctx->current_time > data_last &&
                            ctx->current_time - data_last < tc->limit) {
                            snprintf(buf, sizeof(buf),
                                "$setup violation: data changed %llu ns before clock (limit=%llu)",
                                (unsigned long long)(ctx->current_time - data_last),
                                (unsigned long long)tc->limit);
                            ctx->display_cb(ctx, buf, ctx->display_cb_user_data);
                        }
                    }
                    break;

                case UIR_TIMING_HOLD:
                    /* Record the ref edge time; check on subsequent data change */
                    tc->last_ref_time = ctx->current_time;
                    break;

                case UIR_TIMING_WIDTH:
                    /* Measure time between consecutive edges = pulse width */
                    if (tc->last_ref_time > 0) {
                        uint64_t pw = ctx->current_time - tc->last_ref_time;
                        if (pw < tc->limit) {
                            snprintf(buf, sizeof(buf),
                                "$width violation: pulse width %llu (limit=%llu)",
                                (unsigned long long)pw, (unsigned long long)tc->limit);
                            ctx->display_cb(ctx, buf, ctx->display_cb_user_data);
                        }
                    }
                    tc->last_ref_time = ctx->current_time;
                    break;

                case UIR_TIMING_PERIOD:
                    if (has_posedge) {
                        if (tc->last_ref_time > 0) {
                            uint64_t period = ctx->current_time - tc->last_ref_time;
                            if (period < tc->limit) {
                                snprintf(buf, sizeof(buf),
                                    "$period violation: period %llu (limit=%llu)",
                                    (unsigned long long)period, (unsigned long long)tc->limit);
                                ctx->display_cb(ctx, buf, ctx->display_cb_user_data);
                            }
                        }
                        tc->last_ref_time = ctx->current_time;
                    }
                    break;
                }
            }
        }
    }

    /* ── Phase 5b: $hold check on data signal change ── */
    if (old_val && new_val) {
        for (size_t t = 0; t < ctx->timing_check_count; t++) {
            sim_timing_check_t *tc = &ctx->timing_checks[t];
            if (tc->kind != UIR_TIMING_HOLD) continue;
            if (tc->data_sig_idx != (int)sig_idx) continue;
            if (tc->last_ref_time == 0) continue;
            if (!ctx->display_cb) continue;

            char buf[256];
            uint64_t hold_time = ctx->current_time - tc->last_ref_time;
            if (hold_time < tc->limit) {
                snprintf(buf, sizeof(buf),
                    "$hold violation: data changed %llu ns after clock (limit=%llu)",
                    (unsigned long long)hold_time, (unsigned long long)tc->limit);
                ctx->display_cb(ctx, buf, ctx->display_cb_user_data);
            }
            /* Reset to avoid repeated messages for the same ref edge */
            tc->last_ref_time = 0;
        }
    }
}

/* -- Recursive statement executors -- */

static void exec_assign(uir_sim_context_t *ctx, uir_assign_t *assign) {
    if (!assign) return;

    /* Set context width from LHS for proper width propagation */
    uint32_t saved_cw = ctx->current_context_width;
    if (assign->lhs && assign->lhs->kind == UIR_REF) {
        uir_ref_t *ref = (uir_ref_t *)assign->lhs;
        int lhs_idx = -1;
        if (ctx->current_prefix[0]) {
            char prefixed[520];
            snprintf(prefixed, sizeof(prefixed), "%s.%s",
                     ctx->current_prefix, ref->name);
            lhs_idx = find_signal_idx(ctx, prefixed);
        }
        if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
        if (lhs_idx >= 0)
            ctx->current_context_width = ctx->signals[lhs_idx].value->width;
    }
    qsim_bit_vector_t *rhs_val = eval_expr(ctx, assign->rhs);
    ctx->current_context_width = saved_cw;
    if (!rhs_val) return;

    if (assign->lhs && assign->lhs->kind == UIR_REF) {
        uir_ref_t *ref = (uir_ref_t *)assign->lhs;
        int lhs_idx = -1;
        if (ctx->current_prefix[0]) {
            char prefixed[520];
            snprintf(prefixed, sizeof(prefixed), "%s.%s",
                     ctx->current_prefix, ref->name);
            lhs_idx = find_signal_idx(ctx, prefixed);
        }
        if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);

        if (lhs_idx >= 0) {
            uint32_t target_w = ctx->signals[lhs_idx].value->width;
            qsim_bit_vector_t *final_val = rhs_val;
            if (rhs_val->width != target_w) {
                final_val = qsim_bit_vector_alloc(target_w);
                if (final_val) {
                    for (uint32_t i = 0; i < target_w; i++) {
                        if (i < rhs_val->width)
                            qsim_bit_set(final_val, i, qsim_bit_get(rhs_val, i));
                        else
                            qsim_bit_set(final_val, i, QSIM_VAL_0);
                    }
                }
            }
            /* Multi-dimensional indexed write: mem[i][j] */
            if (ref->multi_idx_count > 0) {
                uir_node_t *lhs_node = ctx->signals[lhs_idx].node;
                uint32_t elem_w = 1;
                uint32_t array_dims[4] = {0};
                size_t array_dim_count = 0;
                if (lhs_node->kind == UIR_SIGNAL) {
                    uir_signal_t *sig = (uir_signal_t *)lhs_node;
                    elem_w = sig->width;
                    array_dim_count = sig->array_dim_count;
                    for (size_t d = 0; d < array_dim_count && d < 4; d++)
                        array_dims[d] = sig->array_dims[d];
                }
                size_t nidxs = ref->multi_idx_count < array_dim_count ? ref->multi_idx_count : array_dim_count;
                uint32_t bit_off = 0;
                int idx_unknown = 0;
                for (size_t mi = 0; mi < nidxs; mi++) {
                    qsim_bit_vector_t *idx_val = eval_expr(ctx, ref->multi_index[mi]);
                    if (!idx_val) { idx_unknown = 1; break; }
                    uint32_t idx = 0;
                    for (uint32_t b = 0; b < idx_val->width && b < 32; b++) {
                        qsim_value_t bv = qsim_bit_get(idx_val, b);
                        if (bv.state == QSIM_1) idx |= (1u << b);
                        else if (bv.state != QSIM_0) { idx_unknown = 1; break; }
                    }
                    qsim_bit_vector_free(idx_val);
                    if (idx_unknown) break;
                    uint32_t stride = elem_w;
                    for (size_t k = mi + 1; k < array_dim_count; k++)
                        stride *= array_dims[k];
                    bit_off += idx * stride;
                }
                qsim_bit_vector_t *modified = qsim_bit_vector_clone(ctx->signals[lhs_idx].value);
                if (modified) {
                    if (idx_unknown) {
                        for (uint32_t i = 0; i < elem_w && i < modified->width; i++)
                            qsim_bit_set(modified, i, QSIM_VAL_X);
                    } else {
                        for (uint32_t i = 0; i < elem_w && i < (final_val ? final_val->width : 0); i++) {
                            if (bit_off + i < modified->width)
                                qsim_bit_set(modified, bit_off + i,
                                             qsim_bit_get(final_val ? final_val : rhs_val, i));
                        }
                    }
                    if (final_val && final_val != rhs_val)
                        qsim_bit_vector_free(final_val);
                    final_val = modified;
                }
            }

            /* Part-select bounds: used for both ref->part_hi (part-select)
             * and ref->index array writes below. */
            int ps_lo_val = -1, ps_hi_val = -1;

            /* Indexed write: array element access or bit-select */
            if (ref->index) {
                uir_node_t *lhs_node = ctx->signals[lhs_idx].node;
                int is_array = 0;
                uint32_t elem_w = 1;
                if (lhs_node->kind == UIR_SIGNAL) {
                    uir_signal_t *sig = (uir_signal_t *)lhs_node;
                    elem_w = sig->width;
                    is_array = (sig->array_size > 0);
                } else if (lhs_node->kind == UIR_PORT) {
                    elem_w = ((uir_port_t *)lhs_node)->width;
                }
                uint32_t write_w = is_array ? elem_w : 1;
                qsim_bit_vector_t *index_val = eval_expr(ctx, ref->index);
                if (index_val) {
                    uint32_t elem_idx = 0;
                    int index_known = 1;
                    for (uint32_t i = 0; i < index_val->width && i < 32; i++) {
                        qsim_value_t b = qsim_bit_get(index_val, i);
                        if (b.state == QSIM_1) elem_idx |= (1u << i);
                        else if (b.state != QSIM_0) { index_known = 0; break; }
                    }
                    qsim_bit_vector_free(index_val);
                    if (!index_known) elem_idx = UINT32_MAX;
                    if (is_array && index_known && assign->delay != 0) {
                        /* Array element NBA: use part-select event mechanism
                         * so the event carries the bit range.  This lets
                         * same-process writes to different array elements
                         * coexist (the NBA-replacement key includes ps_lo)
                         * and the event-apply code merges at the correct
                         * bit offset. */
                        uint32_t bit_off = elem_idx * elem_w;
                        ps_lo_val = (int)bit_off;
                        ps_hi_val = (int)(bit_off + elem_w - 1);
                        /* Keep final_val = rhs_val (element value), no merge;
                         * the part-select apply merges at event time. */
                    } else {
                        qsim_bit_vector_t *modified = qsim_bit_vector_clone(ctx->signals[lhs_idx].value);
                        if (modified) {
                            if (elem_idx == UINT32_MAX) {
                                /* X/Z index: write X to first element/bit */
                                for (uint32_t i = 0; i < write_w && i < modified->width; i++)
                                    qsim_bit_set(modified, i, QSIM_VAL_X);
                            } else {
                                uint32_t bit_off = is_array ? (elem_idx * elem_w) : elem_idx;
                                for (uint32_t i = 0; i < write_w && i < (final_val ? final_val->width : 0); i++) {
                                    if (bit_off + i < modified->width)
                                        qsim_bit_set(modified, bit_off + i,
                                                     qsim_bit_get(final_val ? final_val : rhs_val, i));
                                }
                            }
                            if (final_val && final_val != rhs_val)
                                qsim_bit_vector_free(final_val);
                            final_val = modified;
                        }
                    }
                }
            }

            /* Part-select write: for blocking assigns, apply immediately.
             * For NBA (delay>0), defer part-select to event processing time
             * so multiple NBAs to the same reg at the same delta merge correctly. */
            if (ref->part_hi && ref->part_lo) {
                if (assign->delay == 0) {
                    qsim_bit_vector_t *ps_modified = apply_part_select_write(ctx, ref, lhs_idx,
                        final_val ? final_val : rhs_val);
                    if (ps_modified) {
                        if (final_val && final_val != rhs_val)
                            qsim_bit_vector_free(final_val);
                        final_val = ps_modified;
                    }
                } else {
                    /* NBA: evaluate part-select bounds, apply at event time */
                    qsim_bit_vector_t *hi_val = eval_expr(ctx, ref->part_hi);
                    qsim_bit_vector_t *lo_val = eval_expr(ctx, ref->part_lo);
                    if (hi_val && lo_val) {
                        uint32_t hi = 0, lo = 0;
                        for (uint32_t i = 0; i < hi_val->width && i < 32; i++)
                            if (qsim_bit_get(hi_val, i).state == QSIM_1) hi |= (1u << i);
                        for (uint32_t i = 0; i < lo_val->width && i < 32; i++)
                            if (qsim_bit_get(lo_val, i).state == QSIM_1) lo |= (1u << i);
                        ps_lo_val = (int)(hi >= lo ? lo : hi);
                        ps_hi_val = (int)(hi >= lo ? hi : lo);
                    }
                    qsim_bit_vector_free(hi_val);
                    qsim_bit_vector_free(lo_val);
                }
            }

            if (assign->delay == 0) {
                /* Variable/blocking assignment: update immediately,
                 * not through the event queue, so subsequent statements
                 * in the same process see the new value. */
                qsim_bit_vector_t *old_val = ctx->signals[lhs_idx].value;
                ctx->signals[lhs_idx].value = qsim_bit_vector_clone(final_val ? final_val : rhs_val);
                /* Track last change time for timing check monitoring ($setup etc.) */
                if (old_val && !qsim_bit_vector_eq(old_val, ctx->signals[lhs_idx].value))
                    ctx->signals[lhs_idx].last_change_time = ctx->current_time;
                if (ctx->recording_mode) {
                    if (ctx->recorded_count >= ctx->recorded_cap) {
                        size_t nc = ctx->recorded_cap ? ctx->recorded_cap * 2 : 64;
                        signal_change_t *n = realloc(ctx->recorded_changes,
                                                     nc * sizeof(signal_change_t));
                        if (n) { ctx->recorded_changes = n; ctx->recorded_cap = nc; }
                    }
                    if (ctx->recorded_count < ctx->recorded_cap) {
                        ctx->recorded_changes[ctx->recorded_count].sig_idx = (int)lhs_idx;
                        ctx->recorded_changes[ctx->recorded_count].old_val = old_val;
                        ctx->recorded_count++;
                    } else {
                        qsim_bit_vector_free(old_val);
                    }
                } else {
                    qsim_bit_vector_free(old_val);
                }
                if (ctx->event_cb) {
                    ctx->event_cb(ctx, ctx->signals[lhs_idx].name,
                                  ctx->signals[lhs_idx].value,
                                  ctx->event_cb_user_data);
                }
            } else {
                schedule_event(ctx, ctx->current_time, ctx->current_delta + 1,
                               (uint32_t)lhs_idx,
                               qsim_bit_vector_clone(final_val ? final_val : rhs_val), 1,
                               ps_lo_val, ps_hi_val);
            }
            if (final_val && final_val != rhs_val)
                qsim_bit_vector_free(final_val);
        }
    }
    qsim_bit_vector_free(rhs_val);
}

static void exec_if(uir_sim_context_t *ctx, uir_if_t *if_node) {
    if (!if_node) return;
    qsim_bit_vector_t *cond_val = eval_expr(ctx, if_node->condition);
    if (!cond_val) return;
    int take_then = (cond_val->width > 0 &&
                     qsim_bit_get(cond_val, 0).state == QSIM_1);
    qsim_bit_vector_free(cond_val);
    if (take_then) {
        exec_stmt(ctx, if_node->then_branch);
    } else if (if_node->else_branch) {
        exec_stmt(ctx, if_node->else_branch);
    }
}

static void exec_case(uir_sim_context_t *ctx, uir_case_t *case_node) {
    if (!case_node) return;
    qsim_bit_vector_t *case_val = eval_expr(ctx, case_node->expr);
    if (!case_val) return;
    int matched = 0;
    for (size_t i = 0; i < case_node->item_count && !matched; i++) {
        uir_case_item_t *item = case_node->items[i];
        for (size_t j = 0; j < item->pattern_count && !matched; j++) {
            qsim_bit_vector_t *pat = eval_expr(ctx, item->patterns[j]);
            if (!pat) continue;
            if (case_node->is_wildcard) {
                int match = 1;
                uint32_t w = case_val->width < pat->width ? case_val->width : pat->width;
                for (uint32_t b = 0; b < w; b++) {
                    qsim_value_t pb = qsim_bit_get(pat, b);
                    if (pb.state == QSIM_Z) continue;
                    qsim_value_t cv = qsim_bit_get(case_val, b);
                    if (pb.state != cv.state) { match = 0; break; }
                }
                if (match) matched = 1;
            } else {
                int eq = 1;
                uint32_t w = case_val->width < pat->width ? case_val->width : pat->width;
                for (uint32_t b = 0; b < w; b++) {
                    qsim_value_t cv = qsim_bit_get(case_val, b);
                    qsim_value_t pv = qsim_bit_get(pat, b);
                    if (cv.state != pv.state) { eq = 0; break; }
                }
                matched = eq;
            }
            qsim_bit_vector_free(pat);
            if (matched) {
                exec_stmt(ctx, item->body);
            }
        }
    }
    if (!matched && case_node->default_item) {
        exec_stmt(ctx, case_node->default_item);
    }
    qsim_bit_vector_free(case_val);
}

/* ── System task format output engine ── */

/* Evaluate args and format according to fmt string. Returns malloc'd string. */
static char *sys_format_output(uir_sim_context_t *ctx, const char *fmt,
                                uir_node_t **args, size_t arg_count) {
    /* Overallocate: output <= fmt + 80 per arg */
    size_t cap = strlen(fmt) + arg_count * 80 + 256;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    size_t ai = 0;  /* arg index */

    for (const char *p = fmt; *p && pos < cap - 1; p++) {
        if (*p == '%') {
            p++;
            if (!*p) break;
            if (*p == '%') {
                out[pos++] = '%';
                continue;
            }
            if (ai >= arg_count) {
                pos += snprintf(out + pos, cap - pos, "%%%c", *p);
                continue;
            }
            qsim_bit_vector_t *bv = eval_expr(ctx, args[ai++]);
            if (!bv) { out[pos++] = '?'; continue; }

            uint64_t val = 0;
            int known = uir_bv_to_u64(bv, &val);
            int has_x = 0;
            if (!known) {
                for (uint32_t bi = 0; bi < bv->width; bi++)
                    if (bv->bits[bi].state == QSIM_X || bv->bits[bi].state == QSIM_Z)
                        { has_x = 1; break; }
            }

            switch (*p) {
                case 'd': case 'D':
                    if (has_x)
                        pos += snprintf(out + pos, cap - pos, "x");
                    else
                        pos += snprintf(out + pos, cap - pos, "%llu", (unsigned long long)val);
                    break;
                case 'h': case 'H':
                    if (has_x)
                        pos += snprintf(out + pos, cap - pos, "x");
                    else
                        pos += snprintf(out + pos, cap - pos, "%llx", (unsigned long long)val);
                    break;
                case 'b': case 'B': {
                    if (has_x) {
                        pos += snprintf(out + pos, cap - pos, "x");
                    } else {
                        for (uint32_t bi = bv->width; bi > 0; bi--) {
                            if (pos >= cap - 1) break;
                            out[pos++] = qsim_bit_get(bv, bi - 1).state == QSIM_1 ? '1' : '0';
                        }
                    }
                    break;
                }
                case 'o': case 'O':
                    if (has_x)
                        pos += snprintf(out + pos, cap - pos, "x");
                    else
                        pos += snprintf(out + pos, cap - pos, "%llo", (unsigned long long)val);
                    break;
                case 'c': case 'C': {
                    char c = ' ';
                    if (known) c = (char)(val & 0xFF);
                    out[pos++] = c;
                    break;
                }
                case 's': case 'S': {
                    /* Read bytes from bit vector (8-bit per char, LSB-first byte order) */
                    uint32_t nchars = bv->width / 8;
                    for (uint32_t ci = 0; ci < nchars && pos < cap - 1; ci++) {
                        char ch = 0;
                        for (int b = 0; b < 8; b++) {
                            uint32_t bit_idx = ci * 8 + (uint32_t)b;
                            if (bit_idx < bv->width && bv->bits[bit_idx].state == QSIM_1)
                                ch |= (1 << b);
                        }
                        out[pos++] = ch ? ch : ' ';
                    }
                    break;
                }
                default:
                    pos += snprintf(out + pos, cap - pos, "%%%c", *p);
                    break;
            }
            qsim_bit_vector_free(bv);
        } else {
            out[pos++] = *p;
        }
    }

    out[pos] = '\0';
    return out;
}

/* Evaluate $monitor args, compare with saved values, return 1 if any changed */
static int monitor_check_changed(uir_sim_context_t *ctx, monitor_entry_t *m) {
    if (!m || !m->last_vals) return 0;
    int changed = 0;
    for (size_t i = 0; i < m->arg_count; i++) {
        qsim_bit_vector_t *cur = eval_expr(ctx, m->args[i]);
        if (!cur) continue;
        if (m->last_vals[i]) {
            if (!qsim_bit_vector_eq(cur, m->last_vals[i])) changed = 1;
            qsim_bit_vector_free(m->last_vals[i]);
        } else {
            changed = 1;
        }
        m->last_vals[i] = cur;
    }
    if (changed && ctx->display_cb) {
        char *msg = sys_format_output(ctx, m->fmt, m->args, m->arg_count);
        if (msg) {
            ctx->display_cb(ctx, msg, ctx->display_cb_user_data);
            free(msg);
        }
    }
    return changed;
}

/* ── VHDL TEXTIO runtime ── */

/* Allocate a line buffer for a signal (create if NULL) */
static struct textio_line_st *textio_get_line(uir_sim_context_t *ctx, int sig_idx) {
    if (sig_idx < 0 || (size_t)sig_idx >= ctx->signal_count) return NULL;
    if (!ctx->line_buffers) {
        ctx->line_buffers = calloc(ctx->signal_count, sizeof(struct textio_line_st *));
        if (!ctx->line_buffers) return NULL;
    }
    if (!ctx->line_buffers[sig_idx]) {
        ctx->line_buffers[sig_idx] = calloc(1, sizeof(struct textio_line_st));
    }
    return ctx->line_buffers[sig_idx];
}

/* Get signal index from a TEXTIO procedure argument (handles UIR_REF) */
static int textio_arg_sig(uir_sim_context_t *ctx, uir_node_t *arg) {
    if (!arg || arg->kind != UIR_REF) return -1;
    uir_ref_t *r = (uir_ref_t *)arg;
    /* Try exact match first, then flexible match */
    int idx = find_signal_idx(ctx, r->name);
    if (idx < 0) {
        /* Search for suffix match (e.g. "L" matches "proc.L") */
        for (size_t i = 0; i < ctx->signal_count; i++) {
            const char *sn = ctx->signals[i].name;
            size_t snl = strlen(sn), rnl = strlen(r->name);
            if (snl > rnl + 1 && sn[snl - rnl - 1] == '.' &&
                strcmp(sn + snl - rnl, r->name) == 0)
                return (int)i;
        }
    }
    return idx;
}

/* Write a value to a TEXTIO arg signal (handles UIR_REF) */
static void textio_write_signal(uir_sim_context_t *ctx, uir_node_t *arg,
                                 qsim_bit_vector_t *val) {
    int idx = textio_arg_sig(ctx, arg);
    if (idx < 0) return;
    qsim_bit_vector_free(ctx->signals[idx].value);
    ctx->signals[idx].value = val;
}

/* TEXTIO: readline(file, line) — read one line from file into line buffer */
static void textio_readline(uir_sim_context_t *ctx, uir_func_call_t *tc) {
    if (tc->arg_count < 2) return;
    int file_idx = textio_arg_sig(ctx, tc->args[0]);
    int line_idx = textio_arg_sig(ctx, tc->args[1]);
    if (file_idx < 0 || line_idx < 0) return;
    if (!ctx->file_handles || !ctx->file_handles[file_idx] ||
        !ctx->file_handles[file_idx]->fp) return;
    FILE *fp = ctx->file_handles[file_idx]->fp;
    struct textio_line_st *l = textio_get_line(ctx, line_idx);
    if (!l) return;
    /* Read line into buffer (simple implementation: read until '\n') */
    size_t cap = 256;
    l->data = realloc(l->data, cap);
    l->len = 0;
    l->pos = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (l->len + 2 > cap) { cap *= 2; l->data = realloc(l->data, cap); }
        l->data[l->len++] = (char)c;
    }
    l->data[l->len] = '\0';
    if (c == EOF && l->len == 0) { l->data[0] = '\0'; } /* empty string at EOF */
}

/* TEXTIO: writeline(file, line) — write line buffer to file, reset line */
static void textio_writeline(uir_sim_context_t *ctx, uir_func_call_t *tc) {
    if (tc->arg_count < 2) return;
    int file_idx = textio_arg_sig(ctx, tc->args[0]);
    int line_idx = textio_arg_sig(ctx, tc->args[1]);
    if (file_idx < 0 || line_idx < 0) return;
    if (!ctx->file_handles || !ctx->file_handles[file_idx] ||
        !ctx->file_handles[file_idx]->fp) return;
    FILE *fp = ctx->file_handles[file_idx]->fp;
    struct textio_line_st *l = textio_get_line(ctx, line_idx);
    if (!l) return;
    if (l->data) {
        fputs(l->data, fp);
        fputc('\n', fp);
    }
    fflush(fp);
    /* Reset line buffer */
    l->len = 0;
    l->pos = 0;
    if (l->data) l->data[0] = '\0';
}

/* Convert a std_logic character ('0','1','Z','X','W','L','H','-','U') to qsim_value_t */
static qsim_value_t char_to_std_logic(char c) {
    switch (c) {
        case '0': return QSIM_VAL_0;
        case '1': return QSIM_VAL_1;
        case 'Z': case 'z': return QSIM_VAL_Z;
        case 'X': case 'x': return QSIM_VAL_X;
        case 'W': case 'w': return (qsim_value_t){QSIM_W, QSIM_STRENGTH_WEAK};
        case 'L': case 'l': return (qsim_value_t){QSIM_0, QSIM_STRENGTH_WEAK};
        case 'H': case 'h': return (qsim_value_t){QSIM_1, QSIM_STRENGTH_WEAK};
        case '-': return QSIM_VAL_DC;
        case 'U': case 'u': return QSIM_VAL_U;
        default:  return QSIM_VAL_X;
    }
}

static char std_logic_to_char(qsim_value_t v) {
    if (v.strength == QSIM_STRENGTH_WEAK) {
        if (v.state == QSIM_0) return 'L';
        if (v.state == QSIM_1) return 'H';
        if (v.state == QSIM_W) return 'W';
    }
    switch (v.state) {
        case QSIM_0: return '0';
        case QSIM_1: return '1';
        case QSIM_Z: return 'Z';
        case QSIM_X: return 'X';
        case QSIM_W: return 'W';
        case QSIM_DC:return '-';
        case QSIM_U: return 'U';
        default: return 'X';
    }
}

/* TEXTIO: read(line, value [, good]) — parse std_logic_vector from line buffer */
static void textio_read_slv(uir_sim_context_t *ctx, uir_func_call_t *tc) {
    if (tc->arg_count < 2) return;
    int line_idx = textio_arg_sig(ctx, tc->args[0]);
    if (line_idx < 0) return;
    struct textio_line_st *l = textio_get_line(ctx, line_idx);
    if (!l) return;
    /* Skip leading whitespace */
    while (l->pos < l->len && (l->data[l->pos] == ' ' || l->data[l->pos] == '\t'))
        l->pos++;
    /* Get the target arg: must be UIR_REF for out value */
    uir_node_t *val_arg = tc->args[1];
    int val_idx = textio_arg_sig(ctx, val_arg);
    if (val_idx < 0) return;
    uint32_t target_w = ctx->signals[val_idx].value->width;
    qsim_bit_vector_t *val = qsim_bit_vector_alloc(target_w);
    int good = 1;
    for (uint32_t i = 0; i < target_w; i++) {
        if (l->pos + i < l->len) {
            qsim_value_t sv = char_to_std_logic(l->data[l->pos + i]);
            qsim_bit_set(val, target_w - 1 - i, sv); /* MSB-first in text */
        } else {
            qsim_bit_set(val, target_w - 1 - i, QSIM_VAL_X);
            good = 0;
        }
    }
    /* Advance line position */
    l->pos += target_w;
    /* Write value to target signal */
    textio_write_signal(ctx, val_arg, val);
    /* If 3rd arg (good) present, write status */
    if (tc->arg_count >= 3) {
        qsim_bit_vector_t *gv = qsim_bit_vector_alloc(1);
        qsim_bit_set(gv, 0, good ? QSIM_VAL_1 : QSIM_VAL_0);
        textio_write_signal(ctx, tc->args[2], gv);
    }
}

/* TEXTIO: write(line, value [, side, width]) — append std_logic_vector to line */
static void textio_write_slv(uir_sim_context_t *ctx, uir_func_call_t *tc) {
    if (tc->arg_count < 2) return;
    int line_idx = textio_arg_sig(ctx, tc->args[0]);
    if (line_idx < 0) return;
    struct textio_line_st *l = textio_get_line(ctx, line_idx);
    if (!l) return;
    /* Evaluate the value arg */
    qsim_bit_vector_t *val = eval_expr(ctx, tc->args[1]);
    if (!val) return;
    /* Determine width */
    uint32_t w = val->width;
    /* Append to line buffer */
    for (uint32_t i = 0; i < w; i++) {
        char c = std_logic_to_char(qsim_bit_get(val, w - 1 - i)); /* MSB-first */
        if (l->len + 2 > l->cap) {
            l->cap = l->cap ? l->cap * 2 : 64;
            l->data = realloc(l->data, l->cap);
        }
        l->data[l->len++] = c;
    }
    qsim_bit_vector_free(val);
    if (l->data) l->data[l->len] = '\0';
}

/* TEXTIO: endfile(file) — returns 1 if EOF, 0 if not */
static int textio_endfile(uir_sim_context_t *ctx, uir_func_call_t *tc) {
    if (tc->arg_count < 1) return 1;
    int file_idx = textio_arg_sig(ctx, tc->args[0]);
    if (file_idx < 0) return 1;
    if (!ctx->file_handles || !ctx->file_handles[file_idx] ||
        !ctx->file_handles[file_idx]->fp) return 1;
    return feof(ctx->file_handles[file_idx]->fp) ? 1 : 0;
}

/* Initialize TEXTIO files from design unit file_metas.
 * Called during ctx_init. */
static void textio_init_files(uir_sim_context_t *ctx, uir_design_unit_t **units, size_t count) {
    /* Count total file declarations first to avoid allocating if none */
    size_t total_file_decls = 0;
    for (size_t ui = 0; ui < count; ui++)
        if (units[ui]) total_file_decls += units[ui]->file_meta_count;
    if (total_file_decls == 0) return;
    if (!ctx->file_handles) {
        ctx->file_handles = calloc(ctx->signal_count, sizeof(struct textio_file_st *));
        if (!ctx->file_handles) return;
    }
    for (size_t ui = 0; ui < count; ui++) {
        if (!units[ui]) continue;
        for (size_t fi = 0; fi < units[ui]->file_meta_count; fi++) {
            uir_file_meta_t *fm = &units[ui]->file_metas[fi];
            int sig_idx = find_signal_idx(ctx, fm->name);
            if (sig_idx < 0) continue;
            if (!ctx->file_handles[sig_idx]) {
                ctx->file_handles[sig_idx] = calloc(1, sizeof(struct textio_file_st));
            }
            struct textio_file_st *fh = ctx->file_handles[sig_idx];
            if (!fm->file_name) continue; /* no path given — open later via file_open */
            const char *mode_str = "r";
            if (fm->mode == 1) mode_str = "w";
            else if (fm->mode == 2) mode_str = "a";
            fh->fp = fopen(fm->file_name, mode_str);
            if (fh->fp) {
                fh->is_open = 1;
                fh->mode = fm->mode;
                fh->filename = strdup(fm->file_name);
            }
        }
    }
}

/* Close all open TEXTIO files */
static void textio_close_files(uir_sim_context_t *ctx) {
    if (!ctx->file_handles) return;
    for (size_t i = 0; i < ctx->signal_count; i++) {
        if (!ctx->file_handles[i]) continue;
        if (ctx->file_handles[i]->fp)
            fclose(ctx->file_handles[i]->fp);
        free(ctx->file_handles[i]->filename);
        free(ctx->file_handles[i]);
    }
    free(ctx->file_handles);
    ctx->file_handles = NULL;
}

/* Free all TEXTIO line buffers */
static void textio_free_lines(uir_sim_context_t *ctx) {
    if (!ctx->line_buffers) return;
    for (size_t i = 0; i < ctx->signal_count; i++) {
        if (ctx->line_buffers[i]) {
            free(ctx->line_buffers[i]->data);
            free(ctx->line_buffers[i]);
        }
    }
    free(ctx->line_buffers);
    ctx->line_buffers = NULL;
}

/* ── Recursive statement execution ── */

static void exec_stmt(uir_sim_context_t *ctx, uir_node_t *stmt) {
    if (!stmt) return;

    /* Trace process body structure */
    /* Record line coverage */
    if (stmt->loc.file && stmt->loc.line > 0) {
        int found = 0;
        for (size_t i = 0; i < ctx->coverage_count; i++) {
            if (ctx->coverage[i].line == stmt->loc.line &&
                strcmp(ctx->coverage[i].file, stmt->loc.file) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (ctx->coverage_count >= ctx->coverage_cap) {
                size_t nc = ctx->coverage_cap ? ctx->coverage_cap * 2 : 256;
                coverage_entry_t *n = realloc(ctx->coverage, nc * sizeof(coverage_entry_t));
                if (n) { ctx->coverage = n; ctx->coverage_cap = nc; }
            }
            if (ctx->coverage_count < ctx->coverage_cap) {
                coverage_entry_t *e = &ctx->coverage[ctx->coverage_count++];
                e->file = strdup(stmt->loc.file);
                e->line = stmt->loc.line;
            }
        }
    }

    /* Step callback (for debugger / breakpoints) */
    if (ctx->step_cb) {
        ctx->step_cb(ctx, stmt->loc.file, stmt->loc.line, ctx->step_cb_user_data);
    }

    /* Check breakpoints */
    if (stmt->loc.file && stmt->loc.line > 0 && ctx->breakpoint_count > 0) {
        for (size_t i = 0; i < ctx->breakpoint_count; i++) {
            if (ctx->breakpoints[i].line == stmt->loc.line &&
                strcmp(ctx->breakpoints[i].file, stmt->loc.file) == 0) {
                ctx->breakpoint_hit = 1;
                break;
            }
        }
    }

    switch (stmt->kind) {
        case UIR_BLOCK: {
            uir_block_t *block = (uir_block_t *)stmt;
            /* Track named block hierarchy for disable event/waiter cancellation */
            char saved_block_hier[sizeof(ctx->current_block_hier)] = "";
            if (block->name && block->name[0]) {
                memcpy(saved_block_hier, ctx->current_block_hier, sizeof(ctx->current_block_hier));
                size_t cur_len = strlen(ctx->current_block_hier);
                snprintf(ctx->current_block_hier + cur_len,
                         sizeof(ctx->current_block_hier) - cur_len,
                         ".%s", block->name);
            }
            for (size_t i = 0; i < block->stmt_count; i++) {
                exec_stmt(ctx, block->stmts[i]);
                if (ctx->disable_block_name[0]) {
                    int stop_block = 1;
                    if (block->name && strcmp(block->name, ctx->disable_block_name) == 0) {
                        /* Scope check: empty scope matches all; non-empty requires prefix */
                        int scope_ok = 1;
                        if (ctx->disable_scope[0]) {
                            size_t slen = strlen(ctx->disable_scope);
                            scope_ok = (strncmp(ctx->current_prefix, ctx->disable_scope, slen) == 0 &&
                                       (ctx->current_prefix[slen] == '\0' ||
                                        ctx->current_prefix[slen] == '.'));
                        }
                        if (scope_ok) {
                            /* Cancel pending events/waiters for this (sub-)tree */
                            char target_hier[sizeof(ctx->current_block_hier)];
                            if (ctx->disable_scope[0])
                                snprintf(target_hier, sizeof(target_hier), "%s.%s",
                                         ctx->disable_scope, ctx->disable_block_name);
                            else if (ctx->current_prefix[0])
                                snprintf(target_hier, sizeof(target_hier), "%s.%s",
                                         ctx->current_prefix, ctx->disable_block_name);
                            else
                                snprintf(target_hier, sizeof(target_hier), "%s",
                                         ctx->disable_block_name);
                            size_t tlen = strlen(target_hier);
                            for (sim_event_t *ev = ctx->event_head; ev; ev = ev->next) {
                                if (ev->block_hier[0] == '\0') continue;
                                if (strncmp(ev->block_hier, target_hier, tlen) == 0 &&
                                    (ev->block_hier[tlen] == '\0' ||
                                     ev->block_hier[tlen] == '.'))
                                    ev->cancelled = 1;
                            }
                            for (size_t wi = 0; wi < ctx->waiter_count; wi++) {
                                if (ctx->waiters[wi].block_hier[0] == '\0') continue;
                                if (strncmp(ctx->waiters[wi].block_hier, target_hier, tlen) == 0 &&
                                    (ctx->waiters[wi].block_hier[tlen] == '\0' ||
                                     ctx->waiters[wi].block_hier[tlen] == '.'))
                                    ctx->waiters[wi].cancelled = 1;
                            }
                            ctx->disable_block_name[0] = '\0';
                            ctx->disable_scope[0] = '\0';
                            if (block->name && block->name[0])
                                memcpy(ctx->current_block_hier, saved_block_hier,
                                       sizeof(ctx->current_block_hier));
                        } else {
                            stop_block = 0; /* same name, wrong scope — keep executing */
                        }
                    }
                    if (stop_block)
                        return;
                }
            }
            if (block->name && block->name[0])
                memcpy(ctx->current_block_hier, saved_block_hier,
                       sizeof(ctx->current_block_hier));
            break;
        }
        case UIR_ASSIGN:
            exec_assign(ctx, (uir_assign_t *)stmt);
            break;
        case UIR_IF:
            exec_if(ctx, (uir_if_t *)stmt);
            break;
        case UIR_CASE:
            exec_case(ctx, (uir_case_t *)stmt);
            break;
        case UIR_SYS_TASK: {
            uir_sys_task_t *t = (uir_sys_task_t *)stmt;
            if (t->task_kind == UIR_SYS_READMEMH) {
                int sig_idx = find_signal_idx(ctx, t->mem_name);
                if (sig_idx < 0) break;
                uir_signal_t *sig = (uir_signal_t *)ctx->signals[sig_idx].node;
                if (sig->array_size == 0) break;
                uint32_t elem_w = sig->width;
                FILE *f = fopen(t->filename, "r");
                if (!f) break;
                uint32_t elem_idx = 0;
                char line[256];
                while (fgets(line, sizeof(line), f) && elem_idx < sig->array_size) {
                    char *p = line;
                    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                    if (!*p || *p == '/' || *p == '#') continue;
                    unsigned long val = strtoul(p, NULL, 16);
                    uint32_t bit_off = elem_idx * elem_w;
                    for (uint32_t b = 0; b < elem_w && b < 32; b++) {
                        qsim_bit_set(ctx->signals[sig_idx].value, bit_off + b,
                                     (val >> b) & 1 ? QSIM_VAL_1 : QSIM_VAL_0);
                    }
                    elem_idx++;
                }
                fclose(f);
            } else if (t->task_kind == UIR_SYS_DISPLAY || t->task_kind == UIR_SYS_WRITE) {
                if (!ctx->display_cb) break;
                char *msg = sys_format_output(ctx, t->fmt, t->args, t->arg_count);
                if (!msg) break;
                char *with_newline = NULL;
                if (t->task_kind == UIR_SYS_DISPLAY) {
                    size_t mlen = strlen(msg);
                    with_newline = malloc(mlen + 2);
                    if (with_newline) {
                        memcpy(with_newline, msg, mlen);
                        with_newline[mlen] = '\n';
                        with_newline[mlen + 1] = '\0';
                    }
                }
                ctx->display_cb(ctx, with_newline ? with_newline : msg, ctx->display_cb_user_data);
                free(msg);
                free(with_newline);
            } else if (t->task_kind == UIR_SYS_MONITOR) {
                /* Save or replace monitor entry */
                if (!ctx->monitor) {
                    ctx->monitor = calloc(1, sizeof(monitor_entry_t));
                }
                if (ctx->monitor) {
                    /* Free old state */
                    free(ctx->monitor->fmt);
                    ctx->monitor->fmt = strdup(t->fmt);
                    ctx->monitor->args = t->args;
                    ctx->monitor->arg_count = t->arg_count;
                    if (ctx->monitor->last_vals) {
                        for (size_t i = 0; i < ctx->monitor->arg_count; i++)
                            qsim_bit_vector_free(ctx->monitor->last_vals[i]);
                        free(ctx->monitor->last_vals);
                    }
                    ctx->monitor->last_vals = calloc(t->arg_count, sizeof(qsim_bit_vector_t *));
                    /* First evaluation and output */
                    monitor_check_changed(ctx, ctx->monitor);
                }
            } else if (t->task_kind == UIR_SYS_STOP) {
                ctx->stop_requested = 1;
            } else if (t->task_kind == UIR_SYS_FINISH) {
                ctx->finish_requested = 1;
            } else if (t->task_kind == UIR_SYS_FATAL ||
                       t->task_kind == UIR_SYS_ERROR ||
                       t->task_kind == UIR_SYS_WARNING ||
                       t->task_kind == UIR_SYS_INFO) {
                if (ctx->display_cb) {
                    const char *sev = "";
                    if (t->task_kind == UIR_SYS_FATAL) sev = "Fatal: ";
                    else if (t->task_kind == UIR_SYS_ERROR) sev = "Error: ";
                    else if (t->task_kind == UIR_SYS_WARNING) sev = "Warning: ";
                    else if (t->task_kind == UIR_SYS_INFO) sev = "Info: ";
                    char *formatted = sys_format_output(ctx, t->fmt, t->args, t->arg_count);
                    if (formatted) {
                        size_t slen = strlen(sev) + strlen(formatted) + 2;
                        char *msg = malloc(slen);
                        if (msg) {
                            snprintf(msg, slen, "%s%s\n", sev, formatted);
                            ctx->display_cb(ctx, msg, ctx->display_cb_user_data);
                            free(msg);
                        }
                        free(formatted);
                    } else if (t->fmt) {
                        ctx->display_cb(ctx, t->fmt, ctx->display_cb_user_data);
                    }
                }
                if (t->task_kind == UIR_SYS_FATAL)
                    ctx->finish_requested = 1;
            } else if (t->task_kind == UIR_SYS_DUMPFILE) {
                /* Stub: store dump filename for future VCD integration */
            } else if (t->task_kind == UIR_SYS_DUMPVARS || t->task_kind == UIR_SYS_DUMPON) {
                /* Stub: enable dumping */
            } else if (t->task_kind == UIR_SYS_DUMPOFF) {
                /* Stub: disable dumping */
            } else if (t->task_kind == UIR_SYS_VALUE_PLUSARGS) {
                /* Stub: write 0 to target variable (not-found) */
                if (t->mem_name) {
                    int sig_idx = find_signal_idx(ctx, t->mem_name);
                    if (sig_idx >= 0) {
                        uint32_t w = ctx->signals[sig_idx].node->kind == UIR_SIGNAL
                            ? ((uir_signal_t *)ctx->signals[sig_idx].node)->width : 32;
                        qsim_bit_vector_t *zero = qsim_bit_vector_from_state(w, QSIM_0);
                        qsim_bit_vector_free(ctx->signals[sig_idx].value);
                        ctx->signals[sig_idx].value = qsim_bit_vector_clone(zero);
                        schedule_event(ctx, ctx->current_time, ctx->current_delta,
                                       (uint32_t)sig_idx, zero, 0, -1, -1);
                    }
                }
            } else if (t->task_kind == UIR_SYS_TEST_PLUSARGS) {
                /* Stub: return 0 (not-found) — no-op */
            } else if (t->task_kind == UIR_SYS_FWRITE || t->task_kind == UIR_SYS_FDISPLAY) {
                /* Stub: file output not yet implemented */
            } else if (t->task_kind == UIR_SYS_FCLOSE) {
                /* Stub: no-op */
            } else if (t->task_kind == UIR_SYS_TIMEFORMAT) {
                /* Stub: store timeformat unit/precision/suffix for $realtime display */
            } else if (t->task_kind == UIR_SYS_PRINTTIMESCALE) {
                const char *msg = "Timescale of (top): 1ps / 1ps\n";
                if (ctx->display_cb)
                    ctx->display_cb(ctx, msg, ctx->display_cb_user_data);
                else
                    fprintf(stderr, "%s", msg);
            } else if (t->task_kind == UIR_SDF_ANNOTATE) {
                /* Annotation already applied at elaboration time in
                 * apply_sdf_annotations() — no-op at runtime. */
            }
            break;
        }
        case UIR_TASK_ENABLE: {
            uir_func_call_t *tc = (uir_func_call_t *)stmt;
            /* Check for VITAL builtin procedure */
            { int vk = match_vital_builtin(tc->name); if (vk) { vital_eval_task(ctx, vk, tc); break; } }
            /* Check for TEXTIO builtin procedures */
            if (strcmp(tc->name, "readline") == 0) {
                textio_readline(ctx, tc); break;
            }
            if (strcmp(tc->name, "read") == 0) {
                textio_read_slv(ctx, tc); break;
            }
            if (strcmp(tc->name, "write") == 0) {
                textio_write_slv(ctx, tc); break;
            }
            if (strcmp(tc->name, "writeline") == 0) {
                textio_writeline(ctx, tc); break;
            }
            /* Check if this task is disabled */
            if (ctx->disable_block_name[0]) {
                if (strcmp(tc->name, ctx->disable_block_name) == 0) {
                    ctx->disable_block_name[0] = '\0';
                    ctx->disable_scope[0] = '\0';
                    break; /* Skip execution of disabled task */
                }
            }
            func_frame_t *frame = NULL;
            for (size_t i = 0; i < ctx->func_frame_count; i++) {
                if (strcmp(ctx->func_frames[i].def->name, tc->name) == 0) {
                    frame = &ctx->func_frames[i];
                    break;
                }
            }
            if (!frame || !frame->def) break;
            uir_func_t *ft = frame->def;

            char saved_prefix[256] = "";
            strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);

            /* Save automatic frame state and re-initialize to X */
            qsim_bit_vector_t **auto_saved = NULL;
            if (frame->is_automatic && frame->auto_sig_count > 0) {
                auto_saved = calloc((size_t)frame->auto_sig_count, sizeof(qsim_bit_vector_t *));
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count)
                        auto_saved[sai] = qsim_bit_vector_clone(ctx->signals[sidx].value);
                }
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count) {
                        uint32_t w = ctx->signals[sidx].value->width;
                        qsim_bit_vector_free(ctx->signals[sidx].value);
                        ctx->signals[sidx].value = qsim_bit_vector_from_state(w, QSIM_X);
                    }
                }
            }

            /* Evaluate input/inout args and write to port signals */
            size_t arg_count = tc->arg_count < ft->port_count ? tc->arg_count : ft->port_count;
            for (size_t i = 0; i < arg_count; i++) {
                uir_func_port_t *port = &ft->ports[i];
                if (port->direction == UIR_PORT_IN || port->direction == UIR_PORT_INOUT) {
                    qsim_bit_vector_t *val = eval_expr(ctx, tc->args[i]);
                    if (val) {
                        int port_idx = frame->port_sig_indices[i];
                        if (port_idx >= 0) {
                            qsim_bit_vector_free(ctx->signals[port_idx].value);
                            ctx->signals[port_idx].value = val;
                        } else {
                            qsim_bit_vector_free(val);
                        }
                    }
                }
            }

            /* Set task prefix and execute body */
            strncpy(ctx->current_prefix, frame->prefix, sizeof(ctx->current_prefix) - 1);
            exec_stmt(ctx, ft->body);

            /* Restore automatic frame state */
            if (auto_saved) {
                for (int sai = 0; sai < frame->auto_sig_count; sai++) {
                    int sidx = frame->auto_first_sig_idx + sai;
                    if (sidx >= 0 && (size_t)sidx < ctx->signal_count && auto_saved[sai]) {
                        qsim_bit_vector_free(ctx->signals[sidx].value);
                        ctx->signals[sidx].value = auto_saved[sai];
                    }
                }
                free(auto_saved);
            }

            /* Restore prefix */
            strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);

            /* Write back output/inout args */
            for (size_t i = 0; i < arg_count; i++) {
                uir_func_port_t *port = &ft->ports[i];
                if (port->direction == UIR_PORT_OUT || port->direction == UIR_PORT_INOUT) {
                    int port_idx = frame->port_sig_indices[i];
                    if (port_idx < 0) continue;
                    if (tc->args[i] && tc->args[i]->kind == UIR_REF) {
                        uir_ref_t *arg_ref = (uir_ref_t *)tc->args[i];
                        int sig_idx = find_signal_idx(ctx, arg_ref->name);
                        if (sig_idx >= 0) {
                            schedule_event(ctx, ctx->current_time, ctx->current_delta,
                                            (uint32_t)sig_idx,
                                            qsim_bit_vector_clone(ctx->signals[port_idx].value),
                                            0, -1, -1);
                        }
                    }
                }
            }
            break;
        }
        case UIR_DELAY: {
            uir_delay_t *d = (uir_delay_t *)stmt;
            qsim_bit_vector_t *val = eval_expr(ctx, d->delay_value);
            uint64_t n = 0;
            if (val) {
                for (uint32_t i = 0; i < val->width && i < 64; i++) {
                    if (qsim_bit_get(val, i).state == QSIM_1)
                        n |= (1ULL << i);
                }
                qsim_bit_vector_free(val);
            }
            /* For always-without-sensitivity, re-execute the delay after each
             * body completion to create self-looping timing behavior. */
            schedule_stmt_event(ctx, ctx->current_time + n, d->body,
                                d->always_loop, d->always_loop ? (uir_node_t *)d : NULL,
                                ctx->current_block_hier);
            break;
        }
        case UIR_LOOP: {
            uir_loop_t *loop = (uir_loop_t *)stmt;
            if (loop->init_stmt) exec_stmt(ctx, loop->init_stmt);
            if (loop->body) exec_stmt(ctx, loop->body);
            break;
        }
        case UIR_LOOP_BACK: {
            uir_loop_back_t *lb = (uir_loop_back_t *)stmt;
            qsim_bit_vector_t *cv = lb->condition ? eval_expr(ctx, lb->condition) : NULL;
            int cond_true = 0;
            if (cv) {
                for (uint32_t i = 0; i < cv->width; i++)
                    if (qsim_bit_get(cv, i).state == QSIM_1) { cond_true = 1; break; }
                qsim_bit_vector_free(cv);
            } else {
                /* NULL condition means infinite loop (forever) */
                cond_true = 1;
            }
            if (cond_true)
                schedule_stmt_event(ctx, ctx->current_time, lb->body, 0, NULL,
                                    ctx->current_block_hier);
            break;
        }
        case UIR_WAIT: {
            uir_wait_t *w = (uir_wait_t *)stmt;
            qsim_bit_vector_t *val = eval_expr(ctx, w->condition);
            int cond_true = 0;
            if (val) {
                for (uint32_t b = 0; b < val->width; b++)
                    if (qsim_bit_get(val, b).state == QSIM_1) { cond_true = 1; break; }
                qsim_bit_vector_free(val);
            }
            if (cond_true) {
                /* Condition already true — execute body immediately */
                exec_stmt(ctx, w->body);
            } else {
                /* Register waiter — will check on signal changes */
                add_waiter(ctx, stmt, ctx->current_block_hier);
            }
            break;
        }
        case UIR_EVENT_CTRL: {
            /* Register waiter — will execute body when the signal event occurs */
            add_waiter(ctx, stmt, ctx->current_block_hier);
            break;
        }
        case UIR_DISABLE: {
            uir_disable_t *d = (uir_disable_t *)stmt;
            /* Parse target: "scope.block_name" or just "block_name" */
            char *last_dot = strrchr(d->target_name, '.');
            if (last_dot) {
                /* Hierarchical: split into scope + block_name */
                size_t scope_len = (size_t)(last_dot - d->target_name);
                memcpy(ctx->disable_scope, d->target_name, scope_len);
                ctx->disable_scope[scope_len] = '\0';
                strncpy(ctx->disable_block_name, last_dot + 1,
                        sizeof(ctx->disable_block_name) - 1);
            } else {
                ctx->disable_scope[0] = '\0';
                strncpy(ctx->disable_block_name, d->target_name,
                        sizeof(ctx->disable_block_name) - 1);
            }
            ctx->disable_block_name[sizeof(ctx->disable_block_name) - 1] = '\0';
            /* Cancel pending events for the disabled block */
            char target_hier[sizeof(ctx->current_block_hier)];
            if (ctx->disable_scope[0])
                snprintf(target_hier, sizeof(target_hier), "%s.%s",
                         ctx->disable_scope, ctx->disable_block_name);
            else if (ctx->current_prefix[0])
                snprintf(target_hier, sizeof(target_hier), "%s.%s",
                         ctx->current_prefix, ctx->disable_block_name);
            else
                snprintf(target_hier, sizeof(target_hier), "%s",
                         ctx->disable_block_name);
            size_t tlen = strlen(target_hier);
            for (sim_event_t *ev = ctx->event_head; ev; ev = ev->next) {
                if (ev->block_hier[0] == '\0') continue;
                if (strncmp(ev->block_hier, target_hier, tlen) == 0 &&
                    (ev->block_hier[tlen] == '\0' || ev->block_hier[tlen] == '.'))
                    ev->cancelled = 1;
            }
            for (size_t wi = 0; wi < ctx->waiter_count; wi++) {
                if (ctx->waiters[wi].block_hier[0] == '\0') continue;
                if (strncmp(ctx->waiters[wi].block_hier, target_hier, tlen) == 0 &&
                    (ctx->waiters[wi].block_hier[tlen] == '\0' ||
                     ctx->waiters[wi].block_hier[tlen] == '.'))
                    ctx->waiters[wi].cancelled = 1;
            }
            break;
        }
        case UIR_FORCE: {
            uir_force_t *f = (uir_force_t *)stmt;
            if (f->lhs && f->lhs->kind == UIR_REF) {
                uir_ref_t *lref = (uir_ref_t *)f->lhs;
                qsim_bit_vector_t *val = eval_expr(ctx, f->rhs);
                if (val) {
                    uir_sim_force_signal(ctx, lref->name, val);
                    qsim_bit_vector_free(val);
                }
            }
            break;
        }
        case UIR_RELEASE: {
            uir_release_t *r = (uir_release_t *)stmt;
            if (r->target && r->target->kind == UIR_REF) {
                uir_ref_t *tref = (uir_ref_t *)r->target;
                uir_sim_release_signal(ctx, tref->name);
            }
            break;
        }
        case UIR_EVENT_TRIGGER: {
            uir_event_trigger_t *et = (uir_event_trigger_t *)stmt;
            int sig_idx = find_signal_idx(ctx, et->name);
            if (sig_idx >= 0) {
                qsim_bit_vector_t *old = qsim_bit_vector_clone(ctx->signals[sig_idx].value);
                qsim_bit_vector_free(ctx->signals[sig_idx].value);
                ctx->signals[sig_idx].value = qsim_bit_vector_from_state(1, QSIM_1);
                check_and_trigger(ctx, (uint32_t)sig_idx, old, ctx->signals[sig_idx].value);
                check_waiters_on_change(ctx, (uint32_t)sig_idx, old, ctx->signals[sig_idx].value);
                qsim_bit_vector_free(old);
                qsim_bit_vector_free(ctx->signals[sig_idx].value);
                ctx->signals[sig_idx].value = qsim_bit_vector_from_state(1, QSIM_0);
            }
            break;
        }
        case UIR_VHDL_ASSERT: {
            uir_vhdl_assert_t *va = (uir_vhdl_assert_t *)stmt;
            /* VHDL assert: evaluate condition, fire if all bits are 0 */
            qsim_bit_vector_t *cv = eval_expr(ctx, va->condition);
            int fire = 1;
            if (cv) {
                for (uint32_t _b = 0; _b < cv->width; _b++)
                    if (qsim_bit_get(cv, _b).state == QSIM_1) { fire = 0; break; }
                qsim_bit_vector_free(cv);
            }
            if (fire && ctx->display_cb) {
                const char *sev_strs[] = {"Note", "Warning", "Error", "Failure"};
                const char *sev = va->severity >= 0 && va->severity < 4 ? sev_strs[va->severity] : "?";
                char msg_buf[1024];
                char *msg_val = NULL;
                if (va->message) {
                    qsim_bit_vector_t *mv = eval_expr(ctx, va->message);
                    if (mv) { msg_val = qsim_bit_vector_to_str(mv); qsim_bit_vector_free(mv); }
                }
                snprintf(msg_buf, sizeof(msg_buf), "%%s: %%s", sev, msg_val ? msg_val : "Assertion violation");
                ctx->display_cb(ctx, msg_buf, ctx->display_cb_user_data);
                free(msg_val);
                if (va->severity >= 3) ctx->finish_requested = 1;
            }
            break;
        }

        case UIR_EXIT:   /* VHDL exit — stub, no-op */
        case UIR_NEXT:   /* VHDL next — stub, no-op */
        case UIR_RETURN: /* VHDL return — stub, no-op */
            break;
    }
}

/* -- API -- */
uir_sim_context_t *uir_sim_create(uir_design_unit_t **units, size_t count) {
    if (!units || count == 0) return NULL;

    uir_sim_context_t *ctx = calloc(1, sizeof(uir_sim_context_t));
    if (!ctx) return NULL;

    ctx->units = units;
    ctx->unit_count = count;
    ctx->thread_count = 1;  /* default: single-threaded */
    sim_mutex_init(&ctx->pool_mutex);
    ctx->rand_state = 1;    /* non-zero seed for $random */
    ctx->current_process_id = -1;
    ctx->max_deltas_per_time = 10000;  /* safety limit for combinatorial loop detection */

    /* Determine which units are instantiated by other units (non-top-level).
     * These will be skipped in the direct processing loops below since their
     * signals/processes/CAs are already handled via recursive instance descent. */
    int *instantiated = calloc(count, sizeof(int));
    if (instantiated) {
        for (size_t i = 0; i < count; i++) {
            if (!units[i]) continue;
            for (size_t j = 0; j < units[i]->instance_count; j++) {
                uir_instance_t *inst = units[i]->instances[j];
                if (!inst->bound_to) continue;
                for (size_t k = 0; k < count; k++) {
                    if (units[k] == inst->bound_to) {
                        instantiated[k] = 1;
                        break;
                    }
                }
            }
        }
    }
#define IS_TOPLEVEL(i) (!instantiated || !instantiated[i])

    /* Build signal table */
    for (size_t i = 0; i < count; i++) {
        if (units[i] && IS_TOPLEVEL(i))
            collect_signals(ctx, units[i], "", 0);
    }

    /* Initialize hash table for O(1) signal name→index lookup */
    signal_ht_init(ctx);

    /* Resolve VHDL alias declarations: map alias name → target signal index */
    for (size_t ui = 0; ui < count; ui++) {
        if (!units[ui] || !IS_TOPLEVEL(ui)) continue;
        for (size_t ai = 0; ai < units[ui]->vhdl_alias_count; ai++) {
            uir_vhdl_alias_t *alias = &units[ui]->vhdl_aliases[ai];
            int target_idx = find_signal_idx(ctx, alias->target);
            if (target_idx < 0) continue;
            if (ctx->alias_count >= ctx->alias_cap) {
                size_t nc = ctx->alias_cap ? ctx->alias_cap * 2 : 16;
                char **nn = realloc(ctx->alias_names, nc * sizeof(char *));
                int *nt = realloc(ctx->alias_targets, nc * sizeof(int));
                if (!nn || !nt) break;
                ctx->alias_names = nn;
                ctx->alias_targets = nt;
                ctx->alias_cap = nc;
            }
            ctx->alias_names[ctx->alias_count] = strdup(alias->name);
            ctx->alias_targets[ctx->alias_count] = target_idx;
            ctx->alias_count++;
        }
    }

    /* Collect processes — only from top-level units;
     * add_processes recurses into instances so submodule processes
     * are reached with correct hierarchical prefix. */
    for (size_t i = 0; i < count; i++) {
        if (units[i] && IS_TOPLEVEL(i)) {
            add_processes(ctx, units[i], "");
        }
    }

    /* Resolve auto-sensitivity for @(*) processes */
    for (size_t p = 0; p < ctx->process_count; p++) {
        uir_process_t *proc = ctx->processes[p];
        if (!proc->auto_sens) continue;

        /* Set prefix for scoped signal lookup inside submodule processes */
        char saved_prefix[520] = "";
        if (ctx->process_prefixes[p][0]) {
            strncpy(saved_prefix, ctx->current_prefix, sizeof(saved_prefix) - 1);
            saved_prefix[sizeof(saved_prefix) - 1] = '\0';
            strncpy(ctx->current_prefix, ctx->process_prefixes[p], sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }

        int *ref_sigs = NULL;
        size_t ref_count = 0, ref_cap = 0;
        find_refs_in_node(proc->body, ctx, &ref_sigs, &ref_count, &ref_cap);

        if (ctx->process_prefixes[p][0]) {
            strncpy(ctx->current_prefix, saved_prefix, sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }

        if (ref_count > 0) {
            proc->sensitivity_list = calloc(ref_count, sizeof(uir_sensitivity_t));
            if (proc->sensitivity_list) {
                proc->sensitivity_count = ref_count;
                for (size_t r = 0; r < ref_count; r++) {
                    proc->sensitivity_list[r].signal = ctx->signals[ref_sigs[r]].node;
                    proc->sensitivity_list[r].edge = 0; /* level-sensitive */
                }
            }
        }
        free(ref_sigs);
        proc->auto_sens = 0; /* resolved */
    }

    /* Collect continuous assigns */
    for (size_t i = 0; i < count; i++) {
        if (units[i] && IS_TOPLEVEL(i))
            add_cont_assigns(ctx, units[i], "");
    }

    /* Collect UDP instances */
    for (size_t i = 0; i < count; i++) {
        if (units[i] && IS_TOPLEVEL(i))
            add_udp_instances(ctx, units[i], "");
    }

    /* Wire up port connections from instances */
    for (size_t i = 0; i < count; i++) {
        if (units[i] && IS_TOPLEVEL(i))
            add_port_wires(ctx, units[i], "");
    }

#undef IS_TOPLEVEL
    free(instantiated);

    /* Build unique port-wire source signal index list for efficient
     * Phase 2b snapshot (only snapshot these instead of all signals). */
    if (ctx->port_wire_count > 0 && ctx->signal_count > 0) {
        unsigned char *seen = calloc(ctx->signal_count, 1);
        if (seen) {
            ctx->port_wire_srcs = malloc(ctx->port_wire_count * sizeof(int));
            if (ctx->port_wire_srcs) {
                size_t idx = 0;
                for (size_t w = 0; w < ctx->port_wire_count; w++) {
                    int src = ctx->port_wires[w].src_sig_idx;
                    if (src >= 0 && (size_t)src < ctx->signal_count && !seen[src]) {
                        seen[src] = 1;
                        ctx->port_wire_srcs[idx++] = src;
                    }
                }
                ctx->port_wire_src_count = idx;
            }
            free(seen);
        }
    }

    /* Flush pending literal port initializations (now all port wires exist) */
    for (size_t i = 0; i < ctx->pending_lit_count; i++) {
        int sig = ctx->pending_lit_sigs[i];
        if (sig >= 0 && ctx->signals[sig].value) {
            qsim_bit_vector_t *old = qsim_bit_vector_clone(ctx->signals[sig].value);
            qsim_bit_vector_free(ctx->signals[sig].value);
            ctx->signals[sig].value = qsim_bit_vector_clone(ctx->pending_lit_vals[i]);
            check_and_trigger(ctx, (uint32_t)sig, old, ctx->signals[sig].value);
            qsim_bit_vector_free(old);
        }
        qsim_bit_vector_free(ctx->pending_lit_vals[i]);
    }
    free(ctx->pending_lit_sigs);
    free(ctx->pending_lit_vals);
    ctx->pending_lit_sigs = NULL;
    ctx->pending_lit_vals = NULL;
    ctx->pending_lit_count = 0;
    ctx->pending_lit_cap = 0;

    /* Register function/task local signals with prefixed names */
    {
        size_t total_ft = 0;
        for (size_t i = 0; i < count; i++)
            if (units[i]) total_ft += units[i]->func_task_count;

        if (total_ft > 0) {
            ctx->func_frames = calloc(total_ft, sizeof(func_frame_t));
            size_t frame_idx = 0;

            for (size_t i = 0; i < count; i++) {
                uir_design_unit_t *unit = units[i];
                if (!unit) continue;

                for (size_t f = 0; f < unit->func_task_count; f++) {
                    uir_func_t *ft = unit->func_tasks[f];
                    if (!ft) continue;

                    func_frame_t *frame = &ctx->func_frames[frame_idx++];
                    frame->def = ft;
                    frame->is_automatic = ft->is_automatic;
                    frame->auto_first_sig_idx = (int)ctx->signal_count;

                    /* Build prefix: __func__<unit>__<name> */
                    snprintf(frame->prefix, sizeof(frame->prefix),
                             "__func__%s__%s",
                             unit->name ? unit->name : "anon",
                             ft->name);

                    uir_loc_t loc = {NULL, 0, 0};

                    /* Register ports as signal entries */
                    if (ft->port_count > 0) {
                        frame->port_sig_indices = calloc(ft->port_count, sizeof(int));
                        for (size_t p = 0; p < ft->port_count; p++) {
                            uir_signal_t *psig = (uir_signal_t *)uir_alloc_node(
                                unit, UIR_SIGNAL, sizeof(uir_signal_t), loc);
                            if (!psig) { frame->port_sig_indices[p] = -1; continue; }
                            psig->name = strdup(ft->ports[p].name);
                            psig->sig_type = UIR_SIG_REG;
                            psig->width = ft->ports[p].width;
                            psig->array_size = 0;
                            psig->init_value.state = QSIM_X;
                            psig->init_value.strength = QSIM_STRENGTH_STRONG;

                            char full_name[520];
                            snprintf(full_name, sizeof(full_name), "%s.%s",
                                     frame->prefix, ft->ports[p].name);
                            if (add_signal(ctx, (uir_node_t *)psig, full_name))
                                frame->port_sig_indices[p] = (int)(ctx->signal_count - 1);
                            else
                                frame->port_sig_indices[p] = -1;
                        }
                    }

                    /* Register return register signal (same name as function, in locals) */
                    frame->return_sig_idx = -1;
                    if (ft->is_function) {
                        for (size_t l = 0; l < ft->local_count; l++) {
                            if (!ft->locals[l] || ft->locals[l]->kind != UIR_SIGNAL)
                                continue;
                            uir_signal_t *lsig = (uir_signal_t *)ft->locals[l];
                            if (strcmp(lsig->name, ft->name) == 0) {
                                char full_name[520];
                                snprintf(full_name, sizeof(full_name), "%s.%s",
                                         frame->prefix, lsig->name);
                                if (add_signal(ctx, ft->locals[l], full_name))
                                    frame->return_sig_idx = (int)(ctx->signal_count - 1);
                                break;
                            }
                        }
                    }

                    /* Register remaining locals (skip return reg) */
                    for (size_t l = 0; l < ft->local_count; l++) {
                        if (!ft->locals[l] || ft->locals[l]->kind != UIR_SIGNAL)
                            continue;
                        uir_signal_t *lsig = (uir_signal_t *)ft->locals[l];
                        /* Skip return register (already registered above) */
                        if (ft->is_function && strcmp(lsig->name, ft->name) == 0)
                            continue;
                        char full_name[520];
                        snprintf(full_name, sizeof(full_name), "%s.%s",
                                 frame->prefix, lsig->name);
                        add_signal(ctx, ft->locals[l], full_name);
                    }
                    frame->auto_sig_count = (int)(ctx->signal_count - frame->auto_first_sig_idx);
                }
            }
            ctx->func_frame_count = frame_idx;
        }
    }

    /* Elaborate path delays from specify blocks */
    for (size_t i = 0; i < count; i++) {
        if (units[i])
            add_path_delays(ctx, units[i], "");
    }

    apply_sdf_annotations(ctx);

    /* Register timing checks from specify blocks */
    for (size_t i = 0; i < count; i++) {
        if (units[i])
            add_timing_checks(ctx, units[i], "");
    }

    /* Build signal partitions for thread-parallel delta evaluation */
    build_signal_partitions(ctx);

    /* Initialize TEXTIO file handles from file declarations */
    textio_init_files(ctx, units, count);

    return ctx;
}

int uir_sim_set_thread_count(uir_sim_context_t *ctx, int thread_count) {
    if (!ctx) return -1;
    if (thread_count < 1) thread_count = 1;
    ctx->thread_count = thread_count;
    return 0;
}

void uir_sim_destroy(uir_sim_context_t *ctx) {
    if (!ctx) return;

    /* Free event values before destroying the event pool */
    sim_event_t *ev = ctx->event_head;
    while (ev) {
        qsim_bit_vector_free(ev->value);
        ev = ev->next;
    }
    pool_destroy(ctx);

    /* Free signal values and resolution driver entries */
    for (size_t i = 0; i < ctx->signal_count; i++) {
        qsim_bit_vector_free(ctx->signals[i].value);
        if (ctx->signals[i].prev_value)
            qsim_bit_vector_free(ctx->signals[i].prev_value);
        for (int _di = 0; _di < ctx->signals[i].driver_count; _di++)
            qsim_bit_vector_free(ctx->signals[i].drivers[_di].value);
    }
    free(ctx->signals);
    free(ctx->processes);
    if (ctx->process_prefixes) free(ctx->process_prefixes);
    for (size_t i = 0; i < ctx->cont_assign_count; i++)
        free(ctx->cont_assigns[i].dep_sigs);
    free(ctx->cont_assigns);
    free(ctx->port_wires);
    free(ctx->port_wire_srcs);
    free(ctx->recorded_changes);
    for (size_t i = 0; i < ctx->breakpoint_count; i++)
        free(ctx->breakpoints[i].file);
    free(ctx->breakpoints);
    for (size_t i = 0; i < ctx->coverage_count; i++)
        free(ctx->coverage[i].file);
    free(ctx->coverage);
    for (size_t i = 0; i < ctx->func_frame_count; i++)
        free(ctx->func_frames[i].port_sig_indices);
    free(ctx->func_frames);
    /* Free monitor entry */
    if (ctx->monitor) {
        free(ctx->monitor->fmt);
        if (ctx->monitor->last_vals) {
            for (size_t i = 0; i < ctx->monitor->arg_count; i++)
                qsim_bit_vector_free(ctx->monitor->last_vals[i]);
            free(ctx->monitor->last_vals);
        }
        free(ctx->monitor);
    }

    /* Free waiters */
    for (size_t i = 0; i < ctx->waiter_count; i++)
        free(ctx->waiters[i].sig_indices);
    free(ctx->waiters);
    /* Free VHDL alias mappings */
    for (size_t i = 0; i < ctx->alias_count; i++)
        free(ctx->alias_names[i]);
    free(ctx->alias_names);
    free(ctx->alias_targets);

    /* Free path delays */
    free(ctx->path_delays);

    /* Free timing checks */
    free(ctx->timing_checks);

    /* Free UDP instances */
    for (size_t ui = 0; ui < ctx->udp_instance_count; ui++)
        free(ctx->udp_instances[ui].input_sig_indices);
    free(ctx->udp_instances);

    /* Free signal hash table */
    free(ctx->sig_ht);

    /* ── Thread pool cleanup ── */
    if (ctx->thread_count > 1 && ctx->workers && ctx->workers[0]) {
        /* Signal worker threads to exit */
        sim_atomic_store(&ctx->parallel_phase, -1);
        sim_barrier_wait(&ctx->phase_barrier);
        for (int i = 1; i < ctx->thread_count; i++) {
            if (ctx->workers[i-1])
                sim_thread_join(ctx->workers[i-1]);
        }
    }
    /* Free per-thread state */
    if (ctx->threads) {
        for (int i = 0; i < ctx->thread_count; i++) {
            sim_thread_state_t *ts = &ctx->threads[i];
            free(ts->changes);
            free(ts->work_batch);
            free(ts->proc_indices);
            free(ts->ca_indices);
            /* Per-thread free list events belong to event pool blocks
             * already freed by pool_destroy above — just clear the list. */
            ts->local_free_list = NULL;
        }
        free(ctx->threads);
    }
    free(ctx->workers);
    sim_barrier_destroy(&ctx->phase_barrier);
    sim_mutex_destroy(&ctx->pool_mutex);

    /* Free partition arrays */
    free(ctx->signal_partition);
    free(ctx->process_partition);
    free(ctx->ca_partition);

    /* Free CA change tracking arrays */
    if (ctx->ca_changed_old_vals) {
        for (int i = 0; i < ctx->ca_changed_count; i++)
            qsim_bit_vector_free(ctx->ca_changed_old_vals[i]);
        free(ctx->ca_changed_old_vals);
    }
    free(ctx->ca_changed_sigs);

    /* Close TEXTIO files and free line buffers */
    textio_close_files(ctx);
    textio_free_lines(ctx);

    free(ctx);
}

int uir_sim_set_signal(uir_sim_context_t *ctx, const char *hier_path,
                        qsim_bit_vector_t *value) {
    if (!ctx || !hier_path || !value) return 0;
    int idx = find_signal_idx(ctx, hier_path);
    if (idx < 0) return 0;

    /* Before initial_eval runs (first uir_sim_run call), set the value
     * directly so it is visible during process body execution.  Without
     * this, processes read stale X values during initialization, producing
     * results that can conflict with triggered re-executions (e.g. a VITAL
     * function that maps X -> 0 and 1 -> 1 would produce both 0 and 1 for
     * the same signal at the same delta, resolving to X). */
    if (!ctx->initial_eval_done) {
        qsim_bit_vector_t *old = ctx->signals[idx].value;
        ctx->signals[idx].value = qsim_bit_vector_clone(value);
        /* Propagate through port connections and continuous assigns;
         * processes will run during initial_eval. */
        check_and_trigger_ca_only(ctx, (uint32_t)idx, old, ctx->signals[idx].value);
        qsim_bit_vector_free(old);
    }

    /* Schedule event so the main loop detects the change and triggers
     * sensitive processes. */
    schedule_event(ctx, ctx->current_time, ctx->current_delta,
                    (uint32_t)idx, qsim_bit_vector_clone(value), 0, -1, -1);
    return 1;
}

qsim_bit_vector_t *uir_sim_get_signal(uir_sim_context_t *ctx, const char *hier_path) {
    if (!ctx || !hier_path) return NULL;
    int idx = find_signal_idx(ctx, hier_path);
    if (idx < 0) return NULL;
    return ctx->signals[idx].value;
}

uint64_t uir_sim_current_time(uir_sim_context_t *ctx) {
    return ctx ? ctx->current_time : 0;
}

size_t uir_sim_get_event_count(uir_sim_context_t *ctx) {
    return ctx ? ctx->total_events : 0;
}

void uir_sim_set_event_callback(uir_sim_context_t *ctx, uir_event_callback_t cb, void *user_data) {
    if (!ctx) return;
    ctx->event_cb = cb;
    ctx->event_cb_user_data = user_data;
}

void uir_sim_set_sys_display_callback(uir_sim_context_t *ctx, uir_sys_display_cb_t cb, void *user_data) {
    if (!ctx) return;
    ctx->display_cb = cb;
    ctx->display_cb_user_data = user_data;
}

int uir_sim_get_signal_count(uir_sim_context_t *ctx) {
    return ctx ? (int)ctx->signal_count : 0;
}

const char *uir_sim_get_signal_name(uir_sim_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || (size_t)idx >= ctx->signal_count) return NULL;
    return ctx->signals[idx].name;
}

const qsim_bit_vector_t *uir_sim_get_signal_value(uir_sim_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || (size_t)idx >= ctx->signal_count) return NULL;
    return ctx->signals[idx].value;
}

uir_node_t *uir_sim_get_signal_node(uir_sim_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || (size_t)idx >= ctx->signal_count) return NULL;
    return ctx->signals[idx].node;
}

int uir_sim_force_signal(uir_sim_context_t *ctx, const char *hier_path,
                          const qsim_bit_vector_t *value) {
    if (!ctx || !hier_path || !value) return 0;
    int idx = find_signal_idx(ctx, hier_path);
    if (idx < 0) return 0;

    /* Preserve signal width — the forced value may come from a string
     * that doesn't specify width correctly (e.g. "8'h01" parsed as 5 bits).
     * Pad with 0 or truncate to match the declared signal width. */
    uint32_t sig_width = ctx->signals[idx].value->width;
    qsim_bit_vector_t *adjusted = NULL;
    if (value->width != sig_width) {
        adjusted = qsim_bit_vector_alloc(sig_width);
        if (!adjusted) return 0;
        for (uint32_t i = 0; i < sig_width; i++) {
            if (i < value->width)
                adjusted->bits[i] = value->bits[i];
            else
                adjusted->bits[i] = QSIM_VAL_0;
        }
        value = adjusted;
    }

    qsim_bit_vector_t *old_val = ctx->signals[idx].value;
    ctx->signals[idx].value = qsim_bit_vector_clone(value);
    qsim_bit_vector_free(adjusted);

    /* Fire event callback if registered */
    if (ctx->event_cb) {
        ctx->event_cb(ctx, ctx->signals[idx].name,
                      ctx->signals[idx].value,
                      ctx->event_cb_user_data);
    }

    /* Trigger propagation through port wires and continuous assigns */
    check_and_trigger(ctx, (uint32_t)idx, old_val, ctx->signals[idx].value);
    qsim_bit_vector_free(old_val);
    return 1;
}

int uir_sim_release_signal(uir_sim_context_t *ctx, const char *hier_path) {
    if (!ctx || !hier_path) return 0;
    int idx = find_signal_idx(ctx, hier_path);
    if (idx < 0) return 0;

    uint32_t w = ctx->signals[idx].value->width;
    qsim_bit_vector_t *old_val = ctx->signals[idx].value;
    ctx->signals[idx].value = qsim_bit_vector_from_state(w, QSIM_X);

    /* Fire event callback if registered */
    if (ctx->event_cb) {
        ctx->event_cb(ctx, ctx->signals[idx].name,
                      ctx->signals[idx].value,
                      ctx->event_cb_user_data);
    }

    /* Trigger propagation (release to X may affect dependent signals) */
    check_and_trigger(ctx, (uint32_t)idx, old_val, ctx->signals[idx].value);
    qsim_bit_vector_free(old_val);
    return 1;
}

/* ── Memory array loader ── */

int uir_sim_load_mem(uir_sim_context_t *ctx, const char *hier_path,
                     uint32_t addr, const uint32_t *data, size_t count) {
    if (!ctx || !hier_path || !data || count == 0) return 0;
    int idx = find_signal_idx(ctx, hier_path);
    if (idx < 0) return 0;

    uir_signal_t *sig = (uir_signal_t *)ctx->signals[idx].node;
    if (sig->base.kind != UIR_SIGNAL) return 0;
    uint32_t elem_width = sig->width;
    if (elem_width == 0) return 0;

    qsim_bit_vector_t *full = ctx->signals[idx].value;
    if (!full) return 0;

    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t bit_off = (addr + (uint32_t)i) * elem_width;
        if (bit_off + elem_width > full->width) break;
        for (uint32_t b = 0; b < elem_width && b < 32; b++) {
            qsim_bit_set(full, bit_off + b,
                         (data[i] & (1u << b)) ? QSIM_VAL_1 : QSIM_VAL_0);
        }
        written++;
    }

    if (written > 0 && ctx->event_cb) {
        ctx->event_cb(ctx, ctx->signals[idx].name,
                      ctx->signals[idx].value,
                      ctx->event_cb_user_data);
    }
    return (int)written;
}

/* ── Step callback ── */

void uir_sim_set_step_callback(uir_sim_context_t *ctx, qsim_step_callback_t cb, void *user_data) {
    if (!ctx) return;
    ctx->step_cb = cb;
    ctx->step_cb_user_data = user_data;
}

/* ── Breakpoints ── */

void uir_sim_set_breakpoint(uir_sim_context_t *ctx, const char *file, uint32_t line, int set) {
    if (!ctx || !file) return;

    if (set) {
        /* Add breakpoint if not already present */
        for (size_t i = 0; i < ctx->breakpoint_count; i++) {
            if (ctx->breakpoints[i].line == line &&
                strcmp(ctx->breakpoints[i].file, file) == 0)
                return; /* already exists */
        }
        if (ctx->breakpoint_count >= ctx->breakpoint_cap) {
            size_t nc = ctx->breakpoint_cap ? ctx->breakpoint_cap * 2 : 16;
            breakpoint_t *n = realloc(ctx->breakpoints, nc * sizeof(breakpoint_t));
            if (!n) return;
            ctx->breakpoints = n;
            ctx->breakpoint_cap = nc;
        }
        ctx->breakpoints[ctx->breakpoint_count].file = strdup(file);
        ctx->breakpoints[ctx->breakpoint_count].line = line;
        ctx->breakpoint_count++;
    } else {
        /* Remove breakpoint */
        for (size_t i = 0; i < ctx->breakpoint_count; i++) {
            if (ctx->breakpoints[i].line == line &&
                strcmp(ctx->breakpoints[i].file, file) == 0) {
                free(ctx->breakpoints[i].file);
                ctx->breakpoints[i] = ctx->breakpoints[ctx->breakpoint_count - 1];
                ctx->breakpoint_count--;
                return;
            }
        }
    }
}

int uir_sim_breakpoint_hit(uir_sim_context_t *ctx) {
    return ctx ? ctx->breakpoint_hit : 0;
}

void uir_sim_clear_breakpoint_hit(uir_sim_context_t *ctx) {
    if (ctx) ctx->breakpoint_hit = 0;
}

/* ── Line coverage ── */

size_t uir_sim_get_coverage_count(uir_sim_context_t *ctx) {
    return ctx ? ctx->coverage_count : 0;
}

int uir_sim_get_coverage_entry(uir_sim_context_t *ctx, size_t idx,
                                const char **file, uint32_t *line) {
    if (!ctx || idx >= ctx->coverage_count || !file || !line)
        return 0;
    *file = ctx->coverage[idx].file;
    *line = ctx->coverage[idx].line;
    return 1;
}

void uir_sim_reset_coverage(uir_sim_context_t *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->coverage_count; i++)
        free(ctx->coverage[i].file);
    ctx->coverage_count = 0;
}

/* Initial evaluation: trigger level-sensitive processes at time 0 */
/* Forward decl for ensure_initialized (defined after initial_eval) */
static void initial_eval(uir_sim_context_t *ctx);

/* Run all initial processes and CSA eval if not yet done.
 * Call before load_mem/force to prevent initial blocks from
 * overwriting user-set signal values. */
void uir_sim_ensure_initialized(uir_sim_context_t *ctx) {
    initial_eval(ctx);
}

static void initial_eval(uir_sim_context_t *ctx) {
    volatile uint64_t canary = 0xDEADBEEFCAFEBABEULL;
    if (!ctx->initial_eval_done) {
        ctx->initial_eval_done = 1;

        /* Save initial signal values so we can detect changes after
         * process body execution and trigger cross-process sensitivity. */
        qsim_bit_vector_t **saved = NULL;
        if (ctx->signal_count > 0) {
            saved = malloc(ctx->signal_count * sizeof(qsim_bit_vector_t *));
            if (saved) {
                for (size_t i = 0; i < ctx->signal_count; i++)
                    saved[i] = qsim_bit_vector_clone(ctx->signals[i].value);
            }
        }

        for (size_t p = 0; p < ctx->process_count; p++) {
            uir_process_t *proc = ctx->processes[p];
            if (proc->body) {
                strncpy(ctx->current_prefix, ctx->process_prefixes[p], 255);
                ctx->current_process_id = (int)p;
                exec_stmt(ctx, proc->body);
                ctx->current_process_id = -1;
                ctx->current_prefix[0] = '\0';
            }
        }

        /* Detect signal changes from initial state and trigger sensitive
         * processes. This ensures blocking-assign side effects from one
         * process are visible to @(signal)-sensitive processes even though
         * blocking assigns update directly (no event queue). */
        if (saved) {
            for (size_t i = 0; i < ctx->signal_count; i++) {
                if (!saved[i]) continue;
                if (!ctx->signals[i].value) continue;
                int changed = 0;
                uint32_t w = saved[i]->width < ctx->signals[i].value->width
                    ? saved[i]->width : ctx->signals[i].value->width;
                for (uint32_t b = 0; b < w; b++) {
                    if (qsim_bit_get(saved[i], b).state !=
                        qsim_bit_get(ctx->signals[i].value, b).state) {
                        changed = 1;
                        break;
                    }
                }
                if (changed) {
                    check_and_trigger(ctx, (uint32_t)i, saved[i],
                                      ctx->signals[i].value);
                }
                qsim_bit_vector_free(saved[i]);
            }
            free(saved);
        }
    }

    /* Evaluate continuous assigns — scheduled events will propagate */
    for (size_t i = 0; i < ctx->cont_assign_count; i++) {
        cont_assign_entry_t *entry = &ctx->cont_assigns[i];
        uir_assign_t *a = entry->assign;

        /* Set context width from LHS for proper width propagation */
        uint32_t saved_cw = ctx->current_context_width;
        char saved_pfx[520] = "";
        if (entry->prefix[0]) {
            strncpy(saved_pfx, ctx->current_prefix, sizeof(saved_pfx) - 1);
            saved_pfx[sizeof(saved_pfx) - 1] = '\0';
            strncpy(ctx->current_prefix, entry->prefix, sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }
        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lhs_idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lhs_idx = find_signal_idx(ctx, prefixed);
            }
            if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
            if (lhs_idx >= 0) {
                ctx->current_context_width = ctx->signals[lhs_idx].value->width;
            }
        }
        qsim_bit_vector_t *rhs_val = eval_expr(ctx, a->rhs);
        ctx->current_context_width = saved_cw;
        if (!rhs_val) {
            if (entry->prefix[0])
                strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
            continue;
        }
        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lidx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lidx = find_signal_idx(ctx, prefixed);
            }
            if (lidx < 0) lidx = find_signal_idx(ctx, ref->name);
            if (lidx >= 0) {
                uint32_t target_w = ctx->signals[lidx].value->width;
                qsim_bit_vector_t *final_val = rhs_val;
                if (rhs_val->width != target_w) {
                    final_val = qsim_bit_vector_alloc(target_w);
                    if (final_val) {
                        for (uint32_t b = 0; b < target_w; b++) {
                            if (b < rhs_val->width)
                                qsim_bit_set(final_val, b, qsim_bit_get(rhs_val, b));
                            else
                                qsim_bit_set(final_val, b, QSIM_VAL_0);
                        }
                    }
                }
                /* Handle array-indexed LHS: write only the targeted element */
                qsim_bit_vector_t *arr_result = apply_array_write(ctx, ref, lidx, final_val ? final_val : rhs_val);
                qsim_bit_vector_t *write_val;
                if (arr_result) {
                    write_val = arr_result;
                    if (final_val && final_val != rhs_val) qsim_bit_vector_free(final_val);
                } else {
                    write_val = final_val ? final_val : rhs_val;
                }
                /* Handle part-select LHS: write only the selected bits */
                qsim_bit_vector_t *ps_result = apply_part_select_write(ctx, ref, lidx, write_val);
                if (ps_result) {
                    if (write_val != (final_val ? final_val : rhs_val))
                        qsim_bit_vector_free(write_val);
                    write_val = ps_result;
                }
                /* Schedule update with appropriate delay strategy */
                if (a->delay > 0) {
                    /* VHDL: immediate write so processes see updated CSA output */
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lidx, write_val, &old_ca, -1);
                    if (ca_changed && ctx->event_cb) {
                        ctx->event_cb(ctx, ctx->signals[lidx].name,
                                      ctx->signals[lidx].value,
                                      ctx->event_cb_user_data);
                    }
                    if (ca_changed) {
                        check_and_trigger_impl(ctx, (uint32_t)lidx, old_ca,
                                               ctx->signals[lidx].value, 0, 1);
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                } else if (a->delay_value) {
                    /* Verilog: inertial delay assign #N lhs = rhs; */
                    qsim_bit_vector_t *dval = eval_expr(ctx, a->delay_value);
                    uint64_t n = 0;
                    if (dval) {
                        for (uint32_t i = 0; i < dval->width && i < 64; i++)
                            if (qsim_bit_get(dval, i).state == QSIM_1) n |= (1ULL << i);
                        qsim_bit_vector_free(dval);
                    }
                    uint64_t target_time = ctx->current_time + n;

                    /* Inertial filtering: remove events for same signal at
                     * earlier times (pulse narrower than propagation delay). */
                    sim_event_t *p = NULL, *c = ctx->event_head;
                    while (c) {
                        if (c->sig_idx == (uint32_t)lidx &&
                            c->time < target_time && !c->is_stmt_event) {
                            sim_event_t *vic = c;
                            if (p) p->next = c->next;
                            else ctx->event_head = c->next;
                            c = c->next;
                            if (!c) ctx->event_tail = p;
                            ctx->event_count--;
                            pool_free_event(ctx, vic);
                        } else {
                            p = c;
                            c = c->next;
                        }
                    }

                    schedule_event(ctx, target_time, 0,
                                   (uint32_t)lidx,
                                   qsim_bit_vector_clone(write_val), 0, -1, -1);
                } else {
                    /* Verilog: direct update (zero delay) — use resolution for net types */
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lidx, write_val, &old_ca, (int)i);

                    if (ca_changed) {
                        if (ctx->event_cb) {
                            ctx->event_cb(ctx, ctx->signals[lidx].name,
                                          ctx->signals[lidx].value,
                                          ctx->event_cb_user_data);
                        }
                        check_and_trigger_impl(ctx, (uint32_t)lidx, old_ca,
                                               ctx->signals[lidx].value, 0, 1);
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                }
                if (write_val != (final_val ? final_val : rhs_val)) {
                    /* arr_result was used, already freed final_val above */
                } else if (final_val && final_val != rhs_val) {
                    qsim_bit_vector_free(final_val);
                }
            }
        }
        if (entry->prefix[0])
            strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
        qsim_bit_vector_free(rhs_val);
    }

    /* Initial evaluation of UDP instances (same as CA eval above) */
    for (size_t ui = 0; ui < ctx->udp_instance_count; ui++) {
        udp_instance_entry_t *ue = &ctx->udp_instances[ui];
        if (ue->udp->is_sequential) continue;
        if (ue->output_sig_idx < 0) continue;
        evaluate_combinational_udp(ctx, ue);
    }
}

/* ── Phase 2.5: Resolve net types ──
 * Called after CA evaluation. Resolves all signals with needs_resolve set,
 * computes final values from per-driver slots, and records changes.
 * Returns the number of signals whose value changed. */
static size_t resolve_net_types(uir_sim_context_t *ctx,
                                signal_change_t **changes, size_t *change_count, size_t *change_cap) {
    size_t resolved_count = 0;
    for (size_t ri = 0; ri < ctx->signal_count; ri++) {
        sim_signal_t *rsig = &ctx->signals[ri];
        if (!rsig->needs_resolve) continue;
        rsig->needs_resolve = 0;

        qsim_bit_vector_t *old = qsim_bit_vector_clone(rsig->value);

        if (rsig->net_group == NET_GROUP_WAND ||
            rsig->net_group == NET_GROUP_WOR) {
            qsim_bit_vector_free(rsig->value);
            rsig->value = qsim_bit_vector_from_state(old->width, QSIM_Z);
            for (int _di = 0; _di < rsig->driver_count; _di++) {
                if (rsig->drivers[_di].process_id >= 0 && rsig->drivers[_di].value) {
                    if (rsig->net_group == NET_GROUP_WAND)
                        qsim_bit_vector_resolve_wand(rsig->value, rsig->drivers[_di].value);
                    else
                        qsim_bit_vector_resolve_wor(rsig->value, rsig->drivers[_di].value);
                }
            }
        } else if (rsig->net_group == NET_GROUP_STD_LOGIC) {
            /* Event-driven writes are already resolved in-place during Phase 1.
             * Check for process-driven driver slots (from signal_write_resolved
             * with process_id >= 0, e.g. mixed-language blocking assigns). */
            int has_process_drivers = 0;
            for (int _di = 0; _di < rsig->driver_count; _di++) {
                if (rsig->drivers[_di].process_id >= 0 && rsig->drivers[_di].value) {
                    has_process_drivers = 1; break;
                }
            }
            if (has_process_drivers) {
                /* Start from Z (neutral for std_logic resolution) and resolve
                 * ALL drivers: process-driven + event-accumulated. Previously the
                 * event-accumulated value (set by Phase 1) was discarded, causing
                 * VHDL process signal assignments to produce X when any process
                 * driver slot was occupied (e.g. mixed-language cont_assigns). */
                qsim_bit_vector_t *resolved = qsim_bit_vector_from_state(old->width, QSIM_Z);
                for (int _di = 0; _di < rsig->driver_count; _di++) {
                    if (rsig->drivers[_di].process_id >= 0 && rsig->drivers[_di].value) {
                        qsim_bit_vector_resolve_std_logic(resolved, rsig->drivers[_di].value);
                    }
                }
                /* Phase 1 event-driven writes also participate in the resolution.
                 * Skip rsig->value if it matches old (default init value, QSIM_X),
                 * since including uninitialized X would override process driver values
                 * (X × anything = X in the resolution table). Only include rsig->value
                 * when it has been explicitly written by event-driven (concurrent) sources. */
                { int _all_x = 1;
                  for (uint32_t _b = 0; _b < rsig->value->width && _all_x; _b++)
                      if (rsig->value->bits[_b].state != QSIM_X) _all_x = 0;
                  if (!_all_x)
                      qsim_bit_vector_resolve_std_logic(resolved, rsig->value); }
                qsim_bit_vector_free(rsig->value);
                rsig->value = resolved;
            }
            rsig->needs_resolve = 0;
            /* Fall through to change detection below.
             * Event path: old == current → no-op.
             * Process drivers: old vs resolved → detects change. */
        } else if (rsig->net_group == NET_GROUP_TRI0 ||
                   rsig->net_group == NET_GROUP_TRI1) {
            /* Check if any driver has a non-Z bit */
            int has_non_z = 0;
            for (int _di = 0; _di < rsig->driver_count; _di++) {
                if (rsig->drivers[_di].process_id >= 0 && rsig->drivers[_di].value) {
                    for (uint32_t _b = 0; _b < rsig->drivers[_di].value->width; _b++) {
                        if (rsig->drivers[_di].value->bits[_b].state != QSIM_Z) {
                            has_non_z = 1; break;
                        }
                    }
                }
                if (has_non_z) break;
            }
            if (has_non_z) {
                qsim_bit_vector_t *last_nz = NULL;
                for (int _di = rsig->driver_count - 1; _di >= 0; _di--) {
                    if (rsig->drivers[_di].process_id >= 0 && rsig->drivers[_di].value) {
                        int nz = 0;
                        for (uint32_t _b = 0; _b < rsig->drivers[_di].value->width; _b++) {
                            if (rsig->drivers[_di].value->bits[_b].state != QSIM_Z) {
                                nz = 1; break;
                            }
                        }
                        if (nz) { last_nz = rsig->drivers[_di].value; break; }
                    }
                }
                qsim_bit_vector_free(rsig->value);
                if (last_nz)
                    rsig->value = qsim_bit_vector_clone(last_nz);
                else
                    rsig->value = qsim_bit_vector_from_state(old->width, QSIM_X);
            } else {
                qsim_bit_vector_free(rsig->value);
                rsig->value = qsim_bit_vector_from_state(old->width,
                    rsig->net_group == NET_GROUP_TRI0 ? QSIM_0 : QSIM_1);
            }
        }

        if (!qsim_bit_vector_eq(old, rsig->value)) {
            rsig->last_change_time = ctx->current_time;
            if (*change_count >= *change_cap) {
                size_t nc = *change_cap ? *change_cap * 2 : 64;
                signal_change_t *n = realloc(*changes, nc * sizeof(signal_change_t));
                if (n) { *changes = n; *change_cap = nc; }
            }
            if (*changes) {
                (*changes)[*change_count].sig_idx = (int)ri;
                (*changes)[*change_count].old_val = old;
                (*change_count)++;
                resolved_count++;
            } else {
                qsim_bit_vector_free(old);
            }
        } else {
            qsim_bit_vector_free(old);
        }
    }
    return resolved_count;
}

/* ── Net type resolution write ── */

/* Write a value to a signal, respecting net type resolution.
 * process_id: cont_assign index for per-driver tracking, -1 for direct write (events).
 * Returns 1 if value changed (old_val_out set to clone of previous value).
 * Returns 0 if unchanged (*old_val_out is set to NULL).
 * Caller must free *old_val_out when done. */
static int signal_write_resolved(uir_sim_context_t *ctx, uint32_t sig_idx,
                                  qsim_bit_vector_t *new_val,
                                  qsim_bit_vector_t **old_val_out,
                                  int process_id) {
    sim_signal_t *sig = &ctx->signals[sig_idx];
#define SAVE_PREV(sig) do { \
    qsim_bit_vector_t *_old = (sig)->prev_value; \
    (sig)->prev_value = qsim_bit_vector_clone((sig)->value); \
    if (_old) qsim_bit_vector_free(_old); \
} while(0)
    if (!sig->value) { fprintf(stderr, "CRASH: sig %u has NULL value\n", sig_idx); abort(); }
    if (!new_val) { fprintf(stderr, "CRASH: sig %u received NULL new_val\n", sig_idx); abort(); }

    switch (sig->net_group) {
    case NET_GROUP_WIRE:
        /* Transfer ownership of old value to caller, store clone of new */
        SAVE_PREV(sig);
        *old_val_out = sig->value;
        sig->value = qsim_bit_vector_clone(new_val);
        break;

    case NET_GROUP_WAND:
    case NET_GROUP_WOR: {
        if (process_id < 0) {
            /* Direct write (event path) — overwrite directly */
            SAVE_PREV(sig);
            *old_val_out = sig->value;
            sig->value = qsim_bit_vector_clone(new_val);
            break;
        }
        /* Find or create driver slot for this process */
        int slot = -1;
        for (int _di = 0; _di < sig->driver_count; _di++) {
            if (sig->drivers[_di].process_id == process_id) {
                slot = _di; break;
            }
        }
        if (slot < 0) {
            /* New driver — find unused slot */
            for (int _di = 0; _di < MAX_DRIVERS_PER_SIGNAL; _di++) {
                if (sig->drivers[_di].process_id < 0) {
                    slot = _di;
                    sig->drivers[slot].process_id = process_id;
                    if (_di >= sig->driver_count)
                        sig->driver_count = _di + 1;
                    break;
                }
            }
        }
        if (slot < 0) break; /* out of driver slots — skip */
        /* Update driver value */
        qsim_bit_vector_free(sig->drivers[slot].value);
        sig->drivers[slot].value = qsim_bit_vector_clone(new_val);
        sig->needs_resolve = 1;
        /* Don't update sig->value directly — Phase 1.5 resolves all drivers.
         * Return 0 to suppress change-based triggering until final value known. */
        *old_val_out = NULL;
        return 0;
    }

    case NET_GROUP_STD_LOGIC: {
        if (process_id < 0) {
            /* Event path: overwrite directly (no resolution).
             * VHDL NBA events from a single process driver must overwrite
             * — the last assignment wins within the same delta. Resolution
             * only applies to multiple concurrent drivers (CA path below). */
            if (qsim_bit_vector_eq(sig->value, new_val)) {
                *old_val_out = NULL;
                return 0;
            }
            SAVE_PREV(sig);
            *old_val_out = sig->value;
            sig->value = qsim_bit_vector_clone(new_val);
            return 1;
        }
        /* Process-driven path: use per-process driver slots */
        int slot = -1;
        for (int _di = 0; _di < sig->driver_count; _di++) {
            if (sig->drivers[_di].process_id == process_id) {
                slot = _di; break;
            }
        }
        if (slot < 0) {
            for (int _di = 0; _di < MAX_DRIVERS_PER_SIGNAL; _di++) {
                if (sig->drivers[_di].process_id < 0) {
                    slot = _di;
                    sig->drivers[slot].process_id = process_id;
                    if (_di >= sig->driver_count)
                        sig->driver_count = _di + 1;
                    break;
                }
            }
        }
        if (slot < 0) break;
        qsim_bit_vector_free(sig->drivers[slot].value);
        sig->drivers[slot].value = qsim_bit_vector_clone(new_val);
        sig->needs_resolve = 1;
        *old_val_out = NULL;
        return 0;
    }

    case NET_GROUP_TRI0:
    case NET_GROUP_TRI1: {
        if (process_id < 0) {
            /* Direct write — overwrite directly */
            SAVE_PREV(sig);
            *old_val_out = sig->value;
            sig->value = qsim_bit_vector_clone(new_val);
            break;
        }
        /* Find or create driver slot */
        int slot = -1;
        for (int _di = 0; _di < sig->driver_count; _di++) {
            if (sig->drivers[_di].process_id == process_id) {
                slot = _di; break;
            }
        }
        if (slot < 0) {
            for (int _di = 0; _di < MAX_DRIVERS_PER_SIGNAL; _di++) {
                if (sig->drivers[_di].process_id < 0) {
                    slot = _di;
                    sig->drivers[slot].process_id = process_id;
                    if (_di >= sig->driver_count)
                        sig->driver_count = _di + 1;
                    break;
                }
            }
        }
        if (slot < 0) break;
        qsim_bit_vector_free(sig->drivers[slot].value);
        sig->drivers[slot].value = qsim_bit_vector_clone(new_val);
        sig->needs_resolve = 1;
        *old_val_out = NULL;
        return 0;
    }

    case NET_GROUP_SUPPLY0:
    case NET_GROUP_SUPPLY1:
        /* Supply nets maintain constant value — discard write */
        *old_val_out = NULL;
        return 0;

    default:
        /* Fallback: direct overwrite (NET_GROUP_TRIREG etc.) */
        SAVE_PREV(sig);
        *old_val_out = sig->value;
        sig->value = qsim_bit_vector_clone(new_val);
        break;
    }

    /* Check if value actually changed (non-resolved paths only) */
    if (*old_val_out && qsim_bit_vector_eq(*old_val_out, sig->value)) {
        qsim_bit_vector_free(*old_val_out);
        *old_val_out = NULL;
        return 0;
    }
    sig->last_change_time = ctx->current_time;
    return 1;
}

/* ── Built-in VHDL function evaluation ── */

static qsim_bit_vector_t *eval_vhdl_builtin_func(
    uir_sim_context_t *ctx, uir_func_call_t *fc,
    const vhdl_builtin_func_t *builtin)
{
    switch (builtin->kind) {

    case VHDL_FUNC_RISING_EDGE:
    case VHDL_FUNC_FALLING_EDGE: {
        if (fc->arg_count < 1) return qsim_bit_vector_from_state(1, QSIM_X);
        qsim_bit_vector_t *cur = eval_expr(ctx, fc->args[0]);
        if (!cur) return qsim_bit_vector_from_state(1, QSIM_X);

        qsim_value_t curr_bit = qsim_bit_get(cur, 0);
        qsim_value_t prev_bit = {QSIM_X, 0};
        const char *sig_name = "?";

        if (fc->args[0]->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)fc->args[0];
            int sig_idx = -1;
            sig_name = ref->name;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                sig_idx = find_signal_idx(ctx, prefixed);
            }
            if (sig_idx < 0) sig_idx = find_signal_idx(ctx, ref->name);
            if (sig_idx >= 0 && ctx->signals[sig_idx].prev_value)
                prev_bit = qsim_bit_get(ctx->signals[sig_idx].prev_value, 0);
        }

        int result_bit = 0;
        if (builtin->kind == VHDL_FUNC_RISING_EDGE)
            result_bit = (prev_bit.state == QSIM_0 && curr_bit.state == QSIM_1);
        else
            result_bit = (prev_bit.state == QSIM_1 && curr_bit.state == QSIM_0);

        qsim_bit_vector_t *result = qsim_bit_vector_alloc(1);
        if (result) qsim_bit_set(result, 0, result_bit ? QSIM_VAL_1 : QSIM_VAL_0);
        qsim_bit_vector_free(cur);
        return result;
    }

    case VHDL_FUNC_TO_INTEGER: {
        if (fc->arg_count < 1) return qsim_bit_vector_from_state(32, QSIM_X);
        qsim_bit_vector_t *val = eval_expr(ctx, fc->args[0]);
        if (!val) return qsim_bit_vector_from_state(32, QSIM_X);

        uint64_t accum = 0;
        for (uint32_t i = 0; i < val->width && i < 64; i++) {
            qsim_value_t b = qsim_bit_get(val, i);
            if (b.state == QSIM_1) accum |= (1ULL << i);
            else if (b.state != QSIM_0) {
                qsim_bit_vector_free(val);
                return qsim_bit_vector_from_state(32, QSIM_X);
            }
        }
        qsim_bit_vector_free(val);

        qsim_bit_vector_t *result = qsim_bit_vector_alloc(32);
        if (!result) return qsim_bit_vector_from_state(32, QSIM_X);
        for (uint32_t i = 0; i < 32; i++)
            qsim_bit_set(result, i, (accum & (1ULL << i)) ? QSIM_VAL_1 : QSIM_VAL_0);
        return result;
    }

    case VHDL_FUNC_TO_SIGNED:
    case VHDL_FUNC_TO_UNSIGNED: {
        if (fc->arg_count < 2) return qsim_bit_vector_from_state(1, QSIM_X);
        qsim_bit_vector_t *val = eval_expr(ctx, fc->args[0]);
        qsim_bit_vector_t *size_val = eval_expr(ctx, fc->args[1]);
        if (!val || !size_val) {
            qsim_bit_vector_free(val); qsim_bit_vector_free(size_val);
            return qsim_bit_vector_from_state(1, QSIM_X);
        }

        uint32_t new_width = 1;
        for (uint32_t i = 0; i < size_val->width && i < 32; i++) {
            qsim_value_t b = qsim_bit_get(size_val, i);
            if (b.state == QSIM_1) new_width = (i + 1 > new_width) ? i + 1 : new_width;
            else if (b.state != QSIM_0) { new_width = 1; break; }
        }
        qsim_bit_vector_free(size_val);

        qsim_bit_vector_t *result = qsim_bit_vector_alloc(new_width);
        if (!result) { qsim_bit_vector_free(val); return qsim_bit_vector_from_state(new_width, QSIM_X); }
        for (uint32_t i = 0; i < new_width; i++) {
            if (i < val->width)
                qsim_bit_set(result, i, qsim_bit_get(val, i));
            else
                qsim_bit_set(result, i, QSIM_VAL_0);
        }
        qsim_bit_vector_free(val);
        return result;
    }

    case VHDL_FUNC_RESIZE: {
        if (fc->arg_count < 2) return qsim_bit_vector_from_state(1, QSIM_X);
        qsim_bit_vector_t *val = eval_expr(ctx, fc->args[0]);
        qsim_bit_vector_t *size_val = eval_expr(ctx, fc->args[1]);
        if (!val || !size_val) {
            qsim_bit_vector_free(val); qsim_bit_vector_free(size_val);
            return qsim_bit_vector_from_state(1, QSIM_X);
        }

        uint32_t new_width = 1;
        for (uint32_t i = 0; i < size_val->width && i < 32; i++) {
            qsim_value_t b = qsim_bit_get(size_val, i);
            if (b.state == QSIM_1) new_width = (i + 1 > new_width) ? i + 1 : new_width;
            else if (b.state != QSIM_0) { new_width = 1; break; }
        }
        qsim_bit_vector_free(size_val);

        qsim_bit_vector_t *result = qsim_bit_vector_alloc(new_width);
        if (!result) { qsim_bit_vector_free(val); return qsim_bit_vector_from_state(new_width, QSIM_X); }
        for (uint32_t i = 0; i < new_width; i++) {
            if (i < val->width)
                qsim_bit_set(result, i, qsim_bit_get(val, i));
            else
                qsim_bit_set(result, i, QSIM_VAL_0);
        }
        qsim_bit_vector_free(val);
        return result;
    }

    default:
        return qsim_bit_vector_from_state(1, QSIM_X);
    }
}

/* ── Parallel delta evaluation helpers ──
 * signal_change_t is defined above, before sim_thread_state_t. */

static void process_events_apply(uir_sim_context_t *ctx, sim_thread_state_t *ts) {
    /* Process each event in this thread's work batch.
     * Same logic as the main Phase 1 loop, but only for batch-owned events. */
    for (size_t i = 0; i < ts->work_count; i++) {
        sim_event_t *ev = ts->work_batch[i];
        if (!ev) continue;

        /* NBA part-select merge */
        if (ev->has_part_select) {
            qsim_bit_vector_t *merged = qsim_bit_vector_clone(ctx->signals[ev->sig_idx].value);
            if (merged) {
                uint32_t part_w = (uint32_t)(ev->ps_hi - ev->ps_lo + 1);
                for (uint32_t j = 0; j < part_w && j < ev->value->width; j++) {
                    if ((uint32_t)ev->ps_lo + j < merged->width)
                        qsim_bit_set(merged, (uint32_t)ev->ps_lo + j, qsim_bit_get(ev->value, j));
                }
                qsim_bit_vector_free(ev->value);
                ev->value = merged;
            }
        }

        /* Write through resolution layer */
        qsim_bit_vector_t *old_val = NULL;
        if (signal_write_resolved(ctx, ev->sig_idx, ev->value, &old_val, -1)) {
            ctx->total_events++;
            /* Record change */
            if (ts->change_count >= ts->change_cap) {
                size_t nc = ts->change_cap ? ts->change_cap * 2 : 64;
                signal_change_t *n = realloc(ts->changes, nc * sizeof(signal_change_t));
                if (n) { ts->changes = n; ts->change_cap = nc; }
            }
            if (ts->changes) {
                ts->changes[ts->change_count].sig_idx = (int)ev->sig_idx;
                ts->changes[ts->change_count].old_val = old_val;
                ts->change_count++;
            } else {
                qsim_bit_vector_free(old_val);
            }
        }
    }
}

/* ── Parallel Phase 2a: Partition-scoped continuous assign evaluation ── */

/* Append a signal change to a thread's change list.
 * Used by ca_eval_for_thread to record CA-generated changes
 * for Phase 2b process triggering. */
static int thread_record_change(sim_thread_state_t *ts, int sig_idx,
                                 qsim_bit_vector_t *old_val) {
    if (ts->change_count >= ts->change_cap) {
        size_t nc = ts->change_cap ? ts->change_cap * 2 : 64;
        signal_change_t *n = realloc(ts->changes, nc * sizeof(signal_change_t));
        if (!n) return 0;
        ts->changes = n;
        ts->change_cap = nc;
    }
    ts->changes[ts->change_count].sig_idx = sig_idx;
    ts->changes[ts->change_count].old_val = old_val;
    ts->change_count++;
    return 1;
}

/* Evaluate continuous assigns in this thread's partition for a changed signal.
 * Partition-scoped version of check_and_trigger_impl CA-eval section:
 *  - Only checks CAs in ts->ca_indices[] (this thread's partitions)
 *  - Records changes in ts->changes[] for Phase 2b process triggering
 *  - Recursively handles cascades within the partition (depth-limited)
 *  - Delayed CAs (VHDL delta, Verilog #N) go to per-thread pending list via tls_ts
 */
static void ca_eval_for_thread(uir_sim_context_t *ctx, sim_thread_state_t *ts,
                                uint32_t sig_idx, const qsim_bit_vector_t *old_val,
                                const qsim_bit_vector_t *new_val, int depth) {
    if (depth > 16) return;
    if (!ctx->ca_partition || ts->ca_count == 0) return;

    for (size_t ii = 0; ii < ts->ca_count; ii++) {
        int ci = ts->ca_indices[ii];
        cont_assign_entry_t *entry = &ctx->cont_assigns[ci];
        int depends = 0;
        for (size_t d = 0; d < entry->dep_count; d++) {
            if (entry->dep_sigs[d] == (int)sig_idx) { depends = 1; break; }
        }
        if (!depends) continue;

        uir_assign_t *a = entry->assign;
        uint32_t saved_cw = ctx->current_context_width;
        char saved_pfx[520] = "";
        if (entry->prefix[0]) {
            strncpy(saved_pfx, ctx->current_prefix, sizeof(saved_pfx) - 1);
            saved_pfx[sizeof(saved_pfx) - 1] = '\0';
            strncpy(ctx->current_prefix, entry->prefix, sizeof(ctx->current_prefix) - 1);
            ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
        }
        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lhs_idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lhs_idx = find_signal_idx(ctx, prefixed);
            }
            if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
            if (lhs_idx >= 0)
                ctx->current_context_width = ctx->signals[lhs_idx].value->width;
        }
        qsim_bit_vector_t *rhs_val = eval_expr(ctx, a->rhs);
        ctx->current_context_width = saved_cw;
        if (!rhs_val) {
            if (entry->prefix[0])
                strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
            continue;
        }

        if (a->lhs && a->lhs->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)a->lhs;
            int lhs_idx = -1;
            if (ctx->current_prefix[0]) {
                char prefixed[520];
                snprintf(prefixed, sizeof(prefixed), "%s.%s",
                         ctx->current_prefix, ref->name);
                lhs_idx = find_signal_idx(ctx, prefixed);
            }
            if (lhs_idx < 0) lhs_idx = find_signal_idx(ctx, ref->name);
            if (lhs_idx >= 0) {
                uint32_t target_w = ctx->signals[lhs_idx].value->width;
                qsim_bit_vector_t *final_val = rhs_val;
                if (rhs_val->width != target_w) {
                    final_val = qsim_bit_vector_alloc(target_w);
                    if (final_val) {
                        for (uint32_t b = 0; b < target_w; b++) {
                            if (b < rhs_val->width)
                                qsim_bit_set(final_val, b, qsim_bit_get(rhs_val, b));
                            else
                                qsim_bit_set(final_val, b, QSIM_VAL_0);
                        }
                    }
                }
                qsim_bit_vector_t *to_apply = final_val ? final_val : rhs_val;

                qsim_bit_vector_t *arr_result = apply_array_write(ctx, ref, lhs_idx, to_apply);
                if (arr_result) {
                    if (final_val && final_val != rhs_val) qsim_bit_vector_free(final_val);
                    final_val = arr_result;
                    to_apply = arr_result;
                }

                qsim_bit_vector_t *ps_result = apply_part_select_write(ctx, ref, lhs_idx, to_apply);
                if (ps_result) {
                    if (final_val && final_val != rhs_val) qsim_bit_vector_free(final_val);
                    final_val = ps_result;
                    to_apply = ps_result;
                }

                if (a->delay > 0) {
                    /* VHDL: immediate write */
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lhs_idx, to_apply, &old_ca, -1);
                    if (ca_changed && ctx->event_cb) {
                        ctx->event_cb(ctx, ctx->signals[lhs_idx].name,
                                      ctx->signals[lhs_idx].value,
                                      ctx->event_cb_user_data);
                    }
                    if (ca_changed && (uint32_t)lhs_idx != sig_idx) {
                        thread_record_change(ts, lhs_idx, old_ca);
                        ca_eval_for_thread(ctx, ts, (uint32_t)lhs_idx, NULL,
                                           ctx->signals[lhs_idx].value, depth + 1);
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                } else if (a->delay_value) {
                    qsim_bit_vector_t *dval = eval_expr(ctx, a->delay_value);
                    uint64_t n = 0;
                    if (dval) {
                        for (uint32_t i = 0; i < dval->width && i < 64; i++)
                            if (qsim_bit_get(dval, i).state == QSIM_1) n |= (1ULL << i);
                        qsim_bit_vector_free(dval);
                    }
                    uint64_t target_time = ctx->current_time + n;
                    sim_event_t *prev = NULL, *cur = ctx->event_head;
                    while (cur) {
                        if (cur->sig_idx == (uint32_t)lhs_idx &&
                            cur->time < target_time && !cur->is_stmt_event) {
                            sim_event_t *victim = cur;
                            if (prev) prev->next = cur->next;
                            else ctx->event_head = cur->next;
                            cur = cur->next;
                            if (!cur) ctx->event_tail = prev;
                            ctx->event_count--;
                            pool_free_event_thread(ts, victim);
                        } else {
                            prev = cur;
                            cur = cur->next;
                        }
                    }
                    schedule_event(ctx, target_time, 0,
                                   (uint32_t)lhs_idx, qsim_bit_vector_clone(to_apply), 0, -1, -1);
                } else {
                    qsim_bit_vector_t *old_ca = NULL;
                    int ca_changed = signal_write_resolved(ctx, (uint32_t)lhs_idx, to_apply, &old_ca, ci);
                    if (ca_changed && ctx->event_cb) {
                        ctx->event_cb(ctx, ctx->signals[lhs_idx].name,
                                      ctx->signals[lhs_idx].value,
                                      ctx->event_cb_user_data);
                    }
                    if (ca_changed && (uint32_t)lhs_idx != sig_idx) {
                        thread_record_change(ts, lhs_idx, old_ca);
                        ca_eval_for_thread(ctx, ts, (uint32_t)lhs_idx, NULL,
                                           ctx->signals[lhs_idx].value, depth + 1);
                    } else {
                        qsim_bit_vector_free(old_ca);
                    }
                }
                if (final_val && final_val != rhs_val)
                    qsim_bit_vector_free(final_val);
            }
        }
        qsim_bit_vector_free(rhs_val);
        if (entry->prefix[0])
            strncpy(ctx->current_prefix, saved_pfx, sizeof(ctx->current_prefix) - 1);
    }
}

/* Trigger CA evaluation for all changes in this thread's partition.
 * Called via barrier pair in parallel Phase 2a.
 * Only evaluates CAs for the original Phase 1 changes (index < n).
 * CA-generated cascades are handled recursively within ca_eval_for_thread. */
static void process_ca_triggers(uir_sim_context_t *ctx, sim_thread_state_t *ts) {
    tls_ts = ts;  /* route schedule_event to per-thread pending list */
    size_t n = ts->change_count;
    for (size_t ci = 0; ci < n; ci++) {
        int sig_idx = ts->changes[ci].sig_idx;
        if (sig_idx < 0) continue;
        ca_eval_for_thread(ctx, ts, (uint32_t)sig_idx,
                           ts->changes[ci].old_val,
                           ctx->signals[sig_idx].value, 0);
    }
    tls_ts = NULL;
}

/* Trigger processes in this thread's partition that are sensitive to changes.
 * Only processes assigned to this partition's proc_indices are checked.
 * Process body execution (exec_stmt) can write any signal — WCC construction
 * ensures most writes are within partition. Cross-partition writes produce
 * pending events. */
static void process_partition_triggers(uir_sim_context_t *ctx, sim_thread_state_t *ts) {
    tls_ts = ts;  /* route schedule_event to per-thread pending list */
    for (size_t ci = 0; ci < ts->change_count; ci++) {
        int sig_idx = ts->changes[ci].sig_idx;
        if (sig_idx < 0) continue;
        uir_node_t *sig_node = ctx->signals[sig_idx].node;
        if (!sig_node) continue;

        for (size_t pi = 0; pi < ts->proc_count; pi++) {
            uir_process_t *proc = ctx->processes[ts->proc_indices[pi]];
            if (!proc) continue;
            for (size_t s = 0; s < proc->sensitivity_count; s++) {
                if (proc->sensitivity_list[s].signal != sig_node) continue;
                /* Edge/level check (same as check_and_trigger_impl) */
                int edge = proc->sensitivity_list[s].edge;
                int should_trigger = 0;
                if (edge == 0) {
                    /* Level-sensitive: any bit changed */
                    should_trigger = 1;
                } else {
                    /* Edge-sensitive: check bit 0 */
                    qsim_logic_state_t old_st = qsim_bit_get(ts->changes[ci].old_val, 0).state;
                    qsim_logic_state_t new_st = qsim_bit_get(ctx->signals[sig_idx].value, 0).state;
                    if (edge == 1) /* posedge */
                        should_trigger = (old_st != QSIM_1 && new_st == QSIM_1);
                    else /* negedge */
                        should_trigger = (old_st != QSIM_0 && new_st == QSIM_0);
                }
                if (should_trigger) {
                    strncpy(ctx->current_prefix, ctx->process_prefixes[ts->proc_indices[pi]],
                            sizeof(ctx->current_prefix) - 1);
                    ctx->current_prefix[sizeof(ctx->current_prefix) - 1] = '\0';
                    ctx->current_process_id = (int)ts->proc_indices[pi];
                    exec_stmt(ctx, proc->body);
                    ctx->current_process_id = -1;
                    ctx->current_prefix[0] = '\0';
                }
                break; /* one trigger per signal per process */
            }
        }
    }
    tls_ts = NULL;
}

/* Merge all per-thread pending event lists into the global event queue.
 * Each pending list is built in creation order (roughly time-ordered during
 * Phase 2b since all NBAs share the same (time, delta)). The list is spliced
 * into the global queue at the position of its first event. */
static void merge_thread_pending_events(uir_sim_context_t *ctx) {
    for (int t = 0; t < ctx->thread_count; t++) {
        sim_thread_state_t *ts = &ctx->threads[t];
        sim_event_t *list = ts->pending_head;
        if (!list) continue;

        sim_event_t *list_tail = ts->pending_tail;
        size_t list_count = ts->pending_count;
        ts->pending_head = NULL;
        ts->pending_tail = NULL;
        ts->pending_count = 0;

        if (!ctx->event_head) {
            ctx->event_head = list;
            ctx->event_tail = list_tail;
        } else {
            sim_event_t *cur = ctx->event_head, *prev = NULL;
            while (cur && (cur->time < list->time ||
                   (cur->time == list->time && cur->delta <= list->delta))) {
                prev = cur;
                cur = cur->next;
            }
            if (!prev) {
                list_tail->next = ctx->event_head;
                ctx->event_head = list;
            } else {
                list_tail->next = cur;
                prev->next = list;
                if (!cur) ctx->event_tail = list_tail;
            }
        }
        ctx->event_count += list_count;
    }
}

/* Worker thread main function for parallel delta evaluation */
static void *worker_main(void *arg) {
    sim_thread_state_t *ts = (sim_thread_state_t *)arg;
    uir_sim_context_t *ctx = ts->ctx;
    while (1) {
        sim_barrier_wait(&ctx->phase_barrier);
        int phase = sim_atomic_load(&ctx->parallel_phase);
        if (phase < 0) break; /* PHASE_EXIT */

        switch (phase) {
        case 1: /* PHASE_APPLY */
            process_events_apply(ctx, ts);
            break;
        case 2: /* PHASE_PROCESS */
            process_partition_triggers(ctx, ts);
            break;
        default:
            break;
        }
    }
    return NULL;
}

int uir_sim_run(uir_sim_context_t *ctx, uint64_t duration) {
    if (!ctx) return 0;

    uint64_t end_time = ctx->current_time + duration;

    /* Create thread pool on first run if multi-threaded */
    if (ctx->thread_count > 1 && !ctx->workers) {
        ctx->workers = calloc(ctx->thread_count - 1, sizeof(sim_thread_t));
        ctx->threads = calloc(ctx->thread_count, sizeof(sim_thread_state_t));
        if (ctx->workers && ctx->threads) {
            sim_barrier_init(&ctx->phase_barrier, (unsigned)ctx->thread_count);
            for (int i = 0; i < ctx->thread_count; i++) {
                ctx->threads[i].id = i;
                ctx->threads[i].ctx = ctx;
            }
            /* Assign partitions to threads (round-robin by partition ID) */
            for (int p = 0; p < ctx->partition_count; p++) {
                int t = p % ctx->thread_count;
                ctx->threads[t].partition_count++;
            }
            int accumulator = 0;
            for (int i = 0; i < ctx->thread_count; i++) {
                ctx->threads[i].partition_first = accumulator;
                accumulator += ctx->threads[i].partition_count;
            }
            int worker_ok = 1;
            for (int i = 1; i < ctx->thread_count; i++) {
                if (sim_thread_create(&ctx->workers[i-1], worker_main, &ctx->threads[i]) != 0) {
                    ctx->workers[i-1] = NULL;
                    worker_ok = 0;
                }
            }
            if (!worker_ok) {
                /* Worker creation failed — fall back to single-threaded */
                for (int i = 1; i < ctx->thread_count; i++)
                    if (ctx->workers[i-1]) { sim_thread_join(ctx->workers[i-1]); ctx->workers[i-1] = NULL; }
                ctx->thread_count = 1;
            }
        } else {
            /* Allocation failed — fall back to single-threaded */
            ctx->thread_count = 1;
        }
    }

    /* Initial evaluation */
    initial_eval(ctx);

    uint64_t last_delta_time = UINT64_MAX;
    uint32_t deltas_at_current_time = 0;

    while (ctx->event_count > 0 && !ctx->stop_requested && !ctx->finish_requested) {
        /* Get current batch key from the first event */
        uint64_t batch_time = ctx->event_head->time;
        uint32_t batch_delta = ctx->event_head->delta;

        if (batch_time > end_time) break;

        /* Track delta iterations at the same time to detect combinatorial loops */
        if (batch_time != last_delta_time) {
            last_delta_time = batch_time;
            deltas_at_current_time = 0;
        }
        if (++deltas_at_current_time > ctx->max_deltas_per_time) {
            if (ctx->display_cb) {
                char buf[256];
                snprintf(buf, sizeof(buf), "SIM ERROR: combinatorial loop detected "
                         "at time %llu (%u delta iterations without time advancement)",
                         (unsigned long long)batch_time, (unsigned)deltas_at_current_time);
                ctx->display_cb(ctx, buf, ctx->display_cb_user_data);
            }
            ctx->stop_requested = 1;
            break;
        }

        ctx->current_time = batch_time;
        ctx->current_delta = batch_delta;
        ctx->driver_gen_counter++;

        /* ── Phase 1: Process ALL events at this (time, delta) ── */
        signal_change_t *changes = NULL;
        size_t change_count = 0, change_cap = 0;

        if (ctx->thread_count > 1) {
            /* ── Parallel Phase 1 ── */
            /* Extract events and distribute to per-thread work batches */
            sim_event_t **batch_events = NULL;
            size_t batch_count = 0, batch_cap = 0;

            /* Reset per-thread work counts before pop loop */
            for (int ti = 0; ti < ctx->thread_count; ti++)
                ctx->threads[ti].work_count = 0;

            while (ctx->event_count > 0 &&
                   ctx->event_head->time == batch_time &&
                   ctx->event_head->delta == batch_delta) {
                sim_event_t *ev = pop_event(ctx);
                if (!ev) break;
                if (ev->cancelled) { pool_free_event(ctx, ev); continue; }

                /* Stmt events: leader handles (serial) */
                if (ev->is_stmt_event) {
                    /* Process stmt event directly (same as single-threaded path) */
                    ctx->recording_mode = 1;
                    ctx->recorded_count = 0;
                    exec_stmt(ctx, ev->stmt);
                    if (ev->loop_always && ev->owner_body)
                        exec_stmt(ctx, ev->owner_body);
                    ctx->recording_mode = 0;
                    /* Deduplicate: keep only first write per signal (oldest old_val) */
                    for (size_t ri = 0; ri < ctx->recorded_count; ri++) {
                        int si = ctx->recorded_changes[ri].sig_idx;
                        if (si < 0) continue;
                        for (size_t rj = 0; rj < ri; rj++) {
                            if (ctx->recorded_changes[rj].sig_idx == si) {
                                qsim_bit_vector_free(ctx->recorded_changes[ri].old_val);
                                ctx->recorded_changes[ri].sig_idx = -1;
                                break;
                            }
                        }
                    }
                    /* Append to thread 0 change list for Phase 2 */
                    for (size_t ri = 0; ri < ctx->recorded_count; ri++) {
                        int si = ctx->recorded_changes[ri].sig_idx;
                        if (si < 0) continue;
                        if (qsim_bit_vector_eq(ctx->recorded_changes[ri].old_val,
                                                ctx->signals[si].value)) {
                            qsim_bit_vector_free(ctx->recorded_changes[ri].old_val);
                            continue;
                        }
                        size_t cc = ctx->threads[0].change_count;
                        size_t cap = ctx->threads[0].change_cap;
                        if (cc >= cap) {
                            size_t nc = cap ? cap * 2 : 64;
                            signal_change_t *n = realloc(ctx->threads[0].changes,
                                                         nc * sizeof(signal_change_t));
                            if (!n) { qsim_bit_vector_free(ctx->recorded_changes[ri].old_val); break; }
                            ctx->threads[0].changes = n;
                            ctx->threads[0].change_cap = nc;
                        }
                        ctx->threads[0].changes[cc].sig_idx = si;
                        ctx->threads[0].changes[cc].old_val = ctx->recorded_changes[ri].old_val;
                        ctx->threads[0].change_count++;
                    }
                    ctx->recorded_count = 0;
                    pool_free_event_thread(&ctx->threads[0], ev);
                    continue;
                }

                /* Normal signal event: add to batch array and count per thread */
                if (batch_count >= batch_cap) {
                    size_t nc = batch_cap ? batch_cap * 2 : 256;
                    sim_event_t **n = realloc(batch_events, nc * sizeof(sim_event_t *));
                    if (!n) { free(batch_events); batch_events = NULL; break; }
                    batch_events = n;
                    batch_cap = nc;
                }
                batch_events[batch_count++] = ev;
                {
                    int sig_idx = (int)ev->sig_idx;
                    int part = (sig_idx >= 0 && ctx->signal_partition) ? ctx->signal_partition[sig_idx] : 0;
                    ctx->threads[part % ctx->thread_count].work_count++;
                }
            }

            /* Distribute batch events to threads by signal partition */
            if (batch_events && batch_count > 0) {
                /* Reset per-thread change counts (work_count set during pop loop) */
                for (int ti = 0; ti < ctx->thread_count; ti++) {
                    ctx->threads[ti].change_count = 0;
                }

                /* Allocate thread work batches (pre-counted during pop loop) */
                for (int ti = 0; ti < ctx->thread_count; ti++) {
                    size_t wc = ctx->threads[ti].work_count;
                    if (wc > 0 && (!ctx->threads[ti].work_batch || wc > ctx->threads[ti].work_cap)) {
                        free(ctx->threads[ti].work_batch);
                        ctx->threads[ti].work_batch = calloc(wc, sizeof(sim_event_t *));
                        ctx->threads[ti].work_cap = wc;
                    }
                }

                /* Fill thread work batches */
                size_t *wi = calloc(ctx->thread_count, sizeof(size_t));
                if (wi) {
                    for (size_t bi = 0; bi < batch_count; bi++) {
                        int sig_idx = (int)batch_events[bi]->sig_idx;
                        int part = (sig_idx >= 0 && ctx->signal_partition) ? ctx->signal_partition[sig_idx] : 0;
                        int ti = part % ctx->thread_count;
                        ctx->threads[ti].work_batch[wi[ti]++] = batch_events[bi];
                    }
                    free(wi);
                }

                /* Compute proc_indices per thread (once, cached) */
                for (int ti = 0; ti < ctx->thread_count; ti++) {
                    if (ctx->threads[ti].proc_indices) continue;
                    /* Count processes for this thread's partitions */
                    size_t pc = 0;
                    for (size_t pi = 0; pi < ctx->process_count; pi++) {
                        int p = ctx->process_partition ? ctx->process_partition[pi] : 0;
                        if (p % ctx->thread_count == ti) pc++;
                    }
                    if (pc > 0) {
                        ctx->threads[ti].proc_indices = malloc(pc * sizeof(int));
                        ctx->threads[ti].proc_count = 0;
                        for (size_t pi = 0; pi < ctx->process_count; pi++) {
                            int p = ctx->process_partition ? ctx->process_partition[pi] : 0;
                            if (p % ctx->thread_count == ti)
                                ctx->threads[ti].proc_indices[ctx->threads[ti].proc_count++] = (int)pi;
                        }
                    }
                }

                /* Compute ca_indices per thread (once, cached) */
                for (int ti = 0; ti < ctx->thread_count; ti++) {
                    if (ctx->threads[ti].ca_indices) continue;
                    size_t cc = 0;
                    for (size_t ci = 0; ci < ctx->cont_assign_count; ci++) {
                        int p = ctx->ca_partition ? ctx->ca_partition[ci] : 0;
                        if (p % ctx->thread_count == ti) cc++;
                    }
                    if (cc > 0) {
                        ctx->threads[ti].ca_indices = malloc(cc * sizeof(int));
                        ctx->threads[ti].ca_count = 0;
                        for (size_t ci = 0; ci < ctx->cont_assign_count; ci++) {
                            int p = ctx->ca_partition ? ctx->ca_partition[ci] : 0;
                            if (p % ctx->thread_count == ti)
                                ctx->threads[ti].ca_indices[ctx->threads[ti].ca_count++] = (int)ci;
                        }
                    }
                }

                /* Parallel Phase 1: apply events */
                ctx->parallel_phase = 1;
                sim_barrier_wait(&ctx->phase_barrier);
                /* Leader processes its own batch */
                process_events_apply(ctx, &ctx->threads[0]);
                sim_barrier_wait(&ctx->phase_barrier);

                /* Keep per-thread changes intact for Phase 2b.
                 * Free all processed events to per-thread local free lists
                 * instead of the global pool, avoiding pool_mutex contention. */
                for (int ti = 0; ti < ctx->thread_count; ti++) {
                    for (size_t ei = 0; ei < ctx->threads[ti].work_count; ei++) {
                        if (ctx->threads[ti].work_batch[ei])
                            pool_free_event_thread(&ctx->threads[ti], ctx->threads[ti].work_batch[ei]);
                    }
                }
            }
            free(batch_events);
            } else {
                /* ── Original single-threaded Phase 1 ── */
                /* Track NET_GROUP_STD_LOGIC signals that have received event-driven
                 * writes in this batch. When multiple events target the same signal,
                 * the std_logic resolution table determines the final value instead
                 * of "last write wins". */
                int *ev_stdlogic_seen = calloc(ctx->signal_count, sizeof(int));
                qsim_bit_vector_t **ev_stdlogic_orig = calloc(ctx->signal_count, sizeof(qsim_bit_vector_t *));
                int ev_stdlogic_seen_alloc = 0;

                while (ctx->event_count > 0 &&
                       ctx->event_head->time == batch_time &&
                       ctx->event_head->delta == batch_delta) {
                    sim_event_t *ev = pop_event(ctx);

                    if (ev->is_stmt_event) {
                        ctx->recording_mode = 1;
                        ctx->recorded_count = 0;
                        exec_stmt(ctx, ev->stmt);
                        if (ev->loop_always && ev->owner_body)
                            exec_stmt(ctx, ev->owner_body);
                        ctx->recording_mode = 0;
                        /* Deduplicate: keep only first write per signal */
                        for (size_t ri = 0; ri < ctx->recorded_count; ri++) {
                            int si = ctx->recorded_changes[ri].sig_idx;
                            if (si < 0) continue;
                            for (size_t rj = 0; rj < ri; rj++) {
                                if (ctx->recorded_changes[rj].sig_idx == si) {
                                    qsim_bit_vector_free(ctx->recorded_changes[ri].old_val);
                                    ctx->recorded_changes[ri].sig_idx = -1;
                                    break;
                                }
                            }
                        }
                        /* Append to change list for Phase 2 */
                        for (size_t ri = 0; ri < ctx->recorded_count; ri++) {
                            int si = ctx->recorded_changes[ri].sig_idx;
                            if (si < 0) continue;
                            if (qsim_bit_vector_eq(ctx->recorded_changes[ri].old_val,
                                                    ctx->signals[si].value)) {
                                qsim_bit_vector_free(ctx->recorded_changes[ri].old_val);
                                continue;
                            }
                            if (change_count >= change_cap) {
                                size_t nc = change_cap ? change_cap * 2 : 64;
                                signal_change_t *n = realloc(changes, nc * sizeof(signal_change_t));
                                if (!n) { qsim_bit_vector_free(ctx->recorded_changes[ri].old_val); break; }
                                changes = n; change_cap = nc;
                            }
                            changes[change_count].sig_idx = si;
                            changes[change_count].old_val = ctx->recorded_changes[ri].old_val;
                            change_count++;
                        }
                        ctx->recorded_count = 0;

                        pool_free_event(ctx, ev);
                        continue;
                    }

                    /* For NBA part-select events: merge */
                    if (ev->has_part_select) {
                        qsim_bit_vector_t *merged = qsim_bit_vector_clone(ctx->signals[ev->sig_idx].value);
                        if (merged) {
                            uint32_t part_w = (uint32_t)(ev->ps_hi - ev->ps_lo + 1);
                            for (uint32_t i = 0; i < part_w && i < ev->value->width; i++) {
                                if ((uint32_t)ev->ps_lo + i < merged->width)
                                    qsim_bit_set(merged, (uint32_t)ev->ps_lo + i, qsim_bit_get(ev->value, i));
                            }
                            qsim_bit_vector_free(ev->value);
                            ev->value = merged;
                        }
                    }

                    /* Write through resolution layer.
                     * For NET_GROUP_STD_LOGIC: track the original pre-batch value and
                     * defer change detection to the post-loop check. This handles
                     * multi-event conflicts (multiple VHDL processes driving the same
                     * std_logic signal in the same delta) by resolving all events
                     * through the IEEE 1164 resolution table instead of last-write-wins.
                     * Part-select events (has_part_select) skip this resolution
                     * because they target specific bit ranges (e.g., array elements)
                     * and the part-select merge above already produced the correct
                     * full-signal value. */
                    qsim_bit_vector_t *old_val = NULL;
                    int sig_is_stdlogic = (ctx->signals[ev->sig_idx].net_group == NET_GROUP_STD_LOGIC);
                    if (sig_is_stdlogic && ev_stdlogic_seen[ev->sig_idx] && !ev->has_part_select) {
                        /* Second+ event for this STD_LOGIC signal: resolve with current value */
                        qsim_bit_vector_t *resolved = qsim_bit_vector_from_state(
                            ctx->signals[ev->sig_idx].value->width, QSIM_Z);
                        qsim_bit_vector_resolve_std_logic(resolved, ctx->signals[ev->sig_idx].value);
                        qsim_bit_vector_resolve_std_logic(resolved, ev->value);
                        qsim_bit_vector_free(ctx->signals[ev->sig_idx].value);
                        ctx->signals[ev->sig_idx].value = resolved;
                        /* No change recording — the post-loop check handles it */
                    } else if (signal_write_resolved(ctx, ev->sig_idx, ev->value, &old_val, -1)) {
                        ctx->total_events++;

                        if (ctx->event_cb) {
                            ctx->event_cb(ctx, ctx->signals[ev->sig_idx].name,
                                          ctx->signals[ev->sig_idx].value,
                                          ctx->event_cb_user_data);
                        }

                        if (sig_is_stdlogic) {
                            /* First event for this STD_LOGIC signal: save original,
                             * defer change recording to post-loop check. */
                            ev_stdlogic_seen[ev->sig_idx] = 1;
                            ev_stdlogic_orig[ev->sig_idx] = old_val;
                            ev_stdlogic_seen_alloc = 1;
                            old_val = NULL; /* ownership -> ev_stdlogic_orig */
                            /* Don't record change here — post-loop handles it */
                        } else {
                            if (change_count >= change_cap) {
                                size_t nc = change_cap ? change_cap * 2 : 64;
                                signal_change_t *n = realloc(changes, nc * sizeof(signal_change_t));
                                if (!n) break;
                                changes = n; change_cap = nc;
                            }
                            changes[change_count].sig_idx = (int)ev->sig_idx;
                            changes[change_count].old_val = old_val;
                            change_count++;
                        }
                    } else {
                    }

                    pool_free_event(ctx, ev);
                }  /* end while event loop */

                /* After processing all events in this delta batch, check resolved
                 * STD_LOGIC values for changes from their pre-batch originals. */
                if (ev_stdlogic_seen_alloc) {
                    for (size_t si = 0; si < ctx->signal_count; si++) {
                        if (!ev_stdlogic_seen[si]) continue;
                        if (qsim_bit_vector_eq(ctx->signals[si].value, ev_stdlogic_orig[si])) {
                            qsim_bit_vector_free(ev_stdlogic_orig[si]);
                        } else {
                            if (change_count >= change_cap) {
                                size_t nc = change_cap ? change_cap * 2 : 64;
                                signal_change_t *n = realloc(changes, nc * sizeof(signal_change_t));
                                if (!n) { qsim_bit_vector_free(ev_stdlogic_orig[si]); break; }
                                changes = n; change_cap = nc;
                            }
                            changes[change_count].sig_idx = (int)si;
                            changes[change_count].old_val = ev_stdlogic_orig[si];
                            change_count++;
                            ctx->total_events++;
                            if (ctx->event_cb) {
                                ctx->event_cb(ctx, ctx->signals[si].name,
                                              ctx->signals[si].value,
                                              ctx->event_cb_user_data);
                            }
                        }
                    }
                    free(ev_stdlogic_orig);
                }
                free(ev_stdlogic_seen);
            }  /* end single-threaded else block */

        /* Phase 1.5: Port wire propagation with cascade settling.
         * Forward changes from port wire source signals to destination signals
         * so that processes sensitive to child port signals are triggered.
         * Only INPUT ports (parent→child) are propagated here — OUTPUT port
         * (child→parent) propagation is handled by Phase 3 inside
         * check_and_trigger_impl and Phase 2b post-process port propagation.
         * Without this guard, Phase 1.5 unconditionally overwrites forced
         * parent signals with stale child OUTPUT values.
         *
         * Events are processed immediately in a cascade loop so multi-level
         * hierarchy (TB→level1→level2) settles within one delta before
         * Phase 2 process triggering. */
        {
            int _pw_cascade = 1;
            int _pw_limit = 16;
            while (_pw_cascade && --_pw_limit > 0) {
                _pw_cascade = 0;
                size_t _pw_before = ctx->event_count;
                for (size_t _w = 0; _w < ctx->port_wire_count; _w++) {
                    if (ctx->port_wires[_w].dir == UIR_PORT_OUT) continue;
                    uint32_t _src = (uint32_t)ctx->port_wires[_w].src_sig_idx;
                    uint32_t _dst = (uint32_t)ctx->port_wires[_w].dst_sig_idx;
                    if (ctx->signals[_src].value->width != ctx->signals[_dst].value->width) continue;
                    if (!qsim_bit_vector_eq(ctx->signals[_src].value, ctx->signals[_dst].value)) {
                        qsim_bit_vector_t *_val = qsim_bit_vector_clone(ctx->signals[_src].value);
                        schedule_event(ctx, ctx->current_time, ctx->current_delta,
                                       _dst, _val, 0, -1, -1);
                    }
                }
                if (ctx->event_count > _pw_before &&
                    ctx->event_head->time == batch_time &&
                    ctx->event_head->delta == batch_delta) {
                    while (ctx->event_count > 0 &&
                           ctx->event_head->time == batch_time &&
                           ctx->event_head->delta == batch_delta) {
                        sim_event_t *_ev = pop_event(ctx);
                        if (!_ev) break;
                        if (_ev->is_stmt_event) { pool_free_event(ctx, _ev); continue; }
                        qsim_bit_vector_t *_old = NULL;
                        if (signal_write_resolved(ctx, _ev->sig_idx, _ev->value, &_old, -1)) {
                            if (ctx->thread_count > 1) {
                                size_t _cc = ctx->threads[0].change_count;
                                size_t _cp = ctx->threads[0].change_cap;
                                if (_cc >= _cp) {
                                    size_t _nc = _cp ? _cp * 2 : 64;
                                    signal_change_t *_n = realloc(ctx->threads[0].changes,
                                        _nc * sizeof(signal_change_t));
                                    if (!_n) { qsim_bit_vector_free(_old); pool_free_event(ctx, _ev); break; }
                                    ctx->threads[0].changes = _n;
                                    ctx->threads[0].change_cap = _nc;
                                }
                                ctx->threads[0].changes[_cc].sig_idx = (int)_ev->sig_idx;
                                ctx->threads[0].changes[_cc].old_val = _old;
                                ctx->threads[0].change_count++;
                            } else {
                                if (change_count >= change_cap) {
                                    size_t _nc = change_cap ? change_cap * 2 : 64;
                                    signal_change_t *_n = realloc(changes, _nc * sizeof(signal_change_t));
                                    if (!_n) { qsim_bit_vector_free(_old); pool_free_event(ctx, _ev); break; }
                                    changes = _n;
                                    change_cap = _nc;
                                }
                                changes[change_count].sig_idx = (int)_ev->sig_idx;
                                changes[change_count].old_val = _old;
                                change_count++;
                            }
                            ctx->total_events++;
                            _pw_cascade = 1; /* may have created new mismatches */
                        }
                        pool_free_event(ctx, _ev);
                    }
                }
            }
        }

        /* Phase 2: Trigger processes and waiters for all changed signals
         * Now all signals at this (time, delta) are settled, so processes
         * and waiters see consistent values.
         * In parallel mode: CA evaluation (serial) -> process execution (parallel)
         * -> waiters + cleanup (serial).
         */
        if (ctx->thread_count > 1) {
            /* Phase 2a: Parallel CA evaluation by partition.
             * Each thread evaluates CAs in its own partitions.
             * CA-generated changes go to per-thread change lists
             * and are picked up by Phase 2b process triggering. */
            ctx->parallel_phase = 3;
            sim_barrier_wait(&ctx->phase_barrier);
            process_ca_triggers(ctx, &ctx->threads[0]);
            sim_barrier_wait(&ctx->phase_barrier);

            /* Phase 2b: Parallel process body execution by partition */
            ctx->parallel_phase = 2;
            sim_barrier_wait(&ctx->phase_barrier);
            process_partition_triggers(ctx, &ctx->threads[0]);
            sim_barrier_wait(&ctx->phase_barrier);

            /* Merge per-thread pending events into global queue */
            merge_thread_pending_events(ctx);

            /* Phase 2c: Check waiters and free per-thread changes (serial) */
            for (int ti = 0; ti < ctx->thread_count; ti++) {
                for (size_t ci = 0; ci < ctx->threads[ti].change_count; ci++) {
                    int sig_idx = ctx->threads[ti].changes[ci].sig_idx;
                    if (sig_idx < 0) continue;
                    check_waiters_on_change(ctx, (uint32_t)sig_idx,
                                            ctx->threads[ti].changes[ci].old_val,
                                            ctx->signals[sig_idx].value);
                    qsim_bit_vector_free(ctx->threads[ti].changes[ci].old_val);
                }
                free(ctx->threads[ti].changes);
                ctx->threads[ti].changes = NULL;
                ctx->threads[ti].change_count = 0;
                ctx->threads[ti].change_cap = 0;
            }

            /* Phase 2.5: Resolve net types from CA driver slots (parallel path) */
            {
                size_t rcc = 0, rcap = 0;
                signal_change_t *rc = NULL;
                while (resolve_net_types(ctx, &rc, &rcc, &rcap) > 0) {
                    /* Apply resolved changes: trigger processes and waiters */
                    for (size_t i = 0; i < rcc; i++) {
                        check_and_trigger_ca_only(ctx, (uint32_t)rc[i].sig_idx,
                                                  rc[i].old_val, ctx->signals[rc[i].sig_idx].value);
                    }
                    /* Merge CA-only results, then trigger processes (Phase 2b) */
                    merge_thread_pending_events(ctx);
                    ctx->parallel_phase = 2;
                    sim_barrier_wait(&ctx->phase_barrier);
                    process_partition_triggers(ctx, &ctx->threads[0]);
                    sim_barrier_wait(&ctx->phase_barrier);
                    merge_thread_pending_events(ctx);
                    /* Waiters */
                    for (int ti = 0; ti < ctx->thread_count; ti++) {
                        for (size_t ci = 0; ci < ctx->threads[ti].change_count; ci++) {
                            int sig_idx = ctx->threads[ti].changes[ci].sig_idx;
                            if (sig_idx < 0) continue;
                            check_waiters_on_change(ctx, (uint32_t)sig_idx,
                                                    ctx->threads[ti].changes[ci].old_val,
                                                    ctx->signals[sig_idx].value);
                            qsim_bit_vector_free(ctx->threads[ti].changes[ci].old_val);
                        }
                        free(ctx->threads[ti].changes);
                        ctx->threads[ti].changes = NULL;
                        ctx->threads[ti].change_count = 0;
                        ctx->threads[ti].change_cap = 0;
                    }
                    rcc = 0; /* reset for next potential round */
                }
                free(rc);
            }
        } else {
            /* Serial Phase 2: evaluate CAs, trigger processes, check waiters */
            for (size_t i = 0; i < change_count; i++) {
                const qsim_bit_vector_t *new_val = ctx->signals[changes[i].sig_idx].value;
                check_and_trigger(ctx, (uint32_t)changes[i].sig_idx,
                                  changes[i].old_val, new_val);
                check_waiters_on_change(ctx, (uint32_t)changes[i].sig_idx,
                                         changes[i].old_val, new_val);
                qsim_bit_vector_free(changes[i].old_val);
            }
            free(changes);

            /* Phase 2.5: Resolve net types (WAND/WOR/TRI0/TRI1) from CA driver slots */
            {
                size_t rcc = 0, rcap = 0;
                signal_change_t *rc = NULL;
                while (resolve_net_types(ctx, &rc, &rcc, &rcap) > 0) {
                    for (size_t i = 0; i < rcc; i++) {
                        const qsim_bit_vector_t *nv = ctx->signals[rc[i].sig_idx].value;
                        check_and_trigger(ctx, (uint32_t)rc[i].sig_idx, rc[i].old_val, nv);
                        check_waiters_on_change(ctx, (uint32_t)rc[i].sig_idx, rc[i].old_val, nv);
                        qsim_bit_vector_free(rc[i].old_val);
                    }
                    rcc = 0; /* reset for next potential round */
                }
                free(rc);
            }
        }

        /* Check $monitor after each delta batch */
        if (ctx->monitor) {
            monitor_check_changed(ctx, ctx->monitor);
        }
    }

    /* Advance delta to prevent new set() events from colliding with
     * the same delta where the last cascade batch was processed.
     * This ensures external event scheduling starts at a fresh delta. */
    ctx->current_delta++;

    return 1;
}

/* ── Checkpoint save/restore (binary serialization of simulation state) ── */

#define CKPT_MAGIC "QSIMCKPT"
#define CKPT_VERSION 1

/* Section IDs */
#define CKPT_SECT_SIGNALS 1
#define CKPT_SECT_TIME 2
#define CKPT_SECT_EVENTS 3
#define CKPT_SECT_COVERAGE 4
#define CKPT_SECT_BREAKPOINT_HIT 5

/* Binary write helpers (little-endian, native on x64) */
static void ckpt_write_u32(uint8_t **pp, uint32_t val) {
    memcpy(*pp, &val, 4); *pp += 4;
}
static void ckpt_write_u64(uint8_t **pp, uint64_t val) {
    memcpy(*pp, &val, 8); *pp += 8;
}
static void ckpt_write_i32(uint8_t **pp, int32_t val) {
    memcpy(*pp, &val, 4); *pp += 4;
}

/* Binary read helpers */
static uint32_t ckpt_read_u32(const uint8_t **pp) {
    uint32_t val; memcpy(&val, *pp, 4); *pp += 4; return val;
}
static uint64_t ckpt_read_u64(const uint8_t **pp) {
    uint64_t val; memcpy(&val, *pp, 8); *pp += 8; return val;
}
static int32_t ckpt_read_i32(const uint8_t **pp) {
    int32_t val; memcpy(&val, *pp, 4); *pp += 4; return val;
}

/* Write a section header and return the size field position for back-patching */
static uint8_t *ckpt_begin_section(uint8_t **pp, uint32_t section_id) {
    ckpt_write_u32(pp, section_id);
    uint8_t *size_pos = *pp;
    ckpt_write_u32(pp, 0);    /* placeholder */
    return size_pos;
}

/* Patch the section size at the position returned by ckpt_begin_section */
static void ckpt_end_section(uint8_t **pp, uint8_t *size_pos) {
    size_t section_data_size = *pp - (size_pos + 4);
    uint32_t s = (uint32_t)section_data_size;
    memcpy(size_pos, &s, 4);
}

/* Write a qsim_value_t compactly: 4 bytes state + 1 byte strength = 5 bytes */
static void ckpt_write_value(uint8_t **pp, qsim_value_t v) {
    uint32_t s = (uint32_t)v.state;
    ckpt_write_u32(pp, s);
    (*pp)[0] = v.strength; *pp += 1;
}

static qsim_value_t ckpt_read_value(const uint8_t **pp) {
    qsim_value_t v;
    v.state = (qsim_logic_state_t)ckpt_read_u32(pp);
    v.strength = (*pp)[0]; *pp += 1;
    return v;
}

/* Write a bit vector: width + values */
static void ckpt_write_bv(uint8_t **pp, const qsim_bit_vector_t *bv) {
    if (!bv) { ckpt_write_u32(pp, 0); return; }
    ckpt_write_u32(pp, bv->width);
    for (uint32_t i = 0; i < bv->width; i++)
        ckpt_write_value(pp, bv->bits[i]);
}

/* Read a bit vector (allocates). Caller must free with qsim_bit_vector_free. */
static qsim_bit_vector_t *ckpt_read_bv(const uint8_t **pp) {
    uint32_t w = ckpt_read_u32(pp);
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(w);
    if (!bv) return NULL;
    for (uint32_t i = 0; i < w; i++)
        bv->bits[i] = ckpt_read_value(pp);
    return bv;
}

/* Write a coverage entry: file string + line */
static void ckpt_write_coverage(uint8_t **pp, const coverage_entry_t *e) {
    size_t flen = e->file ? strlen(e->file) : 0;
    ckpt_write_u32(pp, (uint32_t)flen);
    if (flen > 0) memcpy(*pp, e->file, flen);
    *pp += flen;
    ckpt_write_u32(pp, e->line);
}

/* Read a coverage entry (allocates file string). Returns 0 on failure. */
static int ckpt_read_coverage(const uint8_t **pp, coverage_entry_t *e) {
    uint32_t flen = ckpt_read_u32(pp);
    e->file = calloc(flen + 1, 1);
    if (!e->file) return 0;
    if (flen > 0) {
        memcpy(e->file, *pp, flen);
        *pp += flen;
    }
    e->file[flen] = '\0';
    e->line = ckpt_read_u32(pp);
    return 1;
}

/* ── Public API ── */

void *uir_sim_save(uir_sim_context_t *ctx, size_t *size_out) {
    if (!ctx || !size_out) return NULL;
    *size_out = 0;

    size_t est = 128 + ctx->signal_count * (4 + ctx->signal_count * 5)
                 + ctx->event_count * 64
                 + (ctx->event_count > 0 ? ctx->event_count * 1024 : 0)
                 + ctx->coverage_count * 260;
    if (est < 4096) est = 4096;

    uint8_t *buf = malloc(est);
    if (!buf) return NULL;
    uint8_t *p = buf;

    /* Header */
    memcpy(p, CKPT_MAGIC, 8); p += 8;
    ckpt_write_u32(&p, CKPT_VERSION);
    uint8_t *total_size_pos = p; p += 8;

    /* SIGNALS section */
    uint8_t *sect_size = ckpt_begin_section(&p, CKPT_SECT_SIGNALS);
    ckpt_write_u32(&p, (uint32_t)ctx->signal_count);
    for (size_t i = 0; i < ctx->signal_count; i++)
        ckpt_write_bv(&p, ctx->signals[i].value);
    ckpt_end_section(&p, sect_size);

    /* TIME section */
    sect_size = ckpt_begin_section(&p, CKPT_SECT_TIME);
    ckpt_write_u64(&p, ctx->current_time);
    ckpt_write_u32(&p, ctx->current_delta);
    ckpt_write_u64(&p, ctx->total_events);
    ckpt_write_i32(&p, ctx->initial_eval_done);
    ckpt_end_section(&p, sect_size);

    /* EVENTS section */
    sect_size = ckpt_begin_section(&p, CKPT_SECT_EVENTS);
    ckpt_write_u32(&p, (uint32_t)ctx->event_count);
    sim_event_t *ev = ctx->event_head;
    while (ev) {
        ckpt_write_u64(&p, ev->time);
        ckpt_write_u32(&p, ev->delta);
        ckpt_write_u32(&p, ev->sig_idx);
        ckpt_write_i32(&p, ev->is_nba);
        ckpt_write_bv(&p, ev->value);
        ev = ev->next;
    }
    ckpt_end_section(&p, sect_size);

    /* COVERAGE section */
    sect_size = ckpt_begin_section(&p, CKPT_SECT_COVERAGE);
    ckpt_write_u32(&p, (uint32_t)ctx->coverage_count);
    for (size_t i = 0; i < ctx->coverage_count; i++)
        ckpt_write_coverage(&p, &ctx->coverage[i]);
    ckpt_end_section(&p, sect_size);

    /* BREAKPOINT_HIT section */
    sect_size = ckpt_begin_section(&p, CKPT_SECT_BREAKPOINT_HIT);
    ckpt_write_i32(&p, ctx->breakpoint_hit);
    ckpt_end_section(&p, sect_size);

    /* Patch total size */
    size_t total = (size_t)(p - buf);
    memcpy(total_size_pos, &total, 8);

    /* Shrink to fit */
    uint8_t *smaller = realloc(buf, total);
    if (smaller) buf = smaller;

    *size_out = total;
    return buf;
}

uir_sim_context_t *uir_sim_restore(const void *data, size_t size,
                                    uir_design_unit_t **units, size_t unit_count) {
    if (!data || size < 20) return NULL;

    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + size;

    if (memcmp(p, CKPT_MAGIC, 8) != 0) return NULL;
    p += 8;

    uint32_t version = ckpt_read_u32(&p);
    (void)version;

    uint64_t total_size = ckpt_read_u64(&p);
    if (total_size != size) return NULL;

    uir_sim_context_t *ctx = uir_sim_create(units, unit_count);
    if (!ctx) return NULL;

    while (p + 8 <= end) {
        uint32_t sect_id = ckpt_read_u32(&p);
        uint32_t sect_size = ckpt_read_u32(&p);
        const uint8_t *sect_end = p + sect_size;
        if (sect_end > end) { uir_sim_destroy(ctx); return NULL; }

        switch (sect_id) {
        case CKPT_SECT_SIGNALS: {
            uint32_t sig_count = ckpt_read_u32(&p);
            if (sig_count != (uint32_t)ctx->signal_count) {
                uir_sim_destroy(ctx); return NULL;
            }
            for (size_t i = 0; i < sig_count; i++) {
                qsim_bit_vector_t *bv = ckpt_read_bv(&p);
                if (!bv || bv->width != ctx->signals[i].value->width) {
                    qsim_bit_vector_free(bv);
                    uir_sim_destroy(ctx); return NULL;
                }
                qsim_bit_vector_free(ctx->signals[i].value);
                ctx->signals[i].value = bv;
            }
            break;
        }
        case CKPT_SECT_TIME: {
            ctx->current_time = ckpt_read_u64(&p);
            ctx->current_delta = ckpt_read_u32(&p);
            ctx->total_events = ckpt_read_u64(&p);
            ctx->initial_eval_done = ckpt_read_i32(&p);
            break;
        }
        case CKPT_SECT_EVENTS: {
            uint32_t ev_count = ckpt_read_u32(&p);
            for (uint32_t i = 0; i < ev_count; i++) {
                uint64_t ev_time = ckpt_read_u64(&p);
                uint32_t ev_delta = ckpt_read_u32(&p);
                uint32_t ev_sig_idx = ckpt_read_u32(&p);
                int32_t ev_is_nba = ckpt_read_i32(&p);
                qsim_bit_vector_t *ev_val = ckpt_read_bv(&p);
                if (!ev_val) { uir_sim_destroy(ctx); return NULL; }

                sim_event_t *ev = pool_alloc_event(ctx);
                if (!ev) { qsim_bit_vector_free(ev_val); uir_sim_destroy(ctx); return NULL; }
                ev->time = ev_time;
                ev->delta = ev_delta;
                ev->sig_idx = ev_sig_idx;
                ev->is_nba = ev_is_nba;
                ev->value = ev_val;
                ev->next = NULL;

                if (!ctx->event_tail) {
                    ctx->event_head = ev;
                    ctx->event_tail = ev;
                } else {
                    ctx->event_tail->next = ev;
                    ctx->event_tail = ev;
                }
                ctx->event_count++;
            }
            break;
        }
        case CKPT_SECT_COVERAGE: {
            uint32_t cov_count = ckpt_read_u32(&p);
            for (uint32_t i = 0; i < cov_count; i++) {
                if (ctx->coverage_count >= ctx->coverage_cap) {
                    size_t nc = ctx->coverage_cap ? ctx->coverage_cap * 2 : 16;
                    coverage_entry_t *n = realloc(ctx->coverage, nc * sizeof(coverage_entry_t));
                    if (!n) { uir_sim_destroy(ctx); return NULL; }
                    ctx->coverage = n;
                    ctx->coverage_cap = nc;
                }
                if (!ckpt_read_coverage(&p, &ctx->coverage[ctx->coverage_count])) {
                    uir_sim_destroy(ctx); return NULL;
                }
                ctx->coverage_count++;
            }
            break;
        }
        case CKPT_SECT_BREAKPOINT_HIT: {
            ctx->breakpoint_hit = ckpt_read_i32(&p);
            break;
        }
        default:
            break;
        }

        p = sect_end;
    }

    /* Disconnect callbacks — caller must re-register them */
    ctx->step_cb = NULL;
    ctx->step_cb_user_data = NULL;
    ctx->event_cb = NULL;
    ctx->event_cb_user_data = NULL;

    return ctx;
}

/* ── Diff two checkpoints ── */

char *uir_sim_diff(const void *data_a, size_t size_a,
                    const void *data_b, size_t size_b,
                    uir_design_unit_t **units, size_t unit_count) {
    if (!data_a || !data_b || !units) return NULL;

    uir_sim_context_t *ctx_a = uir_sim_restore(data_a, size_a, units, unit_count);
    uir_sim_context_t *ctx_b = uir_sim_restore(data_b, size_b, units, unit_count);
    if (!ctx_a || !ctx_b) {
        uir_sim_destroy(ctx_a); uir_sim_destroy(ctx_b); return NULL;
    }

#define DIFF_BUF 65536
    char *buf = malloc(DIFF_BUF);
    if (!buf) { uir_sim_destroy(ctx_a); uir_sim_destroy(ctx_b); return NULL; }
    size_t len = 0;
    int n;

#define DIFF_APPEND(...) do { \
    n = snprintf(buf + len, DIFF_BUF - len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= DIFF_BUF - len) { free(buf); uir_sim_destroy(ctx_a); uir_sim_destroy(ctx_b); return NULL; } \
    len += (size_t)n; \
} while(0)

    DIFF_APPEND("{\"signal_count\":%zu,\"diffs\":[", ctx_a->signal_count);

    int first = 1;
    for (size_t i = 0; i < ctx_a->signal_count && i < ctx_b->signal_count; i++) {
        const qsim_bit_vector_t *va = ctx_a->signals[i].value;
        const qsim_bit_vector_t *vb = ctx_b->signals[i].value;
        int differs = 0;

        if (!va && !vb) continue;
        if (!va || !vb) { differs = 1; }
        else if (va->width != vb->width) { differs = 1; }
        else {
            for (uint32_t b = 0; b < va->width; b++) {
                if (va->bits[b].state != vb->bits[b].state) {
                    differs = 1; break;
                }
            }
        }

        if (differs) {
            if (!first) DIFF_APPEND(",");
            first = 0;

            uint32_t w = va ? va->width : (vb ? vb->width : 0);
            char str_a[1025] = "", str_b[1025] = "";
            if (va) {
                for (uint32_t b = 0; b < va->width && b < 1024; b++)
                    str_a[b] = (va->bits[b].state == QSIM_1) ? '1'
                             : (va->bits[b].state == QSIM_X) ? 'X'
                             : (va->bits[b].state == QSIM_Z) ? 'Z' : '0';
                str_a[va->width > 1024 ? 1024 : va->width] = '\0';
            }
            if (vb) {
                for (uint32_t b = 0; b < vb->width && b < 1024; b++)
                    str_b[b] = (vb->bits[b].state == QSIM_1) ? '1'
                             : (vb->bits[b].state == QSIM_X) ? 'X'
                             : (vb->bits[b].state == QSIM_Z) ? 'Z' : '0';
                str_b[vb->width > 1024 ? 1024 : vb->width] = '\0';
            }

            DIFF_APPEND(
                "{\"signal\":\"%s\",\"width\":%u,\"old_value\":\"%s\",\"new_value\":\"%s\"}",
                ctx_a->signals[i].name, w, str_a, str_b);
        }
    }

    DIFF_APPEND("]}");

#undef DIFF_BUF
#undef DIFF_APPEND

    uir_sim_destroy(ctx_a);
    uir_sim_destroy(ctx_b);
    return buf;
}

/* ── Signal trace: find driver signals ── */

/* Flexible signal index lookup: exact match first, then suffix ".name" match */
static int find_signal_idx_flexible(uir_sim_context_t *ctx, const char *name) {
    int idx = find_signal_idx(ctx, name);
    if (idx >= 0) return idx;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < ctx->signal_count; i++) {
        const char *sig_name = ctx->signals[i].name;
        size_t sig_len = strlen(sig_name);
        if (sig_len > name_len + 1 && sig_name[sig_len - name_len - 1] == '.' &&
            strcmp(sig_name + sig_len - name_len, name) == 0)
            return (int)i;
    }
    return -1;
}

/* Collect driver signal indices for a given target signal index.
 * Scans continuous assigns and process bodies.
 * Returns number of drivers found (up to max_drivers). */
static size_t collect_drivers(uir_sim_context_t *ctx, int target_idx,
                               int *drivers, size_t max_drivers) {
    size_t count = 0;

    /* Check continuous assigns */
    for (size_t i = 0; i < ctx->cont_assign_count && count < max_drivers; i++) {
        cont_assign_entry_t *ca = &ctx->cont_assigns[i];
        uir_assign_t *a = ca->assign;
        if (!a->lhs || a->lhs->kind != UIR_REF) continue;
        uir_ref_t *ref = (uir_ref_t *)a->lhs;
        int lhs_idx = find_signal_idx_flexible(ctx, ref->name);
        if (lhs_idx == target_idx) {
            for (size_t d = 0; d < ca->dep_count && count < max_drivers; d++) {
                int dep = ca->dep_sigs[d];
                int found = 0;
                for (size_t j = 0; j < count; j++)
                    if (drivers[j] == dep) { found = 1; break; }
                if (!found) drivers[count++] = dep;
            }
        }
    }

    /* Check process bodies for ASSIGN nodes that write to target */
    for (size_t p = 0; p < ctx->process_count && count < max_drivers; p++) {
        uir_process_t *proc = ctx->processes[p];
        if (!proc || !proc->body) continue;

        /* Walk the body looking for ASSIGN nodes */
        /* Use a simple stack-based walk to avoid recursion depth issues */
        uir_node_t *walk_stack[256];
        int walk_sp = 0;
        uir_node_t *cur;

        /* Push the process body */
        if (proc->body) walk_stack[walk_sp++] = proc->body;

        while (walk_sp > 0 && count < max_drivers) {
            cur = walk_stack[--walk_sp];

            if (!cur) continue;

            if (cur->kind == UIR_ASSIGN) {
                uir_assign_t *as = (uir_assign_t *)cur;
                if (as->lhs && as->lhs->kind == UIR_REF) {
                    uir_ref_t *lref = (uir_ref_t *)as->lhs;
                    int lhs_idx = find_signal_idx_flexible(ctx, lref->name);
                    if (lhs_idx == target_idx && as->rhs) {
                        /* Collect signals from the RHS */
                        int *rhs_sigs = NULL;
                        size_t rhs_count = 0, rhs_cap = 0;
                        find_refs_in_node(as->rhs, ctx, &rhs_sigs, &rhs_count, &rhs_cap);
                        for (size_t r = 0; r < rhs_count && count < max_drivers; r++) {
                            int found = 0;
                            for (size_t j = 0; j < count; j++)
                                if (drivers[j] == rhs_sigs[r]) { found = 1; break; }
                            if (!found) drivers[count++] = rhs_sigs[r];
                        }
                        free(rhs_sigs);
                    }
                }
            } else if (cur->kind == UIR_BLOCK) {
                uir_block_t *blk = (uir_block_t *)cur;
                for (int si = (int)blk->stmt_count - 1; si >= 0 && walk_sp < 256; si--)
                    walk_stack[walk_sp++] = blk->stmts[si];
            } else if (cur->kind == UIR_IF) {
                uir_if_t *ifn = (uir_if_t *)cur;
                if (ifn->then_branch) walk_stack[walk_sp++] = ifn->then_branch;
                if (ifn->else_branch) walk_stack[walk_sp++] = ifn->else_branch;
            } else if (cur->kind == UIR_CASE) {
                uir_case_t *case_node = (uir_case_t *)cur;
                for (size_t ci = 0; ci < case_node->item_count && walk_sp < 256; ci++)
                    if (case_node->items[ci])
                        walk_stack[walk_sp++] = (uir_node_t *)case_node->items[ci];
            } else if (cur->kind == UIR_CASE_ITEM) {
                uir_case_item_t *item = (uir_case_item_t *)cur;
                if (item->body) walk_stack[walk_sp++] = item->body;
            } else if (cur->kind == UIR_LOOP) {
                uir_loop_t *loop = (uir_loop_t *)cur;
                if (loop->init_stmt) walk_stack[walk_sp++] = loop->init_stmt;
                if (loop->body) walk_stack[walk_sp++] = loop->body;
            } else if (cur->kind == UIR_LOOP_BACK) {
                uir_loop_back_t *lb = (uir_loop_back_t *)cur;
                if (lb->condition) walk_stack[walk_sp++] = lb->condition;
                if (lb->body) walk_stack[walk_sp++] = lb->body;
            }
        }
    }

    return count;
}

/* Recursive trace: emit JSON for target signal and its drivers */
static int trace_emit(uir_sim_context_t *ctx, int sig_idx, size_t depth,
                       size_t max_depth, char *buf, size_t cap,
                       size_t *len, int *first) {
    int n;
#define TRACE_APPEND(...) do { \
    n = snprintf(buf + *len, cap - *len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= cap - *len) return 0; \
    *len += (size_t)n; \
} while(0)

    if (sig_idx < 0 || (size_t)sig_idx >= ctx->signal_count) return 1;

    if (!(*first)) TRACE_APPEND(",");
    *first = 0;

    TRACE_APPEND("{\"name\":\"%s\",\"width\":%u",
                 ctx->signals[sig_idx].name,
                 ctx->signals[sig_idx].value ? ctx->signals[sig_idx].value->width : 0);

    if (depth < max_depth) {
        int drivers[1024];
        size_t dcount = collect_drivers(ctx, sig_idx, drivers, 1024);

        if (dcount > 0) {
            TRACE_APPEND(",\"drivers\":[");
            int inner_first = 1;
            for (size_t i = 0; i < dcount; i++) {
                if (!trace_emit(ctx, drivers[i], depth + 1, max_depth,
                                buf, cap, len, &inner_first))
                    return 0;
            }
            TRACE_APPEND("]");
        }
    }

    TRACE_APPEND("}");
    return 1;

#undef TRACE_APPEND
}

char *uir_sim_trace_drivers(uir_sim_context_t *ctx, const char *signal,
                             size_t max_depth) {
    if (!ctx || !signal) return NULL;

    int target_idx = find_signal_idx_flexible(ctx, signal);
    if (target_idx < 0) return NULL;

#define TRACE_BUF 65536
    char *buf = malloc(TRACE_BUF);
    if (!buf) return NULL;
    size_t len = 0;
    int first = 1;

    if (!trace_emit(ctx, target_idx, 0, max_depth, buf, TRACE_BUF, &len, &first)) {
        free(buf);
        return NULL;
    }

    /* Wrap in top-level JSON if it's a single signal (not already wrapped) */
    /* trace_emit outputs just the object, so wrap it */
    char *result = malloc(len + 40);
    if (!result) { free(buf); return NULL; }
    snprintf(result, len + 40, "{\"signal\":%s}", buf);
    free(buf);

    return result;
}

int uir_sim_is_stopped(uir_sim_context_t *ctx) {
    return ctx ? ctx->stop_requested : 0;
}

int uir_sim_is_finished(uir_sim_context_t *ctx) {
    return ctx ? ctx->finish_requested : 0;
}

void uir_sim_clear_stop(uir_sim_context_t *ctx) {
    if (ctx) ctx->stop_requested = 0;
}
// force rebuild
