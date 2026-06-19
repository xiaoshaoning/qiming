#ifndef LIBDSIM_SCHEDULER_H
#define LIBDSIM_SCHEDULER_H

#include "libqsim/value.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds */
typedef enum qsim_event_kind {
    QSIM_EVENT_SIGNAL_UPDATE = 0,
    QSIM_EVENT_PROCESS_WAKE  = 1,
    QSIM_EVENT_ASSERTION     = 2,
    QSIM_EVENT_TIMEOUT       = 3,
    QSIM_EVENT_BREAK         = 4,
    QSIM_EVENT_NBA_UPDATE    = 5,
} qsim_event_kind_t;

/* A single simulation event (external view, passed to callback) */
typedef struct qsim_event {
    qsim_event_kind_t kind;
    uint64_t time;
    uint32_t delta;
    uint32_t signal_id;
    qsim_value_t old_value;
    qsim_value_t new_value;
    const char *message;
} qsim_event_t;

/* Event callback */
typedef void (*qsim_event_callback_t)(const qsim_event_t *event, void *user_data);

/* Opaque scheduler handle */
typedef struct qsim_scheduler qsim_scheduler_t;

/* ── Lifecycle ── */

qsim_scheduler_t *qsim_scheduler_create(void);
void qsim_scheduler_destroy(qsim_scheduler_t *sched);

/* ── Scheduling ── */

/* Schedule a signal update at (time, current_delta + delta_offset). */
void qsim_sched_signal_update(qsim_scheduler_t *sched, uint32_t signal_id,
                               qsim_value_t new_value, uint64_t time,
                               uint32_t delta_offset);

/* Schedule a non-blocking assignment update. */
void qsim_sched_nba_update(qsim_scheduler_t *sched, uint32_t signal_id,
                            qsim_value_t new_value, uint64_t time);

/* ── Simulation control ── */

/* Run simulation until until_time (0 = run to completion). */
void qsim_scheduler_run(qsim_scheduler_t *sched, uint64_t until_time);

/* Step by one delta cycle. Returns 0 when simulation is done. */
int qsim_scheduler_step_delta(qsim_scheduler_t *sched);

/* Step by one time unit. Returns 0 when simulation is done. */
int qsim_scheduler_step_time(qsim_scheduler_t *sched);

/* ── Callback ── */

void qsim_scheduler_set_callback(qsim_scheduler_t *sched,
                                  qsim_event_callback_t cb, void *user_data);

/* ── Query ── */

uint64_t qsim_scheduler_current_time(const qsim_scheduler_t *sched);
uint32_t qsim_scheduler_current_delta(const qsim_scheduler_t *sched);
int qsim_scheduler_is_done(const qsim_scheduler_t *sched);
uint64_t qsim_scheduler_event_count(const qsim_scheduler_t *sched);

/* Set maximum delta cycles before detecting zero-delay loop (default 10000). */
void qsim_scheduler_set_max_delta(qsim_scheduler_t *sched, uint64_t max_delta);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_SCHEDULER_H */
