#ifndef LIBDSIM_SESSION_H
#define LIBDSIM_SESSION_H

#include "libqsim/uir.h"
#include "libqsim/value.h"
#include "libqsim/simulator.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque session handle — persistent state across compile → elaborate → simulate */
typedef struct qsim_session qsim_session_t;

/* ── Lifecycle ── */

qsim_session_t *qsim_session_create(void);
void qsim_session_free(qsim_session_t *sess);

/* Reset session state for test reuse. Clears units, sim, waves,
 * coverage, and diagnostics. Call between independent test runs. */
void qsim_session_reset(qsim_session_t *sess);

/* ── Compilation ── */

/* Add an include search path for Verilog `include directives. */
void qsim_session_add_include_path(qsim_session_t *sess, const char *path);

/* Compile an inline source string. Returns 0 on failure. */
int qsim_session_compile_string(qsim_session_t *sess, const char *name, const char *source);

/* Compile a file. Returns 0 on failure. */
int qsim_session_compile_file(qsim_session_t *sess, const char *path);

/* ── Elaboration ── */

/* Elaborate the compiled design. Returns 0 on failure. */
int qsim_session_elaborate(qsim_session_t *sess);

/* ── Simulation stepping ── */

/* Set thread count for parallel delta evaluation (call after elaborate, before step).
 * thread_count=1 disables parallelism (default). */
void qsim_session_set_thread_count(qsim_session_t *sess, int thread_count);

/* Run one delta cycle. Returns 0 if no events remain. */
int qsim_session_step_delta(qsim_session_t *sess);

/* Get total event count processed so far. */
size_t qsim_session_get_event_count(qsim_session_t *sess);

/* Run simulation forward by time_fs femtoseconds. Returns number of delta steps processed. */
uint64_t qsim_session_step_time(qsim_session_t *sess, uint64_t time_fs);

/* ── Signal access ── */

/* Number of signals in the elaborated design */
int qsim_session_get_signal_count(qsim_session_t *sess);

/* Get signal name by index (returns NULL if idx out of range) */
const char *qsim_session_get_signal_name(qsim_session_t *sess, int idx);

/* Get signal value by index (returns NULL if idx out of range) */
qsim_bit_vector_t qsim_session_get_signal_value(qsim_session_t *sess, int idx);

/* Get signal value by hierarchical path. Returns zero-initialized vector on failure. */
qsim_bit_vector_t qsim_session_get_signal_by_name(qsim_session_t *sess, const char *hier_path);

/* ── Interactive debug ── */

/* Evaluate a signal. Returns zero-initialized vector on failure. */
qsim_bit_vector_t qsim_session_eval(qsim_session_t *sess, const char *signal);

/* Evaluate a signal and return its value as a malloc'd string (e.g. "1010XZ").
 * Caller must free the returned string with qsim_session_free_str. */
char *qsim_session_eval_str(qsim_session_t *sess, const char *signal);

/* Bulk evaluation of multiple signals. Returns a malloc'd result struct.
 * Each value string must be freed individually. The result itself must be
 * freed with qsim_eval_multi_result_free. Returns NULL on failure. */
typedef struct {
    char **values;
    size_t count;
} qsim_eval_multi_result_t;

qsim_eval_multi_result_t *qsim_session_eval_multi(qsim_session_t *sess,
                                                    const char **signals,
                                                    size_t count);
void qsim_eval_multi_result_free(qsim_eval_multi_result_t *result);

/* Run N clock cycles and record watched signal values at each posedge.
 * Returns a flat array: result->values[cycle * signal_count + signal].
 * Each value string must be freed individually. The result itself must be
 * freed with qsim_debug_trace_result_free. Returns NULL on failure or if
 * the session is not elaborated. */
typedef struct {
    char **values;       /* [cycle * signal_count + signal] */
    size_t cycle_count;
    size_t signal_count;
} qsim_debug_trace_result_t;

qsim_debug_trace_result_t *qsim_session_debug_trace(qsim_session_t *sess,
                                                      const char **signals,
                                                      size_t signal_count,
                                                      size_t cycles);
void qsim_debug_trace_result_free(qsim_debug_trace_result_t *result);

/* Force a signal to a value (immediate effect). Returns 0 on failure. */
int qsim_session_force(qsim_session_t *sess, const char *signal, const qsim_bit_vector_t *value);

/* Force a signal from a string representation (e.g. "1010XZ").
 * Returns 0 on failure. */
int qsim_session_force_str(qsim_session_t *sess, const char *signal, const char *value_str);

/* Set a signal via the event queue (triggers process sensitivity).
 * Returns 0 on failure. */
int qsim_session_set_str(qsim_session_t *sess, const char *signal, const char *value_str);

/* Release a forced signal (resets to X). Returns 0 on failure. */
int qsim_session_release(qsim_session_t *sess, const char *signal);

/* Load data words into an array memory signal (e.g., reg [31:0] mem [0:1023]).
 * Each uint32_t word is written LSB-first into element at position addr+i.
 * Returns number of words written, or 0 on failure. */
int qsim_session_load_mem(qsim_session_t *sess, const char *signal,
                          uint32_t addr, const uint32_t *data, size_t count);

/* Free a string returned by qsim_session_eval_str or other session API functions. */
void qsim_session_free_str(char *s);

/* ── Wave buffer ── */

