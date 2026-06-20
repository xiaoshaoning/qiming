#include "libqsim/session.h"
#include "libqsim/uir.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"
#include "libqsim/verilog_parser.h"
#include "libqsim/verilog_preprocessor.h"
#include "libqsim/vhdl_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Session state ── */

typedef enum {
    SESSION_CREATED,
    SESSION_COMPILED,
    SESSION_ELABORATED,
    SESSION_SIMULATING,
    SESSION_STOPPED,
    SESSION_FINISHED,
} session_state_t;

typedef struct {
    char *signal_name;
    uint64_t time_fs;
    qsim_bit_vector_t value;
} wave_entry_t;

typedef struct {
    char *file;
    uint32_t line;
} breakpoint_t;

typedef struct {
    char *file;
    uint32_t line;
} coverage_entry_t;

typedef struct {
    char *name;   /* checkpoint identifier */
    void *data;   /* serialized blob */
    size_t size;
} checkpoint_entry_t;

struct qsim_session {
    session_state_t state;

    /* Compiled design units */
    uir_design_unit_t **units;
    size_t unit_count;
    size_t unit_cap;

    /* Verilog preprocessor include paths */
    char **include_paths;
    int include_path_count;
    int include_path_cap;

    /* Elaboration */
    uir_elab_result_t *elab;

    /* Simulation */
    uir_sim_context_t *sim;

    /* Structured diagnostics from last failed compile/elaborate */
    qsim_diagnostic_t *last_diags;
    size_t last_diag_count;

    /* Log buffer */
    char *log;
    size_t log_len;
    size_t log_cap;

    /* Wave buffer */
    wave_entry_t *waves;
    size_t wave_count;
    size_t wave_cap;

    /* Breakpoints */
    breakpoint_t *breakpoints;
    size_t breakpoint_count;
    size_t breakpoint_cap;

    /* Coverage */
    coverage_entry_t *coverage;
    size_t coverage_count;
    size_t coverage_cap;

    /* Checkpoints */
    checkpoint_entry_t *checkpoints;
    size_t checkpoint_count;
    size_t checkpoint_cap;

    /* UART capture */
    char *uart_output;
    size_t uart_output_len;
    size_t uart_output_cap;
    int uart_tx_valid_idx;  /* signal index, -1 if not present */
    int uart_tx_data_idx;   /* signal index, -1 if not present */
    int uart_prev_valid;    /* previous uart_tx_valid (rising edge detection) */
};

/* ── Language detection ── */

static int has_vhdl_ext(const char *filename) {
    if (!filename) return 0;
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return (strcmp(dot, ".vhd") == 0) || (strcmp(dot, ".vhdl") == 0);
}

static int is_vhdl_src(const char *src) {
    if (!src) return 0;
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
        src++;
    return (strncmp(src, "entity", 6) == 0) ||
           (strncmp(src, "architecture", 12) == 0) ||
           (strncmp(src, "library", 7) == 0);
}

/* ── Diagnostic storage helpers ── */

static void clear_last_diags(qsim_session_t *sess) {
    if (!sess->last_diags) return;
    for (size_t i = 0; i < sess->last_diag_count; i++) {
        free((void *)sess->last_diags[i].file);
        free((void *)sess->last_diags[i].message);
        if (sess->last_diags[i].recovery) {
            qsim_recovery_t *rec = sess->last_diags[i].recovery;
            for (size_t j = 0; j < rec->suggestion_count; j++)
                free(rec->suggestions[j]);
            free(rec->suggestions);
            for (size_t j = 0; j < rec->nearby_count; j++)
                free(rec->nearby[j]);
            free(rec->nearby);
            free(rec);
        }
    }
    free(sess->last_diags);
    sess->last_diags = NULL;
    sess->last_diag_count = 0;
}

static void store_diag(qsim_session_t *sess, int is_error, const char *file,
                        uint32_t line, uint32_t column, const char *message,
                        qsim_recovery_t *rec)
{
    qsim_diagnostic_t *new_diags = realloc(sess->last_diags,
        (sess->last_diag_count + 1) * sizeof(qsim_diagnostic_t));
    if (!new_diags) {
        /* cleanup rec since caller transfers ownership */
        if (rec) {
            for (size_t j = 0; j < rec->suggestion_count; j++) free(rec->suggestions[j]);
            free(rec->suggestions);
            for (size_t j = 0; j < rec->nearby_count; j++) free(rec->nearby[j]);
            free(rec->nearby);
            free(rec);
        }
        return;
    }
    sess->last_diags = new_diags;
    qsim_diagnostic_t *d = &sess->last_diags[sess->last_diag_count++];
    d->is_error = is_error;
    d->file = file ? strdup(file) : NULL;
    d->line = line;
    d->column = column;
    d->message = message ? strdup(message) : NULL;
    d->recovery = rec; /* takes ownership */
}

static void store_parse_diag(qsim_session_t *sess, const char *file,
                              uint32_t line, uint32_t column, const char *message)
{
    store_diag(sess, 1, file, line, column, message, NULL);
}

static void store_elab_diagnostics(qsim_session_t *sess, uir_elab_result_t *elab) {
    if (!elab) return;
    for (size_t i = 0; i < elab->diag_count; i++) {
        qsim_recovery_t *rec = (elab->recoveries && elab->recoveries[i])
            ? elab->recoveries[i] : NULL;
        if (rec) elab->recoveries[i] = NULL; /* transfer ownership */
        store_diag(sess, 1, NULL, 0, 0, elab->diagnostics[i], rec);
    }
}

/* ── Log buffer helpers ── */

static void log_append(qsim_session_t *sess, const char *msg) {
    if (!sess || !msg) return;
    size_t n = strlen(msg);
    if (sess->log_len + n + 1 > sess->log_cap) {
        size_t nc = sess->log_cap ? sess->log_cap * 2 : 4096;
        while (sess->log_len + n + 1 > nc) nc *= 2;
        char *nl = realloc(sess->log, nc);
        if (!nl) return;
        sess->log = nl;
        sess->log_cap = nc;
    }
    memcpy(sess->log + sess->log_len, msg, n);
    sess->log_len += n;
    sess->log[sess->log_len] = '\0';
}

/* ── Wave buffer helpers ── */

static void wave_append(qsim_session_t *sess, const char *signal_name,
                         uint64_t time_fs, const qsim_bit_vector_t *value) {
    if (!sess || !signal_name || !value) return;
    if (sess->wave_count >= sess->wave_cap) {
        size_t nc = sess->wave_cap ? sess->wave_cap * 2 : 4096;
        wave_entry_t *nw = realloc(sess->waves, nc * sizeof(wave_entry_t));
        if (!nw) return;
        sess->waves = nw;
        sess->wave_cap = nc;
    }
    wave_entry_t *e = &sess->waves[sess->wave_count++];
    e->signal_name = strdup(signal_name);
    e->time_fs = time_fs;
    e->value.width = value->width;
    e->value.bits = malloc(value->width * sizeof(qsim_value_t));
    if (e->value.bits) {
        memcpy(e->value.bits, value->bits, value->width * sizeof(qsim_value_t));
    }
}

/* ── Event callback for wave accumulation ── */

static void on_signal_change(uir_sim_context_t *ctx,
                              const char *signal_name,
                              const qsim_bit_vector_t *new_value,
                              void *user_data) {
    (void)ctx;
    qsim_session_t *sess = (qsim_session_t *)user_data;
    if (!sess) return;
    wave_append(sess, signal_name, sess->sim ? uir_sim_current_time(sess->sim) : 0, new_value);
}

/* ── Display callback for $display/$write/$monitor output ── */

static void on_sys_display(uir_sim_context_t *ctx, const char *msg, void *user_data) {
    (void)ctx;
    qsim_session_t *sess = (qsim_session_t *)user_data;
    log_append(sess, msg);
}

/* ── Lifecycle ── */

qsim_session_t *qsim_session_create(void) {
    qsim_session_t *sess = calloc(1, sizeof(qsim_session_t));
    if (!sess) return NULL;
    sess->state = SESSION_CREATED;
    sess->uart_tx_valid_idx = -1;
    sess->uart_tx_data_idx = -1;
    return sess;
}

void qsim_session_free(qsim_session_t *sess) {
    if (!sess) return;

    if (sess->sim) uir_sim_destroy(sess->sim);
    if (sess->elab) uir_elab_result_free(sess->elab);
    for (size_t i = 0; i < sess->unit_count; i++)
        uir_destroy_design_unit(sess->units[i]);
    free(sess->units);

    for (size_t i = 0; i < sess->wave_count; i++) {
        free(sess->waves[i].signal_name);
        free(sess->waves[i].value.bits);
    }
    free(sess->waves);

    for (size_t i = 0; i < sess->breakpoint_count; i++)
        free(sess->breakpoints[i].file);
    free(sess->breakpoints);

    for (size_t i = 0; i < sess->coverage_count; i++)
        free(sess->coverage[i].file);
    free(sess->coverage);

    clear_last_diags(sess);
    free(sess->uart_output);

    /* Free checkpoints */
    for (size_t i = 0; i < sess->checkpoint_count; i++) {
        free(sess->checkpoints[i].name);
        free(sess->checkpoints[i].data);
    }
    free(sess->checkpoints);

    /* Free include paths */
    for (int i = 0; i < sess->include_path_count; i++)
        free(sess->include_paths[i]);
    free(sess->include_paths);

    free(sess->log);
    free(sess);
}

/* ── Include paths for Verilog preprocessor ── */

void qsim_session_add_include_path(qsim_session_t *sess, const char *path) {
    if (!sess || !path) return;
    if (sess->include_path_count >= sess->include_path_cap) {
        int nc = sess->include_path_cap ? sess->include_path_cap * 2 : 8;
        char **np = realloc(sess->include_paths, nc * sizeof(char *));
        if (!np) return;
        sess->include_paths = np;
        sess->include_path_cap = nc;
    }
    sess->include_paths[sess->include_path_count++] = strdup(path);
}

/* Reset session state for test reuse. Call between independent test
 * runs to prevent unit accumulation when compiling new designs. */
