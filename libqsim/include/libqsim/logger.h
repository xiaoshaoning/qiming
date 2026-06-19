#ifndef LIBDSIM_LOGGER_H
#define LIBDSIM_LOGGER_H

#include "libqsim/value.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log entry severity. */
typedef enum qsim_log_severity {
    QSIM_LOG_INFO,
    QSIM_LOG_WARNING,
    QSIM_LOG_ERROR,
    QSIM_LOG_ASSERTION,
} qsim_log_severity_t;

/* A single log entry. */
typedef struct qsim_log_entry {
    qsim_log_severity_t severity;
    uint64_t time;
    uint32_t delta;
    char *message;
} qsim_log_entry_t;

/* Opaque logger handle. */
typedef struct qsim_logger qsim_logger_t;

qsim_logger_t *qsim_logger_create(void);
void qsim_logger_destroy(qsim_logger_t *logger);

void qsim_logger_log(qsim_logger_t *logger, qsim_log_severity_t severity,
                      uint64_t time, uint32_t delta, const char *message);

/* Convenience wrappers. */
void qsim_logger_info(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                       const char *fmt, ...);
void qsim_logger_warn(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                       const char *fmt, ...);
void qsim_logger_error(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                        const char *fmt, ...);

/* Serialize all entries to a JSON-Lines string (caller must free). */
char *qsim_logger_to_json(const qsim_logger_t *logger);

/* Query summary. */
size_t qsim_logger_entry_count(const qsim_logger_t *logger);
size_t qsim_logger_error_count(const qsim_logger_t *logger);
size_t qsim_logger_warning_count(const qsim_logger_t *logger);

/* Clear all entries. */
void qsim_logger_clear(qsim_logger_t *logger);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_LOGGER_H */