/* Number of wave entries captured */
size_t qsim_session_get_wave_count(qsim_session_t *sess);

/* Get wave entry by index. Returns 0 on failure. */
int qsim_session_get_wave(qsim_session_t *sess, size_t idx,
                           const char **signal, uint64_t *time_fs,
                           qsim_bit_vector_t *value);

/* Clear the wave buffer */
void qsim_session_clear_wave(qsim_session_t *sess);

/* Bulk wave query — filter by signal names and time window.
 * If signals is NULL or signal_count==0, returns all signals.
 * t_start/t_end of 0 means unbounded. Results are sorted by time.
 * Returns malloc'd result (caller must free with qsim_wave_bulk_result_free).
 * Each value string is malloc'd and must be freed individually. */
typedef struct {
    char **signals;
    uint64_t *times;
    char **values;
    size_t count;
} qsim_wave_bulk_result_t;

qsim_wave_bulk_result_t *qsim_session_query_wave_bulk(
    qsim_session_t *sess,
    const char **signals, size_t signal_count,
    uint64_t t_start, uint64_t t_end);

void qsim_wave_bulk_result_free(qsim_wave_bulk_result_t *result);

/* ── Diagnostics ── */

/* Get structured diagnostics from the last failed compile or elaborate call.
 * Returns an array of qsim_diagnostic_t with recovery hints, or NULL if none.
 * The returned pointer is valid until the next compile/elaborate call on this session.
 * Sets *count to the number of diagnostics. */
const qsim_diagnostic_t *qsim_session_get_last_diagnostics(qsim_session_t *sess, size_t *count);

/* ── Log buffer ── */

/* Get accumulated log text */
const char *qsim_session_get_log(qsim_session_t *sess);

/* ── Breakpoints ── */

int qsim_session_add_breakpoint(qsim_session_t *sess, const char *file, uint32_t line);
int qsim_session_remove_breakpoint(qsim_session_t *sess, const char *file, uint32_t line);
void qsim_session_clear_breakpoints(qsim_session_t *sess);
size_t qsim_session_get_breakpoint_count(qsim_session_t *sess);
int qsim_session_get_breakpoint(qsim_session_t *sess, size_t idx,
                                 const char **file, uint32_t *line);

/* ── Debug run ── */

int qsim_session_debug_run(qsim_session_t *sess);

/* ── Line coverage ── */

size_t qsim_session_get_coverage_count(qsim_session_t *sess);
int qsim_session_get_coverage_entry(qsim_session_t *sess, size_t idx,
                                     const char **file, uint32_t *line);
double qsim_session_get_coverage_percent(qsim_session_t *sess);

/* ── Stop/finish/continue ── */

/* Check if $stop was called (pauses simulation, can continue). */
int qsim_session_is_stopped(qsim_session_t *sess);

/* Check if $finish or $fatal was called (terminates simulation). */
int qsim_session_is_finished(qsim_session_t *sess);

/* Clear the stop flag to allow simulation to continue. */
void qsim_session_continue(qsim_session_t *sess);

/* ── UART capture ── */

/* Get accumulated UART output as a NUL-terminated string */
const char *qsim_session_get_uart_output(qsim_session_t *sess);

/* Clear the UART output buffer */
void qsim_session_clear_uart_output(qsim_session_t *sess);

/* ── Checkpoints (save/restore simulator state) ── */

/* Save a checkpoint with a given name.
 * Returns malloc'd copy of the name (caller must free) or NULL on failure. */
char *qsim_session_save_checkpoint(qsim_session_t *sess, const char *name);

/* Restore simulation state from a checkpoint.
 * Returns 0 on failure. */
int qsim_session_restore_checkpoint(qsim_session_t *sess, const char *name);

/* Diff two checkpoints.
 * Returns malloc'd JSON string with signal-level differences (caller must free).
 * Returns NULL on failure. */
char *qsim_session_diff_checkpoint(qsim_session_t *sess,
                                    const char *name_a, const char *name_b);

/* List all checkpoints.
 * Returns malloc'd JSON string (caller must free) or NULL on failure. */
char *qsim_session_list_checkpoints(qsim_session_t *sess);

/* ── VCD export ── */

/* Export wave buffer to VCD file. Returns 0 on failure. */
int qsim_session_export_vcd(qsim_session_t *sess, const char *path);

/* ── FSDB export ── */

/* Export wave buffer to binary FSDB file. Returns 0 on failure. */
int qsim_session_export_fsdb(qsim_session_t *sess, const char *path);

/* ── Design comprehension tools ── */

/* Get a JSON summary of the design hierarchy, ports, signals, and instances.
 * Caller must free the returned string with free(). Returns NULL if not elaborated. */
char *qsim_session_get_design_summary(qsim_session_t *sess);

/* Extract FSM state registers and transitions from the design.
 * Returns a JSON string describing all detected FSMs.
 * Caller must free the returned string with free(). Returns NULL if not elaborated. */
char *qsim_session_get_control_flow(qsim_session_t *sess);

/* ── Signal trace ── */

/* Trace the driver chain for a signal.
 * Returns a malloc'd JSON tree of driver signals (caller must free).
 * max_depth limits recursion (0 = direct drivers only).
 * Returns NULL on failure. */
char *qsim_session_trace_drivers(qsim_session_t *sess, const char *signal,
                                  size_t max_depth);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_SESSION_H */
