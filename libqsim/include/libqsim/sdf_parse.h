#ifndef LIBSIM_SDF_PARSE_H
#define LIBSIM_SDF_PARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SDF IOPATH entry — one path delay override */
typedef struct sdf_iopath {
    char *src_pin;      /* source pin name */
    char *dst_pin;      /* destination pin name */
    uint64_t rise_delay; /* rise delay in sim time units */
    uint64_t fall_delay; /* fall delay in sim time units */
} sdf_iopath_t;

/* SDF cell entry — maps to a module instance */
typedef struct sdf_cell {
    char *instance;      /* hierarchical instance path (e.g. "top.u1") */
    sdf_iopath_t *iopaths;
    size_t iopath_count;
    size_t iopath_cap;   /* internal: allocated capacity */
} sdf_cell_t;

/* Parsed SDF file */
typedef struct sdf_file {
    sdf_cell_t *cells;
    size_t cell_count;
    char divider;       /* hierarchy separator: '.' or '/' */
    uint64_t scale;     /* timescale multiplier for sim time unit conversion */
} sdf_file_t;

/* Parse an SDF file. Returns NULL on failure (file not found, parse error). */
sdf_file_t *sdf_parse_file(const char *filepath);

/* Free a parsed SDF file and all its contents. */
void sdf_file_free(sdf_file_t *sdf);

#ifdef __cplusplus
}
#endif

#endif /* LIBSIM_SDF_PARSE_H */