void qsim_session_reset(qsim_session_t *sess) {
    if (!sess) return;
    if (sess->sim) { uir_sim_destroy(sess->sim); sess->sim = NULL; }
    if (sess->elab) { uir_elab_result_free(sess->elab); sess->elab = NULL; }
    for (size_t i = 0; i < sess->unit_count; i++)
        uir_destroy_design_unit(sess->units[i]);
    sess->unit_count = 0;
    for (size_t i = 0; i < sess->wave_count; i++) {
        free(sess->waves[i].signal_name);
        free(sess->waves[i].value.bits);
    }
    sess->wave_count = 0;
    sess->wave_cap = 0;
    free(sess->waves); sess->waves = NULL;
    for (size_t i = 0; i < sess->coverage_count; i++)
        free(sess->coverage[i].file);
    sess->coverage_count = 0;
    free(sess->coverage); sess->coverage = NULL;
    clear_last_diags(sess);
    sess->state = SESSION_CREATED;
}

/* ── Compilation ── */

static int add_unit(qsim_session_t *sess, uir_design_unit_t *unit) {
    if (!unit) return 0;
    if (sess->unit_count >= sess->unit_cap) {
        size_t nc = sess->unit_cap ? sess->unit_cap * 2 : 16;
        uir_design_unit_t **nu = realloc(sess->units, nc * sizeof(uir_design_unit_t *));
        if (!nu) return 0;
        sess->units = nu;
        sess->unit_cap = nc;
    }
    sess->units[sess->unit_count++] = unit;
    sess->state = SESSION_COMPILED;
    return 1;
}

int qsim_session_compile_string(qsim_session_t *sess, const char *name, const char *source) {
    if (!sess || !name || !source) return 0;

    if (is_vhdl_src(source)) {
        vhdl_parse_result_t r = vhdl_parse(name, source, strlen(source));
        if (!r.success || !r.unit) {
            log_append(sess, r.error_count > 0 ? r.errors[0].message : "VHDL parse failed");
            clear_last_diags(sess);
            if (r.error_count > 0)
                store_parse_diag(sess, name, r.errors[0].line, r.errors[0].column, r.errors[0].message);
            return 0;
        }
        return add_unit(sess, r.unit);
    } else {
        /* For Verilog inline sources, run the preprocessor so that
         * `ifdef, `define, `include, etc. are expanded.  The file-based
         * path (qsim_session_compile_file) already does this. */
        const char *src_start = source;
        size_t src_len = strlen(source);
        char *expanded = NULL;
        if (memchr(source, '`', src_len) || memchr(source, '$', src_len)) {
            verilog_preprocessor_t *pp = verilog_preprocessor_create();
            if (pp) {
                for (size_t i = 0; i < sess->include_path_count; i++)
                    verilog_preprocessor_add_include_path(pp, sess->include_paths[i]);
                expanded = verilog_preprocessor_process(pp, name, src_start, src_len);
                verilog_preprocessor_destroy(pp);
                if (expanded) {
                    source = expanded;
                    src_len = strlen(expanded);
                }
            }
        }
        parse_result_t r = verilog_parse(name, source, src_len);
        free(expanded);
        if (!r.success || r.unit_count == 0) {
            log_append(sess, r.error_count > 0 ? r.errors[0].message : "Verilog parse failed");
            clear_last_diags(sess);
            if (r.error_count > 0)
                store_parse_diag(sess, name, r.errors[0].line, r.errors[0].column, r.errors[0].message);
            if (r.error_count > 0) {
                fprintf(stderr, "PARSE ERROR at %s:%d: %s\n",
                        name, r.errors[0].line, r.errors[0].message);
            }
            return 0;
        }
        for (size_t i = 0; i < r.unit_count; i++) {
            if (!add_unit(sess, r.units[i])) {
                for (size_t j = i + 1; j < r.unit_count; j++)
                    uir_destroy_design_unit(r.units[j]);
                free(r.units);
                return 0;
            }
        }
        free(r.units);
        return 1;
    }
}

int qsim_session_compile_file(qsim_session_t *sess, const char *path) {
    if (!sess || !path) return 0;

    if (has_vhdl_ext(path)) {
        vhdl_parse_result_t r = vhdl_parse_file(path);
        if (!r.success || !r.unit) {
            log_append(sess, r.error_count > 0 ? r.errors[0].message : "VHDL parse failed");
            clear_last_diags(sess);
            if (r.error_count > 0)
                store_parse_diag(sess, path, r.errors[0].line, r.errors[0].column, r.errors[0].message);
            return 0;
        }
        return add_unit(sess, r.unit);
    } else {
        parse_result_t r = verilog_parse_file_ex(
            path,
            (const char **)sess->include_paths,
            sess->include_path_count);
        if (!r.success || r.unit_count == 0) {
            log_append(sess, r.error_count > 0 ? r.errors[0].message : "Verilog parse failed");
            clear_last_diags(sess);
            if (r.error_count > 0)
                store_parse_diag(sess, path, r.errors[0].line, r.errors[0].column, r.errors[0].message);
            return 0;
        }
        for (size_t i = 0; i < r.unit_count; i++) {
            if (!add_unit(sess, r.units[i])) {
                for (size_t j = i + 1; j < r.unit_count; j++)
                    uir_destroy_design_unit(r.units[j]);
                free(r.units);
                return 0;
            }
        }
        free(r.units);
        return 1;
    }
}

/* ── Elaboration ── */

int qsim_session_elaborate(qsim_session_t *sess) {
    if (!sess || sess->unit_count == 0) return 0;

    if (sess->elab) {
        uir_elab_result_free(sess->elab);
        sess->elab = NULL;
    }
    if (sess->sim) {
        uir_sim_destroy(sess->sim);
        sess->sim = NULL;
    }

    sess->elab = uir_elaborate(sess->units, sess->unit_count);
    if (!sess->elab || !sess->elab->success) {
        log_append(sess, sess->elab && sess->elab->diag_count > 0
                        ? sess->elab->diagnostics[0] : "elaboration failed");
        clear_last_diags(sess);
        if (sess->elab) store_elab_diagnostics(sess, sess->elab);
        sess->state = SESSION_COMPILED;
        return 0;
    }

    /* Create simulation context */
    sess->sim = uir_sim_create(sess->units, sess->unit_count);
    if (!sess->sim) {
        log_append(sess, "failed to create simulation context");
        sess->state = SESSION_COMPILED;
        return 0;
    }

    /* Install event callback for wave accumulation */
    uir_sim_set_event_callback(sess->sim, on_signal_change, sess);

    /* Install display callback for $display/$write/$monitor output */
    uir_sim_set_sys_display_callback(sess->sim, on_sys_display, sess);

    /* Resolve UART signal indices */
    sess->uart_tx_valid_idx = -1;
    sess->uart_tx_data_idx = -1;
    sess->uart_prev_valid = 0;
    int n_sigs = uir_sim_get_signal_count(sess->sim);
    for (int i = 0; i < n_sigs; i++) {
        const char *name = uir_sim_get_signal_name(sess->sim, i);
        if (name) {
            if (strcmp(name, "uart_tx_valid") == 0) sess->uart_tx_valid_idx = i;
            if (strcmp(name, "uart_tx_data") == 0) sess->uart_tx_data_idx = i;
        }
    }

    sess->state = SESSION_ELABORATED;
    return 1;
}

/* ── Event count ── */

size_t qsim_session_get_event_count(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_get_event_count(sess->sim);
}

/* ── Simulation stepping ── */

void qsim_session_set_thread_count(qsim_session_t *sess, int thread_count) {
    if (sess && sess->sim && thread_count >= 1)
        uir_sim_set_thread_count(sess->sim, thread_count);
}

int qsim_session_step_delta(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;

    /* Run one step: duration=0 processes all events at current time */
    int ran = uir_sim_run(sess->sim, 0);

    /* Check sim flags after stepping */
    if (uir_sim_is_finished(sess->sim)) {
        sess->state = SESSION_FINISHED;
    } else if (uir_sim_is_stopped(sess->sim)) {
        sess->state = SESSION_STOPPED;
    } else {
        sess->state = SESSION_SIMULATING;
    }

    /* UART capture: rising-edge detect on uart_tx_valid */
    if (sess->uart_tx_valid_idx >= 0 && sess->uart_tx_data_idx >= 0) {
        const qsim_bit_vector_t *v = uir_sim_get_signal_value(sess->sim, sess->uart_tx_valid_idx);
        if (v && v->width == 1) {
            int valid_now = (v->bits[0].state == QSIM_1);
            if (valid_now && !sess->uart_prev_valid) {
                const qsim_bit_vector_t *d = uir_sim_get_signal_value(sess->sim, sess->uart_tx_data_idx);
                if (d && d->width >= 8) {
                    char c = 0;
                    for (int b = 0; b < 8 && b < (int)d->width; b++)
                        if (d->bits[b].state == QSIM_1) c |= (1u << b);
                    /* Append to UART output buffer */
                    if (sess->uart_output_len + 2 > sess->uart_output_cap) {
                        size_t nc = sess->uart_output_cap ? sess->uart_output_cap * 2 : 256;
                        char *nl = realloc(sess->uart_output, nc);
                        if (nl) {
                            sess->uart_output = nl;
                            sess->uart_output_cap = nc;
                        }
                    }
                    if (sess->uart_output_len + 1 < sess->uart_output_cap) {
                        sess->uart_output[sess->uart_output_len++] = c;
                        sess->uart_output[sess->uart_output_len] = '\0';
                    }
                }
            }
            sess->uart_prev_valid = valid_now;
        }
    }

    return ran;
}

uint64_t qsim_session_step_time(qsim_session_t *sess, uint64_t time_fs) {
    if (!sess || !sess->sim) return 0;
    uint64_t start = uir_sim_current_time(sess->sim);
    uir_sim_run(sess->sim, time_fs);
    uint64_t elapsed = uir_sim_current_time(sess->sim) - start;

    /* Check sim flags after stepping */
    if (uir_sim_is_finished(sess->sim)) {
        sess->state = SESSION_FINISHED;
    } else if (uir_sim_is_stopped(sess->sim)) {
        sess->state = SESSION_STOPPED;
    } else {
        sess->state = SESSION_SIMULATING;
    }

    return elapsed;
}

/* ── Signal access ── */

int qsim_session_get_signal_count(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_get_signal_count(sess->sim);
}

const char *qsim_session_get_signal_name(qsim_session_t *sess, int idx) {
    if (!sess || !sess->sim) return NULL;
    return uir_sim_get_signal_name(sess->sim, idx);
}

qsim_bit_vector_t qsim_session_get_signal_value(qsim_session_t *sess, int idx) {
    qsim_bit_vector_t zero = {0, NULL};
    if (!sess || !sess->sim) return zero;
    const qsim_bit_vector_t *v = uir_sim_get_signal_value(sess->sim, idx);
    if (!v) return zero;
    qsim_bit_vector_t copy;
    copy.width = v->width;
    copy.bits = malloc(v->width * sizeof(qsim_value_t));
    if (copy.bits && v->bits)
        memcpy(copy.bits, v->bits, v->width * sizeof(qsim_value_t));
    return copy;
}

