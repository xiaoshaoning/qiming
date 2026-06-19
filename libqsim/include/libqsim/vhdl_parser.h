#ifndef LIBDSIM_VHDL_PARSER_H
#define LIBDSIM_VHDL_PARSER_H

#include "libqsim/uir.h"
#include <stddef.h>
#include <stdint.h>

#define VHDL_PARSE_MAX_ERRORS 16

typedef struct {
    uint32_t line;
    uint32_t column;
    char message[256];
} vhdl_parse_error_t;

typedef struct {
    uir_design_unit_t *unit;
    int success;
    int error_count;
    vhdl_parse_error_t errors[VHDL_PARSE_MAX_ERRORS];
} vhdl_parse_result_t;

vhdl_parse_result_t vhdl_parse(const char *filename, const char *source, size_t length);
vhdl_parse_result_t vhdl_parse_file(const char *filename);

#endif /* LIBDSIM_VHDL_PARSER_H */
