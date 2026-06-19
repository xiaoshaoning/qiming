#ifndef LIBDSIM_UIR_SIM_H
#define LIBDSIM_UIR_SIM_H

#include "libqsim/uir.h"
#include "libqsim/value.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Simulation context — drives UIR designs using the event-driven scheduler */
typedef struct uir_sim_context uir_sim_context_t;

/* Event callback invoked when any signal changes value during simulation */
typedef void (*uir_event_callback_t)(uir_sim_context_t *ctx,
                                      const char *signal_name,
                                      const qsim_bit_vector_t *new_value,
                                      void *user_data);

/* Step callback invoked before each statement executes (for breakpoints and debugger) */
typedef void (*qsim_step_callback_t)(uir_sim_context_t *ctx,
                                      const char *file,
                                      uint32_t line,
                                      void *user_data);

/* Display log callback: receives formatted output string from $display/$write/$monitor */
typedef void (*uir_sys_display_cb_t)(uir_sim_context_t *ctx,
                                      const char *msg,
                                      void *user_data);

/* Create a simulation context from elaborated design units */
uir_sim_context_t *uir_sim_create(uir_design_unit_t **units, size_t count);

/* Set thread count for parallel delta evaluation (call before uir_sim_run).
 * thread_count=1 disables parallelism (default). Returns 0 on success. */
int uir_sim_set_thread_count(uir_sim_context_t *ctx, int thread_count);

/* Destroy simulation context */
void uir_sim_destroy(uir_sim_context_t *ctx);

/* Run simulation for a given number of time units */
int uir_sim_run(uir_sim_context_t *ctx, uint64_t duration);

/* Set a signal value by hierarchical path (for testbench/driving) */
int uir_sim_set_signal(uir_sim_context_t *ctx, const char *hier_path, qsim_bit_vector_t *value);

/* Ensure initial evaluation has run. Safe to call multiple times.
 * Call before load_mem/force to prevent initial blocks overwriting user writes. */
void uir_sim_ensure_initialized(uir_sim_context_t *ctx);

/* Get signal value by hierarchical path (do NOT free the returned pointer) */
qsim_bit_vector_t *uir_sim_get_signal(uir_sim_context_t *ctx, const char *hier_path);

/* Current simulation time */
uint64_t uir_sim_current_time(uir_sim_context_t *ctx);

/* Total number of events processed during simulation */
size_t uir_sim_get_event_count(uir_sim_context_t *ctx);

/* Register an event callback for signal changes (NULL to unset) */
void uir_sim_set_event_callback(uir_sim_context_t *ctx, uir_event_callback_t cb, void *user_data);

/* Signal table accessors by index */
int uir_sim_get_signal_count(uir_sim_context_t *ctx);
const char *uir_sim_get_signal_name(uir_sim_context_t *ctx, int idx);
const qsim_bit_vector_t *uir_sim_get_signal_value(uir_sim_context_t *ctx, int idx);
uir_node_t *uir_sim_get_signal_node(uir_sim_context_t *ctx, int idx);

/* Force a signal to a value immediately (bypasses event queue, fires callback) */
int uir_sim_force_signal(uir_sim_context_t *ctx, const char *hier_path, const qsim_bit_vector_t *value);

/* Release a forced signal (resets to X, fires callback) */
int uir_sim_release_signal(uir_sim_context_t *ctx, const char *hier_path);

/* Load data words into an array memory signal at a given word address.
 * Each uint32_t word is written LSB-first into the element at position addr+i.
 * The signal must be an array signal (sig->width = element bits, total storage
 * = width * array_size).  Returns number of words written, or 0 on failure. */
int uir_sim_load_mem(uir_sim_context_t *ctx, const char *hier_path,
                     uint32_t addr, const uint32_t *data, size_t count);

/* Utility: convert bit vector to uint64. Returns 0 if any bit is X or Z. */
int uir_bv_to_u64(const qsim_bit_vector_t *bv, uint64_t *out);

/* Utility: create bit vector from uint64 with given width */
qsim_bit_vector_t *uir_u64_to_bv(uint64_t val, uint32_t width);

/* ── Step callback (for breakpoints / debugger) ── */

/* Register a step callback invoked before each statement execution (NULL to unset) */
void uir_sim_set_step_callback(uir_sim_context_t *ctx, qsim_step_callback_t cb, void *user_data);

/* Register a display callback for $display/$write/$monitor output (NULL to unset) */
void uir_sim_set_sys_display_callback(uir_sim_context_t *ctx, uir_sys_display_cb_t cb, void *user_data);

/* ── Breakpoints ── */

/* Add or remove a breakpoint at (file, line). set=1 to add, set=0 to remove. */
void uir_sim_set_breakpoint(uir_sim_context_t *ctx, const char *file, uint32_t line, int set);

/* Check if execution stopped at a breakpoint during the last step */
int uir_sim_breakpoint_hit(uir_sim_context_t *ctx);

/* Clear the breakpoint-hit flag */
void uir_sim_clear_breakpoint_hit(uir_sim_context_t *ctx);

/* ── Line coverage ── */

/* Number of unique (file, line) entries executed */
size_t uir_sim_get_coverage_count(uir_sim_context_t *ctx);

/* Get coverage entry by index. Returns 0 on failure. */
int uir_sim_get_coverage_entry(uir_sim_context_t *ctx, size_t idx,
                                const char **file, uint32_t *line);

/* Reset all coverage tracking */
void uir_sim_reset_coverage(uir_sim_context_t *ctx);

/* ── System task query API ── */

/* Check if $stop was called (pauses simulation, can continue). */
int uir_sim_is_stopped(uir_sim_context_t *ctx);

/* Check if $finish or $fatal was called (terminates simulation). */
int uir_sim_is_finished(uir_sim_context_t *ctx);

/* Clear the stop flag to allow simulation to continue via another uir_sim_run call. */
void uir_sim_clear_stop(uir_sim_context_t *ctx);

/* ── Checkpoint save/restore ── */

/* Save simulation state to a binary blob.
 * Returns malloc'd buffer with serialized state. Caller must free with free().
 * *size_out is set to the buffer size in bytes. Returns NULL on failure. */
void *uir_sim_save(uir_sim_context_t *ctx, size_t *size_out);

/* Restore simulation state from a binary blob.
 * Creates a new simulation context with saved state. The design units must be
 * identical to those used when the checkpoint was saved.
 * Returns NULL on failure. Caller owns the returned context. */
uir_sim_context_t *uir_sim_restore(const void *data, size_t size,
                                    uir_design_unit_t **units, size_t unit_count);

/* Diff two checkpoints and return a JSON string with signal-level differences.
 * Returns malloc'd JSON string (caller must free) or NULL on failure. */
char *uir_sim_diff(const void *data_a, size_t size_a,
                    const void *data_b, size_t size_b,
                    uir_design_unit_t **units, size_t unit_count);

/* Trace signal driver dependencies.
 * Given a signal name, returns a JSON tree of signals that drive it.
 * max_depth limits recursion depth (0 = direct drivers only).
 * Returns malloc'd JSON string (caller must free) or NULL on failure. */
char *uir_sim_trace_drivers(uir_sim_context_t *ctx, const char *signal,
                             size_t max_depth);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_UIR_SIM_H */