qsim_bit_vector_t qsim_session_get_signal_by_name(qsim_session_t *sess, const char *hier_path) {
    qsim_bit_vector_t zero = {0, NULL};
    if (!sess || !sess->sim || !hier_path) return zero;
    const qsim_bit_vector_t *v = uir_sim_get_signal(sess->sim, hier_path);
    if (!v) return zero;
    qsim_bit_vector_t copy;
    copy.width = v->width;
    copy.bits = malloc(v->width * sizeof(qsim_value_t));
    if (copy.bits && v->bits)
        memcpy(copy.bits, v->bits, v->width * sizeof(qsim_value_t));
    return copy;
}

/* ── Interactive debug ── */

qsim_bit_vector_t qsim_session_eval(qsim_session_t *sess, const char *signal) {
    return qsim_session_get_signal_by_name(sess, signal);
}

int qsim_session_force(qsim_session_t *sess, const char *signal,
                        const qsim_bit_vector_t *value) {
    if (!sess || !sess->sim || !signal || !value) return 0;
    return uir_sim_force_signal(sess->sim, signal, value);
}

int qsim_session_release(qsim_session_t *sess, const char *signal) {
    if (!sess || !sess->sim || !signal) return 0;
    return uir_sim_release_signal(sess->sim, signal);
}

int qsim_session_load_mem(qsim_session_t *sess, const char *signal,
                          uint32_t addr, const uint32_t *data, size_t count) {
    if (!sess || !sess->sim || !signal || !data) return 0;
    /* Run initial blocks first so load_mem data isn't overwritten */
    uir_sim_ensure_initialized(sess->sim);
    return uir_sim_load_mem(sess->sim, signal, addr, data, count);
}

char *qsim_session_eval_str(qsim_session_t *sess, const char *signal) {
    if (!sess || !sess->sim || !signal) return NULL;
    qsim_bit_vector_t v = qsim_session_eval(sess, signal);
    if (!v.bits) return NULL;

    /* Convert to string: 0, 1, X, Z */
    char *str = malloc(v.width + 1);
    if (!str) { free(v.bits); return NULL; }
    for (uint32_t i = 0; i < v.width; i++) {
        qsim_value_t bit = qsim_bit_get(&v, i);
        str[i] = (bit.state == QSIM_1) ? '1' :
                 (bit.state == QSIM_X) ? 'X' :
                 (bit.state == QSIM_Z) ? 'Z' :
                 (bit.state == QSIM_U) ? 'U' :
                 (bit.state == QSIM_W) ? 'W' :
                 (bit.state == QSIM_L) ? 'L' :
                 (bit.state == QSIM_H) ? 'H' :
                 (bit.state == QSIM_DC) ? '-' : '0';
    }
    str[v.width] = '\0';
    free(v.bits);
    return str;
}

/* Convert a hex character to its 4-bit value */
static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

/* Parse a value string into a bit vector.
 * Handles Verilog literal format: [width]'[s]?[bodh]<digits>
 * Also accepts bare strings like "0101" (treated as binary, LSB-first).
 * For formats with a width prefix, the returned vector has that width;
 * otherwise the width is the number of digit characters. */
static qsim_bit_vector_t *parse_value_str(const char *value_str) {
    if (!value_str || !*value_str) return NULL;

    uint32_t width = 0;
    int base = 2;
    const char *digits = value_str;
    size_t digit_len;

    const char *apos = strchr(value_str, '\'');
    if (apos) {
        if (apos > value_str) {
            char wbuf[32];
            size_t wlen = (size_t)(apos - value_str);
            if (wlen >= sizeof(wbuf)) wlen = sizeof(wbuf) - 1;
            memcpy(wbuf, value_str, wlen);
            wbuf[wlen] = '\0';
            width = (uint32_t)atoi(wbuf);
        }
        apos++;
        if (*apos == 's' || *apos == 'S') apos++;
        switch (*apos) {
            case 'b': case 'B': base = 2; apos++; break;
            case 'o': case 'O': base = 8; apos++; break;
            case 'd': case 'D': base = 10; apos++; break;
            case 'h': case 'H': base = 16; apos++; break;
            default: base = 2; break;
        }
        digits = apos;
    }

    digit_len = strlen(digits);
    if (width == 0) {
        if (base == 16) width = (uint32_t)(digit_len * 4);
        else width = (uint32_t)digit_len;
    }
    if (width == 0) return NULL;

    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(width);
    if (!bv) return NULL;

    for (uint32_t i = 0; i < width; i++)
        bv->bits[i] = QSIM_VAL_0;

    if (base == 16) {
        /* Hex: each digit → 4 bits, processed from rightmost digit */
        for (size_t di = 0; di < digit_len; di++) {
            int hv = hex_char_val(digits[digit_len - 1 - di]);
            for (int b = 0; b < 4; b++) {
                uint32_t bit_idx = di * 4 + (uint32_t)b;
                if (bit_idx < width)
                    bv->bits[bit_idx].state = ((hv >> b) & 1) ? QSIM_1 : QSIM_0;
            }
        }
    } else {
        /* Binary (or octal/dec, treated as binary per-digit): LSB-first */
        for (size_t di = 0; di < digit_len && di < width; di++) {
            char c = digits[digit_len - 1 - di];
            bv->bits[di].state = (c == '1') ? QSIM_1
                               : (c == 'X' || c == 'x') ? QSIM_X
                               : (c == 'Z' || c == 'z') ? QSIM_Z
                               : QSIM_0;
        }
    }

    return bv;
}

int qsim_session_force_str(qsim_session_t *sess, const char *signal, const char *value_str) {
    if (!sess || !sess->sim || !signal || !value_str) return 0;
    qsim_bit_vector_t *bv = parse_value_str(value_str);
    if (!bv) return 0;
    int ret = uir_sim_force_signal(sess->sim, signal, bv);
    qsim_bit_vector_free(bv);
    return ret;
}

int qsim_session_set_str(qsim_session_t *sess, const char *signal, const char *value_str) {
    if (!sess || !sess->sim || !signal || !value_str) return 0;
    qsim_bit_vector_t *bv = parse_value_str(value_str);
    if (!bv) return 0;
    int ret = uir_sim_set_signal(sess->sim, signal, bv);
    qsim_bit_vector_free(bv);
    return ret;
}

void qsim_session_free_str(char *s) {
    free(s);
}

/* ── Bulk evaluation ── */

qsim_eval_multi_result_t *qsim_session_eval_multi(qsim_session_t *sess,
                                                    const char **signals,
                                                    size_t count)
{
    if (!sess || !signals || count == 0) return NULL;

    qsim_eval_multi_result_t *result = calloc(1, sizeof(qsim_eval_multi_result_t));
    if (!result) return NULL;

    result->values = calloc(count, sizeof(char *));
    if (!result->values) { free(result); return NULL; }

    result->count = count;
    for (size_t i = 0; i < count; i++) {
        if (signals[i])
            result->values[i] = qsim_session_eval_str(sess, signals[i]);
        else
            result->values[i] = NULL;
    }
    return result;
}

void qsim_eval_multi_result_free(qsim_eval_multi_result_t *result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; i++)
        free(result->values[i]);
    free(result->values);
    free(result);
}

/* ── Debug trace ── */

qsim_debug_trace_result_t *qsim_session_debug_trace(qsim_session_t *sess,
                                                      const char **signals,
                                                      size_t signal_count,
                                                      size_t cycles)
{
    if (!sess || !sess->sim || !signals || signal_count == 0 || cycles == 0)
        return NULL;

    size_t total = cycles * signal_count;
    qsim_debug_trace_result_t *result = calloc(1, sizeof(qsim_debug_trace_result_t));
    if (!result) return NULL;

    result->values = calloc(total, sizeof(char *));
    if (!result->values) { free(result); return NULL; }
    result->cycle_count = cycles;
    result->signal_count = signal_count;

    for (size_t c = 0; c < cycles; c++) {
        /* Toggle clock: falling then rising edge */
        qsim_session_set_str(sess, "clk", "0");
        qsim_session_step_delta(sess);
        qsim_session_set_str(sess, "clk", "1");
        qsim_session_step_delta(sess);

        /* Record all watched signals at posedge */
        for (size_t s = 0; s < signal_count; s++) {
            if (signals[s])
                result->values[c * signal_count + s] = qsim_session_eval_str(sess, signals[s]);
        }
    }

    return result;
}

void qsim_debug_trace_result_free(qsim_debug_trace_result_t *result) {
    if (!result) return;
    for (size_t i = 0; i < result->cycle_count * result->signal_count; i++)
        free(result->values[i]);
    free(result->values);
    free(result);
}

/* ── Wave buffer ── */

size_t qsim_session_get_wave_count(qsim_session_t *sess) {
    return sess ? sess->wave_count : 0;
}

int qsim_session_get_wave(qsim_session_t *sess, size_t idx,
                           const char **signal, uint64_t *time_fs,
                           qsim_bit_vector_t *value) {
    if (!sess || idx >= sess->wave_count || !signal || !time_fs || !value)
        return 0;
    *signal = sess->waves[idx].signal_name;
    *time_fs = sess->waves[idx].time_fs;
    value->width = sess->waves[idx].value.width;
    value->bits = malloc(value->width * sizeof(qsim_value_t));
    if (value->bits)
        memcpy(value->bits, sess->waves[idx].value.bits,
               value->width * sizeof(qsim_value_t));
    return 1;
}

void qsim_session_clear_wave(qsim_session_t *sess) {
    if (!sess) return;
    for (size_t i = 0; i < sess->wave_count; i++) {
        free(sess->waves[i].signal_name);
        free(sess->waves[i].value.bits);
    }
    sess->wave_count = 0;
}

/* ── Bulk wave query ── */

/* Comparison for qsort — used by both bulk query and VCD export */
static int wave_sort_by_time(const void *a, const void *b) {
    const wave_entry_t *wa = (const wave_entry_t *)a;
    const wave_entry_t *wb = (const wave_entry_t *)b;
    if (wa->time_fs < wb->time_fs) return -1;
    if (wa->time_fs > wb->time_fs) return 1;
    return 0;
}

