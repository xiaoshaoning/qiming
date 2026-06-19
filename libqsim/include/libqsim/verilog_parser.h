#ifndef LIBDSIM_VERILOG_PARSER_H
#define LIBDSIM_VERILOG_PARSER_H

#include "libqsim/uir.h"
#include "libqsim/verilog_lexer.h"

/* Maximum number of error messages */
#define PARSE_MAX_ERRORS 16

/* Parser error */
typedef struct {
    uint32_t line;
    uint32_t column;
    char message[256];
} parse_error_t;

/* Parser result */
typedef struct {
    uir_design_unit_t *unit;      /* first unit (backward compat) */
    uir_design_unit_t **units;    /* all parsed units (caller must free each unit) */
    size_t unit_count;
    int success;
    int error_count;
    parse_error_t errors[PARSE_MAX_ERRORS];
} parse_result_t;

/* Parse a Verilog source string. Returns all parsed units on success. */
parse_result_t verilog_parse(const char *filename, const char *source, size_t length);

/* Parse a Verilog source from a file path. */
parse_result_t verilog_parse_file(const char *filename);

/* Parse a Verilog file with additional preprocessor include paths.
 * include_paths is an array of strings; include_path_count is its length.
 * The file's own directory is always searched first. */
parse_result_t verilog_parse_file_ex(const char *filename,
                                      const char **include_paths,
                                      int include_path_count);

#endif /* LIBDSIM_VERILOG_PARSER_H */
