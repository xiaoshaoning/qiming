#ifndef LIBQSIM_VERILOG_PREPROCESSOR_H
#define LIBQSIM_VERILOG_PREPROCESSOR_H

#include <stddef.h>

/* Text-level Verilog preprocessor for IEEE 1364-2005 compiler directives.
 *
 * Supports: `define, `undef, `ifdef, `ifndef, `elsif, `else, `endif,
 * `include, `timescale, `resetall, `celldefine/`endcelldefine,
 * `default_nettype, `pragma.
 *
 * Macro substitution is performed on the output text. Nested includes
 * are supported. Conditional compilation state is tracked with a stack
 * for arbitrary nesting. */

typedef struct verilog_preprocessor verilog_preprocessor_t;

verilog_preprocessor_t *verilog_preprocessor_create(void);
void verilog_preprocessor_destroy(verilog_preprocessor_t *pp);

/* Add a directory to the include search path. The directory of the
 * source file being processed is always searched first. */
void verilog_preprocessor_add_include_path(verilog_preprocessor_t *pp,
                                           const char *path);

/* Process source text. Returns a malloc'd NUL-terminated string of
 * preprocessed output, or NULL on failure.
 * On failure, call verilog_preprocessor_get_error() for the message.
 * The filename is used for diagnostic messages and include resolution. */
char *verilog_preprocessor_process(verilog_preprocessor_t *pp,
                                    const char *filename,
                                    const char *source, size_t length);

/* Get the last error message. Valid until the next process call. */
const char *verilog_preprocessor_get_error(verilog_preprocessor_t *pp);

/* Convenience: read a file, preprocess it, return expanded output.
 * The file's directory is automatically added as an include path.
 * Returns malloc'd string or NULL on failure. */
char *verilog_preprocessor_process_file(const char *path);

#endif /* LIBQSIM_VERILOG_PREPROCESSOR_H */