qsim_wave_bulk_result_t *qsim_session_query_wave_bulk(
    qsim_session_t *sess,
    const char **signals, size_t signal_count,
    uint64_t t_start, uint64_t t_end)
{
    if (!sess || sess->wave_count == 0) return NULL;

    /* Count matching entries */
    size_t match_count = 0;
    size_t *match_idx = NULL;
    size_t match_cap = 0;

    for (size_t i = 0; i < sess->wave_count; i++) {
        wave_entry_t *w = &sess->waves[i];

        /* Filter by time window */
        if (t_start > 0 && w->time_fs < t_start) continue;
        if (t_end > 0 && w->time_fs >= t_end) continue;

        /* Filter by signal names */
        if (signals && signal_count > 0) {
            int matched = 0;
            for (size_t s = 0; s < signal_count; s++) {
                if (signals[s] && strcmp(w->signal_name, signals[s]) == 0) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) continue;
        }

        if (match_count >= match_cap) {
            size_t nc = match_cap ? match_cap * 2 : 1024;
            size_t *n = realloc(match_idx, nc * sizeof(size_t));
            if (!n) { free(match_idx); return NULL; }
            match_idx = n;
            match_cap = nc;
        }
        match_idx[match_count++] = i;
    }

    if (match_count == 0) {
        free(match_idx);
        qsim_wave_bulk_result_t *empty = calloc(1, sizeof(qsim_wave_bulk_result_t));
        if (empty) empty->count = 0;
        return empty;
    }

    /* Build temporary sorted array */
    wave_entry_t *sorted = malloc(match_count * sizeof(wave_entry_t));
    if (!sorted) { free(match_idx); return NULL; }
    for (size_t i = 0; i < match_count; i++)
        sorted[i] = sess->waves[match_idx[i]];
    free(match_idx);

    qsort(sorted, match_count, sizeof(wave_entry_t), wave_sort_by_time);

    /* Allocate result */
    qsim_wave_bulk_result_t *result = malloc(sizeof(qsim_wave_bulk_result_t));
    if (!result) { free(sorted); return NULL; }
    result->count = match_count;
    result->signals = calloc(match_count, sizeof(char *));
    result->times = calloc(match_count, sizeof(uint64_t));
    result->values = calloc(match_count, sizeof(char *));
    if (!result->signals || !result->times || !result->values) {
        free(result->signals); free(result->times); free(result->values);
        free(result); free(sorted); return NULL;
    }

    for (size_t i = 0; i < match_count; i++) {
        result->signals[i] = strdup(sorted[i].signal_name);
        result->times[i] = sorted[i].time_fs;
        if (sorted[i].value.bits) {
            char *str = malloc(sorted[i].value.width + 1);
            if (str) {
                for (uint32_t b = 0; b < sorted[i].value.width; b++) {
                    str[b] = (sorted[i].value.bits[b].state == QSIM_1) ? '1'
                           : (sorted[i].value.bits[b].state == QSIM_X) ? 'X'
                           : (sorted[i].value.bits[b].state == QSIM_Z) ? 'Z' : '0';
                }
                str[sorted[i].value.width] = '\0';
                result->values[i] = str;
            }
        } else {
            result->values[i] = strdup("");
        }
    }

    free(sorted);
    return result;
}

void qsim_wave_bulk_result_free(qsim_wave_bulk_result_t *result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; i++) {
        free(result->signals[i]);
        free(result->values[i]);
    }
    free(result->signals);
    free(result->times);
    free(result->values);
    free(result);
}

/* ── Log buffer ── */

const char *qsim_session_get_log(qsim_session_t *sess) {
    return sess ? sess->log : NULL;
}

/* ── Diagnostics ── */

const qsim_diagnostic_t *qsim_session_get_last_diagnostics(qsim_session_t *sess, size_t *count) {
    if (!sess || !count) return NULL;
    *count = sess->last_diag_count;
    return sess->last_diags;
}

/* ── UART capture ── */

const char *qsim_session_get_uart_output(qsim_session_t *sess) {
    return sess ? sess->uart_output : NULL;
}

void qsim_session_clear_uart_output(qsim_session_t *sess) {
    if (sess) {
        sess->uart_output_len = 0;
        if (sess->uart_output)
            sess->uart_output[0] = '\0';
    }
}

/* ── Checkpoints (save/restore simulator state) ── */

/* Helper: find checkpoint by name */
static checkpoint_entry_t *find_checkpoint(qsim_session_t *sess, const char *name) {
    if (!sess || !name) return NULL;
    for (size_t i = 0; i < sess->checkpoint_count; i++) {
        if (sess->checkpoints[i].name && strcmp(sess->checkpoints[i].name, name) == 0)
            return &sess->checkpoints[i];
    }
    return NULL;
}

char *qsim_session_save_checkpoint(qsim_session_t *sess, const char *name) {
    if (!sess || !name) return NULL;
    if (!sess->sim) return NULL;  /* must be elaborated/simulating */

    /* Serialize sim context */
    size_t blob_size = 0;
    void *blob = uir_sim_save(sess->sim, &blob_size);
    if (!blob) return NULL;

    /* Remove existing checkpoint with same name */
    checkpoint_entry_t *existing = find_checkpoint(sess, name);
    if (existing) {
        free(existing->data);
        existing->data = blob;
        existing->size = blob_size;
        return strdup(name);  /* name already in use, return dup'd id */
    }

    /* Add new checkpoint */
    if (sess->checkpoint_count >= sess->checkpoint_cap) {
        size_t nc = sess->checkpoint_cap ? sess->checkpoint_cap * 2 : 8;
        checkpoint_entry_t *n = realloc(sess->checkpoints, nc * sizeof(checkpoint_entry_t));
        if (!n) { free(blob); return NULL; }
        sess->checkpoints = n;
        sess->checkpoint_cap = nc;
    }

    sess->checkpoints[sess->checkpoint_count].name = strdup(name);
    sess->checkpoints[sess->checkpoint_count].data = blob;
    sess->checkpoints[sess->checkpoint_count].size = blob_size;
    sess->checkpoint_count++;

    return strdup(name);
}

int qsim_session_restore_checkpoint(qsim_session_t *sess, const char *name) {
    if (!sess || !name) return 0;

    checkpoint_entry_t *ckpt = find_checkpoint(sess, name);
    if (!ckpt) return 0;
    if (!sess->elab) return 0;

    /* Destroy current sim context */
    if (sess->sim) {
        uir_sim_destroy(sess->sim);
        sess->sim = NULL;
    }

    /* Restore from checkpoint */
    sess->sim = uir_sim_restore(ckpt->data, ckpt->size,
                                 sess->elab->units, sess->elab->unit_count);
    if (!sess->sim) return 0;

    /* Re-register wave event callback */
    uir_sim_set_event_callback(sess->sim, on_signal_change, sess);

    /* Re-register display callback */
    uir_sim_set_sys_display_callback(sess->sim, on_sys_display, sess);

    /* Re-register breakpoints with restored sim context */
    for (size_t i = 0; i < sess->breakpoint_count; i++)
        uir_sim_set_breakpoint(sess->sim, sess->breakpoints[i].file,
                                sess->breakpoints[i].line, 1);

    sess->state = SESSION_SIMULATING;
    return 1;
}

char *qsim_session_diff_checkpoint(qsim_session_t *sess,
                                    const char *name_a, const char *name_b) {
    if (!sess || !name_a || !name_b) return NULL;

    checkpoint_entry_t *a = find_checkpoint(sess, name_a);
    checkpoint_entry_t *b = find_checkpoint(sess, name_b);
    if (!a || !b || !sess->elab) return NULL;

    return uir_sim_diff(a->data, a->size, b->data, b->size,
                         sess->elab->units, sess->elab->unit_count);
}

char *qsim_session_list_checkpoints(qsim_session_t *sess) {
    if (!sess) return NULL;

#define LIST_BUF 4096
    char *buf = malloc(LIST_BUF);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;
    int n;

#define LIST_APPEND(...) do { \
    n = snprintf(buf + len, LIST_BUF - len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= LIST_BUF - len) { free(buf); return NULL; } \
    len += (size_t)n; \
} while(0)

    LIST_APPEND("{\"checkpoints\":[");
    for (size_t i = 0; i < sess->checkpoint_count; i++) {
        if (i > 0) LIST_APPEND(",");
        LIST_APPEND("{\"name\":\"%s\",\"size\":%zu}",
                   sess->checkpoints[i].name, sess->checkpoints[i].size);
    }
    LIST_APPEND("]}");

#undef LIST_BUF
#undef LIST_APPEND

    return buf;
}

/* ── Breakpoints ── */

int qsim_session_add_breakpoint(qsim_session_t *sess, const char *file, uint32_t line) {
    if (!sess || !file) return 0;

    /* Check for duplicates */
    for (size_t i = 0; i < sess->breakpoint_count; i++) {
        if (sess->breakpoints[i].line == line &&
            strcmp(sess->breakpoints[i].file, file) == 0)
            return 1; /* already exists */
    }

    if (sess->breakpoint_count >= sess->breakpoint_cap) {
        size_t nc = sess->breakpoint_cap ? sess->breakpoint_cap * 2 : 16;
        breakpoint_t *n = realloc(sess->breakpoints, nc * sizeof(breakpoint_t));
        if (!n) return 0;
        sess->breakpoints = n;
        sess->breakpoint_cap = nc;
    }

    sess->breakpoints[sess->breakpoint_count].file = strdup(file);
    sess->breakpoints[sess->breakpoint_count].line = line;
    sess->breakpoint_count++;

    /* Also register with sim context if available */
    if (sess->sim)
        uir_sim_set_breakpoint(sess->sim, file, line, 1);

    return 1;
}

int qsim_session_remove_breakpoint(qsim_session_t *sess, const char *file, uint32_t line) {
    if (!sess || !file) return 0;

    for (size_t i = 0; i < sess->breakpoint_count; i++) {
        if (sess->breakpoints[i].line == line &&
            strcmp(sess->breakpoints[i].file, file) == 0) {
            free(sess->breakpoints[i].file);
            sess->breakpoints[i] = sess->breakpoints[sess->breakpoint_count - 1];
            sess->breakpoint_count--;

            if (sess->sim)
                uir_sim_set_breakpoint(sess->sim, file, line, 0);

            return 1;
        }
    }
    return 0;
}

void qsim_session_clear_breakpoints(qsim_session_t *sess) {
    if (!sess) return;
    for (size_t i = 0; i < sess->breakpoint_count; i++)
        free(sess->breakpoints[i].file);
    sess->breakpoint_count = 0;
    if (sess->sim)
        uir_sim_clear_breakpoint_hit(sess->sim);
}

