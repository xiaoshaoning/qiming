#ifndef LIBDSIM_SIMULATOR_H
#define LIBDSIM_SIMULATOR_H

#include <stddef.h>
#include <stdint.h>

/* Structs matching the Rust FFI repr(C) types */

typedef struct {
    const char **files;
    size_t file_count;
    const char **sources;
    size_t source_count;
    const char **library_paths;    /* -y library directories for automatic module lookup */
    size_t library_path_count;
} qsim_compile_input_t;

typedef struct {
    char **suggestions;
    size_t suggestion_count;
    char **nearby;
    size_t nearby_count;
    const char *next_tool;
} qsim_recovery_t;

typedef struct {
    int is_error;
    const char *file;
    uint32_t line;
    uint32_t column;
    const char *message;
    qsim_recovery_t *recovery;   /* NULL if no recovery hints */
} qsim_diagnostic_t;

typedef struct {
    int success;
    void **units;
    size_t unit_count;
    qsim_diagnostic_t *diagnostics;
    size_t diag_count;
} qsim_compile_result_t;

typedef struct {
    int success;
    uint64_t ended_at;
    uint32_t final_delta;
    const char *stop_reason;
} qsim_sim_result_t;

/* Recovery helper — compute name suggestions from a list of valid names.
 * Returns a qsim_recovery_t* with the closest matches (edit distance < 5),
 * or NULL if no close matches found. The caller owns the returned pointer
 * and must free it using free() (the suggestions arrays, but not next_tool
 * which points to a string literal). */
qsim_recovery_t *qsim_recovery_from_names(const char *bad_name,
                                           const char **valid_names,
                                           size_t valid_count,
                                           const char *next_tool);

/* Top-level API */
void qsim_init(void);
void qsim_shutdown(void);
qsim_compile_result_t *qsim_compile(const qsim_compile_input_t *input);
void qsim_compile_result_free(qsim_compile_result_t *result);
qsim_sim_result_t *qsim_sim_run(void *session, uint64_t until_time);
void qsim_sim_result_free(qsim_sim_result_t *result);

#endif /* LIBDSIM_SIMULATOR_H */
