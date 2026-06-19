#include "libqsim/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

struct qsim_logger {
    qsim_log_entry_t *entries;
    size_t count;
    size_t capacity;
};

qsim_logger_t *qsim_logger_create(void)
{
    qsim_logger_t *logger = calloc(1, sizeof(qsim_logger_t));
    if (!logger) return NULL;
    logger->capacity = 64;
    logger->entries = calloc(logger->capacity, sizeof(qsim_log_entry_t));
    if (!logger->entries) {
        free(logger);
        return NULL;
    }
    return logger;
}

void qsim_logger_destroy(qsim_logger_t *logger)
{
    if (!logger) return;
    for (size_t i = 0; i < logger->count; i++)
        free(logger->entries[i].message);
    free(logger->entries);
    free(logger);
}

void qsim_logger_log(qsim_logger_t *logger, qsim_log_severity_t severity,
                      uint64_t time, uint32_t delta, const char *message)
{
    if (!logger) return;

    if (logger->count >= logger->capacity) {
        logger->capacity *= 2;
        qsim_log_entry_t *new_entries = realloc(logger->entries,
            logger->capacity * sizeof(qsim_log_entry_t));
        if (!new_entries) return;
        logger->entries = new_entries;
    }

    qsim_log_entry_t *entry = &logger->entries[logger->count++];
    entry->severity = severity;
    entry->time = time;
    entry->delta = delta;
    entry->message = strdup(message ? message : "");
}

void qsim_logger_info(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                       const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    qsim_logger_log(logger, QSIM_LOG_INFO, time, delta, buf);
}

void qsim_logger_warn(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                       const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    qsim_logger_log(logger, QSIM_LOG_WARNING, time, delta, buf);
}

void qsim_logger_error(qsim_logger_t *logger, uint64_t time, uint32_t delta,
                        const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    qsim_logger_log(logger, QSIM_LOG_ERROR, time, delta, buf);
}

char *qsim_logger_to_json(const qsim_logger_t *logger)
{
    if (!logger || logger->count == 0) {
        char *empty = malloc(3);
        if (empty) strcpy(empty, "[]");
        return empty;
    }

    /* Estimate total size: rough */
    size_t est = 2; /* [ */
    for (size_t i = 0; i < logger->count; i++) {
        if (i > 0) est++; /* comma or newline */
        est += 80 + (logger->entries[i].message ? strlen(logger->entries[i].message) : 0);
    }
    est += 2; /* ]\n */

    char *json = malloc(est);
    if (!json) return NULL;
    char *p = json;
    size_t remaining = est;

#define APPEND(...) do { \
    int n = snprintf(p, remaining, __VA_ARGS__); \
    if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; } \
} while(0)

    APPEND("[");
    for (size_t i = 0; i < logger->count; i++) {
        if (i > 0) APPEND(",");
        const qsim_log_entry_t *e = &logger->entries[i];
        const char *sev = "info";
        switch (e->severity) {
            case QSIM_LOG_INFO:      sev = "info"; break;
            case QSIM_LOG_WARNING:   sev = "warning"; break;
            case QSIM_LOG_ERROR:     sev = "error"; break;
            case QSIM_LOG_ASSERTION: sev = "assertion"; break;
        }
        APPEND("{\"severity\":\"%s\",\"time\":%llu,\"delta\":%u,\"message\":\"",
               sev, (unsigned long long)e->time, e->delta);
        /* Escape message */
        for (const char *m = e->message; *m; m++) {
            if (*m == '"' || *m == '\\') {
                if (remaining > 2) { *p++ = '\\'; remaining--; }
            }
            if (remaining > 1) { *p++ = *m; remaining--; }
        }
        APPEND("\"}");
    }
    APPEND("]");
    *p = '\0';

#undef APPEND

    return json;
}

size_t qsim_logger_entry_count(const qsim_logger_t *logger)
{
    return logger ? logger->count : 0;
}

size_t qsim_logger_error_count(const qsim_logger_t *logger)
{
    if (!logger) return 0;
    size_t n = 0;
    for (size_t i = 0; i < logger->count; i++)
        if (logger->entries[i].severity == QSIM_LOG_ERROR ||
            logger->entries[i].severity == QSIM_LOG_ASSERTION)
            n++;
    return n;
}

size_t qsim_logger_warning_count(const qsim_logger_t *logger)
{
    if (!logger) return 0;
    size_t n = 0;
    for (size_t i = 0; i < logger->count; i++)
        if (logger->entries[i].severity == QSIM_LOG_WARNING)
            n++;
    return n;
}

void qsim_logger_clear(qsim_logger_t *logger)
{
    if (!logger) return;
    for (size_t i = 0; i < logger->count; i++)
        free(logger->entries[i].message);
    logger->count = 0;
}