size_t qsim_session_get_breakpoint_count(qsim_session_t *sess) {
    return sess ? sess->breakpoint_count : 0;
}

int qsim_session_get_breakpoint(qsim_session_t *sess, size_t idx,
                                 const char **file, uint32_t *line) {
    if (!sess || idx >= sess->breakpoint_count || !file || !line)
        return 0;
    *file = sess->breakpoints[idx].file;
    *line = sess->breakpoints[idx].line;
    return 1;
}

/* ── Debug step callback ── */

static void on_debug_step(uir_sim_context_t *ctx,
                           const char *file, uint32_t line,
                           void *user_data) {
    (void)ctx;
    (void)line;
    qsim_session_t *sess = (qsim_session_t *)user_data;
    if (!sess || !file) return;

    /* Check if this line matches any session breakpoint */
    for (size_t i = 0; i < sess->breakpoint_count; i++) {
        if (sess->breakpoints[i].line == line &&
            strcmp(sess->breakpoints[i].file, file) == 0) {
            /* Breakpoint match — will be handled by uir_sim's own check,
             * but also store in session-level for debug_run */
            break;
        }
    }
}

/* ── Debug run ── */

int qsim_session_debug_run(qsim_session_t *sess) {
    if (!sess || !sess->sim) return -1;

    /* Install step callback to track breakpoints */
    uir_sim_set_step_callback(sess->sim, on_debug_step, sess);
    uir_sim_clear_breakpoint_hit(sess->sim);

    /* Re-register breakpoints with sim context (in case session was re-elaborated) */
    for (size_t i = 0; i < sess->breakpoint_count; i++)
        uir_sim_set_breakpoint(sess->sim, sess->breakpoints[i].file,
                                sess->breakpoints[i].line, 1);

    /* Step one delta at a time until breakpoint hit or no events */
    while (1) {
        int hit = uir_sim_breakpoint_hit(sess->sim);
        if (hit) {
            uir_sim_set_step_callback(sess->sim, NULL, NULL);
            return 1;
        }

        /* Try to step one delta */
        size_t before = 0;
        /* Use a heuristic: run with 0 duration to process init events */
        uir_sim_run(sess->sim, 0);

        /* Check if more events exist by running a tiny step */
        uint64_t t_before = uir_sim_current_time(sess->sim);
        uir_sim_run(sess->sim, 1);
        uint64_t t_after = uir_sim_current_time(sess->sim);

        if (t_after == t_before && !uir_sim_breakpoint_hit(sess->sim)) {
            /* No events processed and no breakpoint — simulation complete */
            uir_sim_set_step_callback(sess->sim, NULL, NULL);
            return 0;
        }

        /* Check again after step */
        if (uir_sim_breakpoint_hit(sess->sim)) {
            uir_sim_set_step_callback(sess->sim, NULL, NULL);
            return 1;
        }
    }
}

/* ── Line coverage ── */

size_t qsim_session_get_coverage_count(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_get_coverage_count(sess->sim);
}

int qsim_session_get_coverage_entry(qsim_session_t *sess, size_t idx,
                                     const char **file, uint32_t *line) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_get_coverage_entry(sess->sim, idx, file, line);
}

/* Count total unique source lines across all compiled design units */
static size_t count_total_source_lines(qsim_session_t *sess) {
    if (!sess || !sess->units || sess->unit_count == 0) return 0;

    /* Use a simple array of (file, line) pairs */
    size_t total = 0;
    size_t cap = 1024;
    coverage_entry_t *lines = malloc(cap * sizeof(coverage_entry_t));
    if (!lines) return 0;

    for (size_t u = 0; u < sess->unit_count; u++) {
        uir_design_unit_t *unit = sess->units[u];
        if (!unit) continue;

        /* Walk the unit's process bodies to count statements */
        for (size_t p = 0; p < unit->process_count; p++) {
            uir_process_t *proc = unit->processes[p];
            if (!proc || !proc->body) continue;

            /* Recursively count statements in the body */
            /* Use a simple recursive walk (limited depth for practical designs) */
            uir_node_t *body = proc->body;
            if (body->kind == UIR_BLOCK) {
                uir_block_t *block = (uir_block_t *)body;
                for (size_t s = 0; s < block->stmt_count; s++) {
                    uir_node_t *stmt = block->stmts[s];
                    if (stmt && stmt->loc.file && stmt->loc.line > 0) {
                        int found = 0;
                        for (size_t i = 0; i < total; i++) {
                            if (lines[i].line == stmt->loc.line &&
                                strcmp(lines[i].file, stmt->loc.file) == 0) {
                                found = 1; break;
                            }
                        }
                        if (!found) {
                            if (total >= cap) break;
                            lines[total].file = stmt->loc.file;
                            lines[total].line = stmt->loc.line;
                            total++;
                        }
                    }
                }
            }
        }

        /* Count continuous assign statements */
        for (size_t a = 0; a < unit->assign_count; a++) {
            uir_assign_t *assign = unit->assigns[a];
            if (assign && ((uir_node_t *)assign)->loc.file &&
                ((uir_node_t *)assign)->loc.line > 0) {
                uir_loc_t loc = ((uir_node_t *)assign)->loc;
                int found = 0;
                for (size_t i = 0; i < total; i++) {
                    if (lines[i].line == loc.line &&
                        strcmp(lines[i].file, loc.file) == 0) {
                        found = 1; break;
                    }
                }
                if (!found && total < cap) {
                    lines[total].file = loc.file;
                    lines[total].line = loc.line;
                    total++;
                }
            }
        }
    }

    free(lines);
    return total;
}

double qsim_session_get_coverage_percent(qsim_session_t *sess) {
    if (!sess) return 0.0;
    size_t executed = qsim_session_get_coverage_count(sess);
    if (executed == 0) return 0.0;
    size_t total = count_total_source_lines(sess);
    if (total == 0) return 100.0;
    return (double)executed / (double)total * 100.0;
}

/* VCD signal info: extends basic name+code with IEEE 1364-2001 metadata */

typedef struct {
    char *name;
    char code[8];
    uint32_t width;
    int sig_type;     /* UIR_SIG_WIRE / UIR_SIG_REG / etc. */
    int is_vector;
    uint32_t msb;
    uint32_t lsb;
} vcd_signal_t;

static void write_vcd_value(FILE *f, const qsim_bit_vector_t *val, const char *code) {
    if (!val || !val->bits) {
        fprintf(f, "x%s\n", code);
        return;
    }
    if (val->width <= 1) {
        char ch = 'x';
        switch (val->bits[0].state) {
            case QSIM_1: ch = '1'; break;
            case QSIM_X: ch = 'x'; break;
            case QSIM_Z: ch = 'z'; break;
            case QSIM_U: ch = 'x'; break;
            case QSIM_W: ch = 'x'; break;
            case QSIM_L: ch = '0'; break;
            case QSIM_H: ch = '1'; break;
            case QSIM_DC: ch = 'x'; break;
            default: ch = '0'; break;
        }
        fprintf(f, "%c%s\n", ch, code);
    } else {
        fprintf(f, "b");
        for (uint32_t i = 0; i < val->width; i++) {
            uint32_t idx = val->width - 1 - i; /* VCD is MSB-first */
            char ch = 'x';
            switch (val->bits[idx].state) {
                case QSIM_1: ch = '1'; break;
                case QSIM_X: ch = 'x'; break;
                case QSIM_Z: ch = 'z'; break;
                case QSIM_U: ch = 'x'; break;
                case QSIM_W: ch = 'x'; break;
                case QSIM_L: ch = '0'; break;
                case QSIM_H: ch = '1'; break;
                case QSIM_DC: ch = 'x'; break;
                default: ch = '0'; break;
            }
            fputc(ch, f);
        }
        fprintf(f, " %s\n", code);
    }
}

static int vcd_signal_cmp(const void *a, const void *b) {
    const vcd_signal_t *sa = (const vcd_signal_t *)a;
    const vcd_signal_t *sb = (const vcd_signal_t *)b;
    return strcmp(sa->name, sb->name);
}

/* Look up the UIR signal node for a sim signal by index and fill vcd_signal_t fields. */
static void vcd_fill_signal_info(vcd_signal_t *vs, int sim_idx, uir_sim_context_t *sim) {
    uir_node_t *node = uir_sim_get_signal_node(sim, sim_idx);
    if (!node) return;
    if (node->kind == UIR_SIGNAL) {
        uir_signal_t *sig = (uir_signal_t *)node;
        vs->sig_type = sig->sig_type;
        vs->is_vector = sig->is_vector;
        /* UIR signals don't store msb/lsb — use width-1:0 for vectors */
        if (sig->is_vector) {
            vs->msb = sig->width - 1;
            vs->lsb = 0;
        }
    } else if (node->kind == UIR_PORT) {
        uir_port_t *p = (uir_port_t *)node;
        vs->sig_type = p->sig_type;
        vs->is_vector = p->is_vector;
        if (p->is_vector) {
            vs->msb = p->msb;
            vs->lsb = p->lsb;
        }
    }
}

/* Determine the VCD var type string from signal type. */
static const char *vcd_type_str(int sig_type) {
    switch (sig_type) {
        case UIR_SIG_REG:  return "reg";
        case UIR_SIG_VHDL_SIGNAL:    return "wire";
        case UIR_SIG_VHDL_VARIABLE:  return "reg";
        default: return "wire";
    }
}

int qsim_session_export_vcd(qsim_session_t *sess, const char *path) {
    if (!sess || !path) return 0;

    /* Determine the module scope name from the first elaborated design unit */
    const char *module_scope = "top";
    if (sess->elab && sess->elab->unit_count > 0 && sess->elab->units[0] &&
        sess->elab->units[0]->name) {
        module_scope = sess->elab->units[0]->name;
    }

    int sim_count = sess->sim ? uir_sim_get_signal_count(sess->sim) : 0;
    size_t max_sigs = (sim_count > 0 ? (size_t)sim_count : 0) + sess->wave_count;

    vcd_signal_t *sigs = calloc(max_sigs, sizeof(vcd_signal_t));
    if (!sigs) return 0;
    size_t sig_count = 0;

    for (int i = 0; i < sim_count && sig_count < max_sigs; i++) {
        const char *name = uir_sim_get_signal_name(sess->sim, i);
        if (!name) continue;
        int found = 0;
        for (size_t j = 0; j < sig_count; j++) {
            if (strcmp(sigs[j].name, name) == 0) { found = 1; break; }
        }
        if (!found) {
            sigs[sig_count].name = strdup(name);
            const qsim_bit_vector_t *v = uir_sim_get_signal_value(sess->sim, i);
            sigs[sig_count].width = v ? v->width : 1;
            sigs[sig_count].sig_type = UIR_SIG_WIRE;
            sigs[sig_count].is_vector = 0;
            sigs[sig_count].msb = 0;
            sigs[sig_count].lsb = 0;
            vcd_fill_signal_info(&sigs[sig_count], i, sess->sim);
            sig_count++;
        }
    }

    for (size_t i = 0; i < sess->wave_count && sig_count < max_sigs; i++) {
        const char *name = sess->waves[i].signal_name;
        if (!name) continue;
        int found = 0;
        for (size_t j = 0; j < sig_count; j++) {
            if (strcmp(sigs[j].name, name) == 0) { found = 1; break; }
        }
        if (!found) {
            sigs[sig_count].name = strdup(name);
            sigs[sig_count].width = sess->waves[i].value.width > 0 ? sess->waves[i].value.width : 1;
            sig_count++;
        }
    }

    for (size_t i = 0; i < sig_count; i++) {
        if (i < 94) {
            sigs[i].code[0] = (char)(33 + i);
            sigs[i].code[1] = '\0';
        } else {
            size_t n = i;
            size_t len = 0;
            while (1) {
                sigs[i].code[len++] = (char)(33 + (n % 94));
                n = n / 94;
                if (n == 0) break;
                n--;
            }
            sigs[i].code[len] = '\0';
        }
    }

    qsort(sigs, sig_count, sizeof(vcd_signal_t), vcd_signal_cmp);

    FILE *f = fopen(path, "w");
    if (!f) {
        for (size_t i = 0; i < sig_count; i++) free(sigs[i].name);
        free(sigs);
        return 0;
    }

    /* ── Header ── */
    fprintf(f, "$date $end\n");
    fprintf(f, "$version Qiming Simulator 0.1 $end\n");
    fprintf(f, "$timescale 1 ps $end\n");

    /* ── Scope and variable declarations ── */
    char *scope_stack[64] = {0};
    size_t scope_depth = 0;

    for (size_t i = 0; i < sig_count; i++) {
        const char *dot = strrchr(sigs[i].name, '.');
        const char *leaf;
        size_t num_segments = 0;
        char *segments[64];

        if (dot) {
            leaf = dot + 1;
            char scope_buf[512];
            size_t scope_len = (size_t)(dot - sigs[i].name);
            if (scope_len >= sizeof(scope_buf)) scope_len = sizeof(scope_buf) - 1;
            memcpy(scope_buf, sigs[i].name, scope_len);
            scope_buf[scope_len] = '\0';

            char *tok = strtok(scope_buf, ".");
            while (tok && num_segments < 64) {
                segments[num_segments++] = tok;
                tok = strtok(NULL, ".");
            }
        } else {
            leaf = sigs[i].name;
            /* Use the actual module name instead of "top" */
            segments[num_segments++] = (char *)module_scope;
        }

        size_t common = 0;
        while (common < scope_depth && common < num_segments &&
               strcmp(scope_stack[common], segments[common]) == 0) {
            common++;
        }

        while (scope_depth > common) {
            scope_depth--;
            fprintf(f, "$upscope $end\n");
            free(scope_stack[scope_depth]);
            scope_stack[scope_depth] = NULL;
        }

        for (size_t j = common; j < num_segments; j++) {
            fprintf(f, "$scope module %s $end\n", segments[j]);
            scope_stack[scope_depth] = strdup(segments[j]);
            scope_depth++;
        }

        /* Emit $var with proper type (reg/wire) and range (MSB:LSB) */
        const char *vtype = vcd_type_str(sigs[i].sig_type);
        if (sigs[i].width > 1 || sigs[i].is_vector)
            fprintf(f, "$var %s %u %s %s [%u:%u] $end\n",
                    vtype, sigs[i].width, sigs[i].code, leaf,
                    sigs[i].msb, sigs[i].lsb);
        else
            fprintf(f, "$var %s 1 %s %s $end\n", vtype, sigs[i].code, leaf);
    }

    while (scope_depth > 0) {
        scope_depth--;
        fprintf(f, "$upscope $end\n");
        free(scope_stack[scope_depth]);
    }

    fprintf(f, "$enddefinitions $end\n");

    /* ── $dumpvars: initial values at time 0 ── */
    fprintf(f, "$dumpvars\n");
    for (size_t i = 0; i < sig_count; i++) {
        qsim_bit_vector_t val = {0, NULL};
        int have_val = 0;

        for (size_t w = 0; w < sess->wave_count; w++) {
            if (sess->waves[w].time_fs == 0 &&
                strcmp(sess->waves[w].signal_name, sigs[i].name) == 0) {
                val.width = sess->waves[w].value.width;
                val.bits = malloc(val.width * sizeof(qsim_value_t));
                if (val.bits) {
                    memcpy(val.bits, sess->waves[w].value.bits,
                           val.width * sizeof(qsim_value_t));
                    have_val = 1;
                }
                break;
            }
        }

        if (!have_val && sess->sim) {
            qsim_bit_vector_t v = qsim_session_eval(sess, sigs[i].name);
            if (v.bits) {
                val = v;
                have_val = 1;
            }
        }

        if (have_val) {
            write_vcd_value(f, &val, sigs[i].code);
            free(val.bits);
        } else {
            if (sigs[i].width <= 1)
                fprintf(f, "x%s\n", sigs[i].code);
            else {
                fprintf(f, "b");
                for (uint32_t j = 0; j < sigs[i].width; j++) fputc('x', f);
                fprintf(f, " %s\n", sigs[i].code);
            }
        }
    }
    fprintf(f, "$end\n");

    /* ── Timeline: wave entries sorted by time ── */
    if (sess->wave_count > 1) {
        wave_entry_t *sorted = malloc(sess->wave_count * sizeof(wave_entry_t));
        if (sorted) {
            memcpy(sorted, sess->waves, sess->wave_count * sizeof(wave_entry_t));
            qsort(sorted, sess->wave_count, sizeof(wave_entry_t), wave_sort_by_time);

            uint64_t current_time = 0;
            for (size_t i = 0; i < sess->wave_count; i++) {
                if (sorted[i].time_fs != current_time) {
                    current_time = sorted[i].time_fs;
                    fprintf(f, "#%llu\n", (unsigned long long)current_time);
                }
                const char *code = NULL;
                for (size_t s = 0; s < sig_count; s++) {
                    if (strcmp(sigs[s].name, sorted[i].signal_name) == 0) {
                        code = sigs[s].code;
                        break;
                    }
                }
                if (code)
                    write_vcd_value(f, &sorted[i].value, code);
            }
            free(sorted);
        }
    } else if (sess->wave_count == 1) {
        const char *code = NULL;
        for (size_t s = 0; s < sig_count; s++) {
            if (strcmp(sigs[s].name, sess->waves[0].signal_name) == 0) {
                code = sigs[s].code;
                break;
            }
        }
        if (sess->waves[0].time_fs > 0)
            fprintf(f, "#%llu\n", (unsigned long long)sess->waves[0].time_fs);
        if (code)
            write_vcd_value(f, &sess->waves[0].value, code);
    }

    fclose(f);

    for (size_t i = 0; i < sig_count; i++) free(sigs[i].name);
    free(sigs);
    return 1;
}


/* ── FSDB binary export ── */

/* Binary FSDB-like format:
 *   Header: magic "FSDB" + version(uint32 LE) + timescale_exp(int32 LE)
 *   Signal table: count(uint32 LE), then for each signal:
 *     code(uint8), name_len(uint16 LE), name(char[]), width(uint16 LE)
 *   Value blocks: for each unique time (sorted):
 *     time(uint64 LE), change_count(uint32 LE), then for each change:
 *       code(uint8), byte_len(uint32 LE), data(uint8[byte_len])
 *   End marker: byte 0xFF
 */

static int write_u32_le(FILE *f, uint32_t v) {
    unsigned char buf[4];
    buf[0] = (unsigned char)(v & 0xFF);
    buf[1] = (unsigned char)((v >> 8) & 0xFF);
    buf[2] = (unsigned char)((v >> 16) & 0xFF);
    buf[3] = (unsigned char)((v >> 24) & 0xFF);
    return fwrite(buf, 1, 4, f) == 4 ? 1 : 0;
}

static int write_u16_le(FILE *f, uint16_t v) {
    unsigned char buf[2];
    buf[0] = (unsigned char)(v & 0xFF);
    buf[1] = (unsigned char)((v >> 8) & 0xFF);
    return fwrite(buf, 1, 2, f) == 2 ? 1 : 0;
}

static int write_u64_le(FILE *f, uint64_t v) {
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
    return fwrite(buf, 1, 8, f) == 8 ? 1 : 0;
}

static size_t value_byte_count(const qsim_bit_vector_t *val) {
    return (val->width + 3) / 4; /* 2 bits per state, 4 states per byte */
}

static void write_value_bytes(unsigned char *dst, const qsim_bit_vector_t *val) {
    /* Pack 2-bit states: 0=00, 1=01, X=10, Z=11, MSB first */
    size_t bytes = value_byte_count(val);
    for (size_t b = 0; b < bytes; b++) {
        unsigned char byte = 0;
        for (int bit = 0; bit < 4; bit++) {
            size_t idx = (bytes - 1 - b) * 4 + bit; /* MSB first within byte */
            if (idx < val->width) {
                size_t v_idx = val->width - 1 - idx; /* VCD-style MSB-first */
                uint8_t code = 0;
                switch (val->bits[v_idx].state) {
                    case QSIM_1: code = 1; break;
                    case QSIM_X: code = 2; break;
                    case QSIM_Z: code = 3; break;
                    case QSIM_U: code = 2; break;
                    case QSIM_W: code = 2; break;
                    case QSIM_L: code = 0; break;
                    case QSIM_H: code = 1; break;
                    case QSIM_DC: code = 2; break;
                    default: code = 0; break;
                }
                byte |= (code << (6 - bit * 2));
            }
        }
        dst[b] = byte;
    }
}

int qsim_session_export_fsdb(qsim_session_t *sess, const char *path) {
    if (!sess || !path) return 0;

    /* Collect unique signal names (same as VCD) */
    int sim_count = sess->sim ? uir_sim_get_signal_count(sess->sim) : 0;
    size_t max_sigs = (sim_count > 0 ? (size_t)sim_count : 0) + sess->wave_count;

    vcd_signal_t *sigs = calloc(max_sigs, sizeof(vcd_signal_t));
    if (!sigs) return 0;
    size_t sig_count = 0;

    for (int i = 0; i < sim_count && sig_count < max_sigs; i++) {
        const char *name = uir_sim_get_signal_name(sess->sim, i);
        if (!name) continue;
        int found = 0;
        for (size_t j = 0; j < sig_count; j++) {
            if (strcmp(sigs[j].name, name) == 0) { found = 1; break; }
        }
        if (!found) {
            sigs[sig_count].name = strdup(name);
            const qsim_bit_vector_t *v = uir_sim_get_signal(sess->sim, name);
            sigs[sig_count].width = v ? v->width : 1;
            sigs[sig_count].code[0] = (char)(sig_count);
            sig_count++;
        }
    }

    for (size_t i = 0; i < sess->wave_count && sig_count < max_sigs; i++) {
        const char *name = sess->waves[i].signal_name;
        if (!name) continue;
        int found = 0;
        for (size_t j = 0; j < sig_count; j++) {
            if (strcmp(sigs[j].name, name) == 0) { found = 1; break; }
        }
        if (!found) {
            sigs[sig_count].name = strdup(name);
            sigs[sig_count].width = sess->waves[i].value.width > 0 ? sess->waves[i].value.width : 1;
            sigs[sig_count].code[0] = (char)sig_count;
            sig_count++;
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        for (size_t i = 0; i < sig_count; i++) free(sigs[i].name);
        free(sigs);
        return 0;
    }

    /* Write header: magic + version + timescale */
    fwrite("FSDB", 1, 4, f);
    write_u32_le(f, 1);       /* version */
    write_u32_le(f, -12);     /* timescale exponent (1ps = 10^-12) as signed bit pattern */

    /* Write signal table */
    uint32_t sc = (uint32_t)sig_count;
    write_u32_le(f, sc);

    for (size_t i = 0; i < sig_count; i++) {
        size_t name_len = strlen(sigs[i].name);
        if (name_len > 65535) name_len = 65535;
        fputc((int)i, f);
        write_u16_le(f, (uint16_t)name_len);
        fwrite(sigs[i].name, 1, name_len, f);
        write_u16_le(f, (uint16_t)sigs[i].width);
    }

    /* Write initial values (time 0) */
    {
        uint32_t init_count = 0;
        for (size_t i = 0; i < sig_count; i++) {
            if (sess->sim) {
                qsim_bit_vector_t v = qsim_session_eval(sess, sigs[i].name);
                if (v.bits) {
                    init_count++;
                    free(v.bits);
                }
            }
        }

        write_u64_le(f, 0);
        write_u32_le(f, init_count);

        for (size_t i = 0; i < sig_count; i++) {
            qsim_bit_vector_t v = qsim_session_eval(sess, sigs[i].name);
            if (!v.bits) continue;

            fputc((int)i, f);       /* code */
            size_t bc = value_byte_count(&v);
            write_u32_le(f, (uint32_t)bc); /* byte count */
            unsigned char *vbuf = malloc(bc);
            if (vbuf) {
                write_value_bytes(vbuf, &v);
                fwrite(vbuf, 1, bc, f);
                free(vbuf);
            }
            free(v.bits);
        }
    }

    /* Write value changes sorted by time */
    if (sess->wave_count > 0) {
        wave_entry_t *sorted = malloc(sess->wave_count * sizeof(wave_entry_t));
        if (sorted) {
            memcpy(sorted, sess->waves, sess->wave_count * sizeof(wave_entry_t));
            qsort(sorted, sess->wave_count, sizeof(wave_entry_t), wave_sort_by_time);

            uint64_t current_time = 0;
            /* Count changes at each unique time */
            size_t start = 0;
            while (start < sess->wave_count) {
                current_time = sorted[start].time_fs;
                size_t end = start;
                while (end < sess->wave_count && sorted[end].time_fs == current_time)
                    end++;

                uint32_t changes = 0;
                for (size_t c = start; c < end; c++) {
                    const char *code_str = NULL;
                    for (size_t s = 0; s < sig_count; s++) {
                        if (strcmp(sigs[s].name, sorted[c].signal_name) == 0) {
                            code_str = sigs[s].code;
                            break;
                        }
                    }
                    if (code_str) changes++;
                }

                if (current_time > 0 && changes > 0) {
                    write_u64_le(f, current_time);
                    write_u32_le(f, changes);

                    for (size_t c = start; c < end; c++) {
                        int code_idx = -1;
                        for (size_t s = 0; s < sig_count; s++) {
                            if (strcmp(sigs[s].name, sorted[c].signal_name) == 0) {
                                code_idx = (int)s;
                                break;
                            }
                        }
                        if (code_idx < 0) continue;

                        fputc(code_idx, f);
                        size_t bc = value_byte_count(&sorted[c].value);
                        write_u32_le(f, (uint32_t)bc);
                        unsigned char *vbuf = malloc(bc);
                        if (vbuf) {
                            write_value_bytes(vbuf, &sorted[c].value);
                            fwrite(vbuf, 1, bc, f);
                            free(vbuf);
                        }
                    }
                }
                start = end;
            }
            free(sorted);
        }
    }

    /* End marker */
    fputc(0xFF, f);

    fclose(f);

    for (size_t i = 0; i < sig_count; i++) free(sigs[i].name);
    free(sigs);
    return 1;
}


/* ── Design summary (hierarchy, ports, signals, instances) ── */

char *qsim_session_get_design_summary(qsim_session_t *sess) {
    if (!sess || !sess->elab) return NULL;

#define SUM_BUF 65536
    char *buf = malloc(SUM_BUF);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;
    int n;

#define SUM_APPEND(...) do { \
    n = snprintf(buf + len, SUM_BUF - len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= SUM_BUF - len) { free(buf); return NULL; } \
    len += (size_t)n; \
} while(0)

    SUM_APPEND("{\"units\":[");
    for (size_t i = 0; i < sess->elab->unit_count; i++) {
        uir_design_unit_t *unit = sess->elab->units[i];
        if (!unit) continue;
        if (i > 0) SUM_APPEND(",");

        SUM_APPEND("{\"name\":\"%s\",\"language\":\"%s\"",
                   unit->name ? unit->name : "",
                   unit->language ? unit->language : "");

        /* Ports */
        SUM_APPEND(",\"port_count\":%zu,\"ports\":[", unit->port_count);
        if (unit->ports) {
            size_t first = 1;
            for (size_t p = 0; p < unit->port_count; p++) {
                uir_port_t *port = unit->ports[p];
                if (!port || !port->name) continue;
                if (!first) SUM_APPEND(",");
                first = 0;
                const char *dir = port->direction == UIR_PORT_IN ? "input"
                                : port->direction == UIR_PORT_OUT ? "output" : "inout";
                SUM_APPEND("{\"name\":\"%s\",\"direction\":\"%s\",\"width\":%u}",
                           port->name, dir, port->width);
            }
        }
        SUM_APPEND("]");

        /* Signals */
        SUM_APPEND(",\"signal_count\":%zu,\"signals\":[", unit->signal_count);
        if (unit->signals) {
            size_t first = 1;
            for (size_t s = 0; s < unit->signal_count; s++) {
                uir_signal_t *sig = unit->signals[s];
                if (!sig || !sig->name) continue;
                if (!first) SUM_APPEND(",");
                first = 0;
                const char *stype = sig->sig_type == UIR_SIG_REG ? "reg"
                                  : sig->sig_type == UIR_SIG_VHDL_SIGNAL ? "signal"
                                  : sig->sig_type == UIR_SIG_VHDL_VARIABLE ? "variable"
                                  : "wire";
                if (sig->array_size > 0)
                    SUM_APPEND("{\"name\":\"%s\",\"type\":\"%s\",\"width\":%u,\"array_size\":%u}",
                               sig->name, stype, sig->width, sig->array_size);
                else
                    SUM_APPEND("{\"name\":\"%s\",\"type\":\"%s\",\"width\":%u}",
                               sig->name, stype, sig->width);
            }
        }
        SUM_APPEND("]");

        /* Instances */
        SUM_APPEND(",\"instance_count\":%zu,\"instances\":[", unit->instance_count);
        if (unit->instances) {
            size_t first = 1;
            for (size_t j = 0; j < unit->instance_count; j++) {
                uir_instance_t *inst = unit->instances[j];
                if (!inst || !inst->instance_name) continue;
                if (!first) SUM_APPEND(",");
                first = 0;
                SUM_APPEND("{\"name\":\"%s\",\"module_name\":\"%s\",\"port_count\":%zu}",
                           inst->instance_name,
                           inst->module_name ? inst->module_name : "",
                           inst->connection_count);
            }
        }
        SUM_APPEND("]");

        SUM_APPEND(",\"process_count\":%zu,\"assign_count\":%zu}",
                   unit->process_count, unit->assign_count);
    }
    SUM_APPEND("]}");

#undef SUM_APPEND
    return buf;
}

/* ── Control-flow / FSM extraction ── */

/* Check if a UIR node is a ref to a reg signal in the given unit. */
static int is_reg_ref(uir_node_t *node, uir_design_unit_t *unit) {
    if (!node || node->kind != UIR_REF || !unit) return 0;
    uir_ref_t *ref = (uir_ref_t *)node;
    if (!ref->name) return 0;
    if (!unit->signals) return 0;
    for (size_t i = 0; i < unit->signal_count; i++) {
        uir_signal_t *sig = unit->signals[i];
        if (sig && sig->name && strcmp(sig->name, ref->name) == 0)
            return (sig->sig_type == UIR_SIG_REG);
    }
    return 0;
}

/* Convert a literal node to a binary string. Returns 0 if not a literal. */
static int lit_to_str(uir_node_t *node, char *buf, size_t buf_size) {
    if (!node || node->kind != UIR_LITERAL || !buf || buf_size == 0) return 0;
    uir_literal_t *lit = (uir_literal_t *)node;
    if (!lit->value || !lit->value->bits) {
        buf[0] = 'X'; buf[1] = '\0';
        return 1;
    }
    size_t pos = 0;
    for (uint32_t i = 0; i < lit->value->width && pos + 1 < buf_size; i++) {
        buf[pos++] = (lit->value->bits[i].state == QSIM_1) ? '1' : '0';
    }
    buf[pos] = '\0';
    return 1;
}

/* Maximum FSM states per FSM */
#define MAX_FSM_STATES 64
/* Maximum transitions per FSM */
#define MAX_FSM_TRANS 128

/* Find case statements in a process body and emit FSM JSON into buf+len.
 * Returns 0 if buffer would overflow. */
static int extract_fsms_from_node(uir_node_t *node, uir_design_unit_t *unit,
                                   char *buf, size_t cap, size_t *len,
                                   int *first_fsm) {
    if (!node) return 1;
    int n;
    char cond_str[64] = "";  /* current IF condition, propagated to ASSIGN handler */

#define FSM_APPEND(...) do { \
    n = snprintf(buf + *len, cap - *len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= cap - *len) return 0; \
    *len += (size_t)n; \
} while(0)

    switch (node->kind) {
    case UIR_CASE: {
        uir_case_t *case_node = (uir_case_t *)node;
        if (!case_node->expr || !is_reg_ref(case_node->expr, unit))
            goto walk_children; /* Not an FSM case statement */

        uir_ref_t *ref = (uir_ref_t *)case_node->expr;
        const char *reg_name = ref->name;

        /* Find the reg signal width */
        uint32_t reg_width = 0;
        for (size_t i = 0; i < unit->signal_count; i++) {
            uir_signal_t *sig = unit->signals[i];
            if (sig && sig->name && strcmp(sig->name, reg_name) == 0) {
                reg_width = sig->width;
                break;
            }
        }

        /* Collect state encodings from case item patterns */
        char state_values[MAX_FSM_STATES][64];
        size_t state_count = 0;
        size_t item_idx;
        for (item_idx = 0; item_idx < case_node->item_count && state_count < MAX_FSM_STATES; item_idx++) {
            uir_case_item_t *item = case_node->items[item_idx];
            if (!item || item->pattern_count == 0) continue;
            /* Use the first pattern as the state encoding */
            if (lit_to_str(item->patterns[0], state_values[state_count], sizeof(state_values[0])))
                state_count++;
        }

        /* For each case item, search body for assignments to state reg → transitions */
        char trans_from[MAX_FSM_TRANS][64];
        char trans_to[MAX_FSM_TRANS][64];
        char trans_cond[MAX_FSM_TRANS][64];
        size_t trans_count = 0;

        for (item_idx = 0; item_idx < case_node->item_count && trans_count < MAX_FSM_TRANS; item_idx++) {
            uir_case_item_t *item = case_node->items[item_idx];
            if (!item || item->pattern_count == 0) continue;
            char from_val[64];
            if (!lit_to_str(item->patterns[0], from_val, sizeof(from_val)))
                continue;

            /* Walk the item body looking for ASSIGN nodes to the state register */
            /* We do a simple walk of the body tree */
            /* This inner walk finds assignments within the case item */
            /* Use a recursive helper approach — inline for simplicity */

            /* Walk the item body */
            uir_node_t *walk_stack[64];
            int walk_sp = 0;
            uir_node_t *walk_cur = item->body;
            cond_str[0] = '\0';  /* reset condition for each case item */

walk_body:
            if (!walk_cur || trans_count >= MAX_FSM_TRANS) goto next_item;

            if (walk_cur->kind == UIR_ASSIGN) {
                uir_assign_t *assign = (uir_assign_t *)walk_cur;
                /* Check if LHS is a ref to our state register */
                if (assign->lhs && assign->lhs->kind == UIR_REF) {
                    uir_ref_t *lhs_ref = (uir_ref_t *)assign->lhs;
                    if (lhs_ref->name && strcmp(lhs_ref->name, reg_name) == 0) {
                        /* It's a transition: from=from_val, to=RHS value */
                        char to_val[64] = "";
                        lit_to_str(assign->rhs, to_val, sizeof(to_val));
                        if (to_val[0] && strcmp(from_val, to_val) != 0) {
                            strcpy(trans_from[trans_count], from_val);
                            strcpy(trans_to[trans_count], to_val);
                            strcpy(trans_cond[trans_count], cond_str);
                            trans_count++;
                        }
                        goto next_item;
                    }
                }
                goto next_item;
            } else if (walk_cur->kind == UIR_IF) {
                uir_if_t *if_node = (uir_if_t *)walk_cur;
                /* Extract condition name from the if condition */
                cond_str[0] = '\0';
                if (if_node->condition && if_node->condition->kind == UIR_REF) {
                    uir_ref_t *cond_ref = (uir_ref_t *)if_node->condition;
                    if (cond_ref->name)
                        snprintf(cond_str, sizeof(cond_str), "%s", cond_ref->name);
                }

                /* Push then_branch and else_branch onto stack */
                if (if_node->then_branch && if_node->then_branch->kind == UIR_BLOCK) {
                    uir_block_t *blk = (uir_block_t *)if_node->then_branch;
                    for (int si = (int)blk->stmt_count - 1; si >= 0 && walk_sp < 64; si--)
                        walk_stack[walk_sp++] = blk->stmts[si];
                }
                if (if_node->else_branch && if_node->else_branch->kind == UIR_BLOCK) {
                    uir_block_t *blk = (uir_block_t *)if_node->else_branch;
                    for (int si = (int)blk->stmt_count - 1; si >= 0 && walk_sp < 64; si--)
                        walk_stack[walk_sp++] = blk->stmts[si];
                }
                goto pop_stack;
            } else if (walk_cur->kind == UIR_BLOCK) {
                uir_block_t *blk = (uir_block_t *)walk_cur;
                for (int si = (int)blk->stmt_count - 1; si >= 0 && walk_sp < 64; si--)
                    walk_stack[walk_sp++] = blk->stmts[si];
                goto pop_stack;
            }

next_item:
            if (walk_sp > 0) {
                walk_cur = walk_stack[--walk_sp];
                goto walk_body;
            }
            continue;

pop_stack:
            if (walk_sp > 0) {
                walk_cur = walk_stack[--walk_sp];
                goto walk_body;
            }
        }

        /* Emit this FSM as JSON */
        if (!(*first_fsm)) FSM_APPEND(",");
        *first_fsm = 0;

        FSM_APPEND("{\"state_register\":\"%s\",\"width\":%u,\"states\":[",
                   reg_name, reg_width);
        for (size_t si = 0; si < state_count; si++) {
            if (si > 0) FSM_APPEND(",");
            FSM_APPEND("{\"value\":\"%s\"}", state_values[si]);
        }
        FSM_APPEND("],\"transitions\":[");
        for (size_t ti = 0; ti < trans_count; ti++) {
            if (ti > 0) FSM_APPEND(",");
            if (trans_cond[ti][0])
                FSM_APPEND("{\"from\":\"%s\",\"to\":\"%s\",\"condition\":\"%s\"}",
                           trans_from[ti], trans_to[ti], trans_cond[ti]);
            else
                FSM_APPEND("{\"from\":\"%s\",\"to\":\"%s\"}",
                           trans_from[ti], trans_to[ti]);
        }
        FSM_APPEND("]}");
        return 1;
    }

    case UIR_BLOCK: {
        uir_block_t *blk = (uir_block_t *)node;
        for (size_t i = 0; i < blk->stmt_count; i++) {
            if (!extract_fsms_from_node(blk->stmts[i], unit, buf, cap, len, first_fsm))
                return 0;
        }
        return 1;
    }
    case UIR_IF: {
        uir_if_t *if_node = (uir_if_t *)node;
        if (if_node->then_branch)
            if (!extract_fsms_from_node(if_node->then_branch, unit, buf, cap, len, first_fsm))
                return 0;
        if (if_node->else_branch)
            if (!extract_fsms_from_node(if_node->else_branch, unit, buf, cap, len, first_fsm))
                return 0;
        return 1;
    }
    case UIR_PROCESS:
    case UIR_ALWAYS:
    case UIR_INITIAL:
    case UIR_PROCESS_VHDL: {
        uir_process_t *proc = (uir_process_t *)node;
        if (proc->body)
            return extract_fsms_from_node(proc->body, unit, buf, cap, len, first_fsm);
        return 1;
    }
    case UIR_CASE_ITEM: {
        uir_case_item_t *item = (uir_case_item_t *)node;
        if (item->body)
            return extract_fsms_from_node(item->body, unit, buf, cap, len, first_fsm);
        return 1;
    }
    case UIR_LOOP: {
        uir_loop_t *loop = (uir_loop_t *)node;
        if (loop->body)
            return extract_fsms_from_node(loop->body, unit, buf, cap, len, first_fsm);
        return 1;
    }
    default:
        return 1;
    }

walk_children:
    /* Recursively walk children for other node types */
    /* For case nodes that didn't match, still walk their items */
    if (node->kind == UIR_CASE) {
        uir_case_t *case_node = (uir_case_t *)node;
        for (size_t i = 0; i < case_node->item_count; i++) {
            if (!extract_fsms_from_node((uir_node_t *)case_node->items[i], unit, buf, cap, len, first_fsm))
                return 0;
        }
    }
    return 1;
}

#undef FSM_APPEND

char *qsim_session_get_control_flow(qsim_session_t *sess) {
    if (!sess || !sess->elab) return NULL;

#define CF_BUF 65536
    char *buf = malloc(CF_BUF);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;
    int n;

#define SUM_APPEND(...) do { \
    n = snprintf(buf + len, CF_BUF - len, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= CF_BUF - len) { free(buf); return NULL; } \
    len += (size_t)n; \
} while(0)

    SUM_APPEND("{\"fsms\":[");
    int first_fsm = 1;
    for (size_t u = 0; u < sess->elab->unit_count; u++) {
        uir_design_unit_t *unit = sess->elab->units[u];
        if (!unit) continue;
        for (size_t p = 0; p < unit->process_count; p++) {
            if (!unit->processes[p]) continue;
            if (!extract_fsms_from_node((uir_node_t *)unit->processes[p], unit,
                                         buf, CF_BUF, &len, &first_fsm)) {
                free(buf);
                return NULL;
            }
        }
    }
    SUM_APPEND("]}");

#undef SUM_APPEND
    return buf;
}


/* ── Signal trace ── */

char *qsim_session_trace_drivers(qsim_session_t *sess, const char *signal,
                                  size_t max_depth) {
    if (!sess || !sess->sim || !signal) return NULL;
    return uir_sim_trace_drivers(sess->sim, signal, max_depth);
}


/* ── Stop/finish/continue API ── */

int qsim_session_is_stopped(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_is_stopped(sess->sim);
}

int qsim_session_is_finished(qsim_session_t *sess) {
    if (!sess || !sess->sim) return 0;
    return uir_sim_is_finished(sess->sim);
}

void qsim_session_continue(qsim_session_t *sess) {
    if (!sess || !sess->sim) return;
    uir_sim_clear_stop(sess->sim);
    sess->state = SESSION_SIMULATING;
}
