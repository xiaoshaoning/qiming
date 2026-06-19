#include "libqsim/scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Time wheel configuration ── */

#define QSIM_WHEEL_BITS    8
#define QSIM_WHEEL_SIZE    (1 << QSIM_WHEEL_BITS)
#define QSIM_WHEEL_MASK    (QSIM_WHEEL_SIZE - 1)

/* ── Internal event node (linked list) ── */

typedef struct qsim_event_node {
    qsim_event_t event;
    struct qsim_event_node *next;
} qsim_event_node_t;

static qsim_event_node_t *event_node_alloc(qsim_event_kind_t kind, uint32_t signal_id,
                                            qsim_value_t new_value, uint64_t time,
                                            uint32_t delta)
{
    qsim_event_node_t *n = calloc(1, sizeof(qsim_event_node_t));
    if (!n) return NULL;
    n->event.kind = kind;
    n->event.time = time;
    n->event.delta = delta;
    n->event.signal_id = signal_id;
    n->event.new_value = new_value;
    n->event.message = NULL;
    return n;
}

/* ── Time wheel ── */

typedef struct qsim_time_wheel {
    qsim_event_node_t *slots[QSIM_WHEEL_SIZE];
    uint64_t current_time;
} qsim_time_wheel_t;

static void wheel_init(qsim_time_wheel_t *wheel)
{
    memset(wheel->slots, 0, sizeof(wheel->slots));
    wheel->current_time = 0;
}

static void wheel_push(qsim_time_wheel_t *wheel, qsim_event_node_t *node)
{
    uint64_t t = node->event.time;
    size_t slot = (size_t)(t & QSIM_WHEEL_MASK);
    node->next = wheel->slots[slot];
    wheel->slots[slot] = node;
}

static qsim_event_node_t *wheel_pop_slot(qsim_time_wheel_t *wheel, uint64_t time)
{
    size_t slot = (size_t)(time & QSIM_WHEEL_MASK);
    qsim_event_node_t *head = wheel->slots[slot];
    wheel->slots[slot] = NULL;
    return head;
}

/* ── Scheduler struct ── */

struct qsim_scheduler {
    qsim_time_wheel_t wheel;

    /* Delta queue: events in current time + delta */
    qsim_event_node_t *delta_queue;
    qsim_event_node_t *delta_queue_tail;

    /* Overflow: events beyond wheel horizon */
    qsim_event_node_t *overflow;
    uint64_t overflow_min_time;

    /* NBA pending list */
    qsim_event_node_t *nba_pending;
    qsim_event_node_t *nba_pending_tail;

    /* Current state */
    uint64_t current_time;
    uint32_t current_delta;
    int is_done;

    /* Limits */
    uint64_t max_delta_limit;
    uint64_t event_count;

    /* Callback */
    qsim_event_callback_t callback;
    void *user_data;
};

qsim_scheduler_t *qsim_scheduler_create(void)
{
    qsim_scheduler_t *sched = calloc(1, sizeof(qsim_scheduler_t));
    if (!sched) return NULL;
    wheel_init(&sched->wheel);
    sched->current_time = 0;
    sched->current_delta = 0;
    sched->is_done = 0;
    sched->max_delta_limit = 10000;
    sched->event_count = 0;
    return sched;
}

void qsim_scheduler_destroy(qsim_scheduler_t *sched)
{
    if (!sched) return;

    /* Free all remaining event nodes */
    for (int i = 0; i < QSIM_WHEEL_SIZE; i++) {
        qsim_event_node_t *n = sched->wheel.slots[i];
        while (n) {
            qsim_event_node_t *next = n->next;
            free(n);
            n = next;
        }
    }

    qsim_event_node_t *n = sched->delta_queue;
    while (n) {
        qsim_event_node_t *next = n->next;
        free(n);
        n = next;
    }

    n = sched->overflow;
    while (n) {
        qsim_event_node_t *next = n->next;
        free(n);
        n = next;
    }

    n = sched->nba_pending;
    while (n) {
        qsim_event_node_t *next = n->next;
        free(n);
        n = next;
    }

    free(sched);
}

/* ── Insert into delta queue (sorted by delta) ── */

static void delta_queue_insert(qsim_scheduler_t *sched, qsim_event_node_t *node)
{
    node->next = NULL;

    if (!sched->delta_queue) {
        sched->delta_queue = node;
        sched->delta_queue_tail = node;
        return;
    }

    /* Insert at end (we process in FIFO order within same delta) */
    sched->delta_queue_tail->next = node;
    sched->delta_queue_tail = node;
}

/* ── Insert into overflow sorted by time ── */

static void overflow_insert(qsim_scheduler_t *sched, qsim_event_node_t *node)
{
    node->next = NULL;

    if (!sched->overflow) {
        sched->overflow = node;
        sched->overflow_min_time = node->event.time;
        return;
    }

    /* Insert in time order */
    if (node->event.time < sched->overflow_min_time) {
        sched->overflow_min_time = node->event.time;
    }

    qsim_event_node_t *prev = NULL;
    qsim_event_node_t *cur = sched->overflow;
    while (cur && cur->event.time <= node->event.time) {
        prev = cur;
        cur = cur->next;
    }
    if (prev) {
        node->next = prev->next;
        prev->next = node;
    } else {
        node->next = sched->overflow;
        sched->overflow = node;
    }
}

/* ── Move events from wheel to delta queue ── */

static void wheel_to_delta(qsim_scheduler_t *sched)
{
    qsim_event_node_t *node = wheel_pop_slot(&sched->wheel, sched->current_time);
    while (node) {
        qsim_event_node_t *next = node->next;
        node->next = NULL;
        delta_queue_insert(sched, node);
        node = next;
    }
}

/* ── Move events from overflow to wheel ── */

static void overflow_to_wheel(qsim_scheduler_t *sched)
{
    if (!sched->overflow) return;
    if (sched->overflow_min_time > sched->current_time + QSIM_WHEEL_SIZE) return;

    qsim_event_node_t *prev = NULL;
    qsim_event_node_t *cur = sched->overflow;
    while (cur) {
        qsim_event_node_t *next = cur->next;
        if (cur->event.time <= sched->current_time + QSIM_WHEEL_SIZE) {
            if (prev)
                prev->next = next;
            else
                sched->overflow = next;
            cur->next = NULL;
            wheel_push(&sched->wheel, cur);
        } else {
            prev = cur;
        }
        cur = next;
    }

    /* Update overflow min time */
    sched->overflow_min_time = sched->overflow ? sched->overflow->event.time : 0;
}

/* ── Schedule a signal update ── */

void qsim_sched_signal_update(qsim_scheduler_t *sched, uint32_t signal_id,
                               qsim_value_t new_value, uint64_t time,
                               uint32_t delta_offset)
{
    uint32_t delta = (time == sched->current_time)
                     ? sched->current_delta + delta_offset
                     : delta_offset;

    qsim_event_node_t *node = event_node_alloc(QSIM_EVENT_SIGNAL_UPDATE,
                                                signal_id, new_value, time, delta);
    if (!node) return;

    uint64_t now = sched->current_time;

    if (time == now) {
        /* Same time: go to delta queue */
        delta_queue_insert(sched, node);
    } else if (time <= now + QSIM_WHEEL_SIZE) {
        /* Within wheel horizon */
        wheel_push(&sched->wheel, node);
    } else {
        /* Beyond horizon: overflow queue */
        overflow_insert(sched, node);
    }
}

void qsim_sched_nba_update(qsim_scheduler_t *sched, uint32_t signal_id,
                            qsim_value_t new_value, uint64_t time)
{
    qsim_event_node_t *node = event_node_alloc(QSIM_EVENT_NBA_UPDATE,
                                                signal_id, new_value, time, 0);
    if (!node) return;

    /* Append to NBA pending list */
    node->next = NULL;
    if (sched->nba_pending_tail) {
        sched->nba_pending_tail->next = node;
        sched->nba_pending_tail = node;
    } else {
        sched->nba_pending = node;
        sched->nba_pending_tail = node;
    }
}

/* ── Process a single event ── */

static void process_event(qsim_scheduler_t *sched, qsim_event_node_t *node)
{
    sched->event_count++;

    if (sched->callback) {
        qsim_event_t ev = node->event;
        ev.old_value = node->event.new_value; /* simplified */
        sched->callback(&ev, sched->user_data);
    }
}

/* ── Run simulation ── */

void qsim_scheduler_run(qsim_scheduler_t *sched, uint64_t until_time)
{
    if (sched->is_done) return;

    while (!sched->is_done) {
        /* Advance time if delta queue is empty */
        if (!sched->delta_queue) {
            /* Check NBA pending */
            if (sched->nba_pending) {
                /* Process NBA updates as a new delta */
                sched->current_delta++;
            } else {
                /* Move to next time */
                overflow_to_wheel(sched);

                /* Find next non-empty slot in wheel */
                int found = 0;
                for (uint64_t t = sched->current_time + 1; t <= sched->current_time + QSIM_WHEEL_SIZE; t++) {
                    size_t slot = (size_t)(t & QSIM_WHEEL_MASK);
                    if (sched->wheel.slots[slot] || sched->overflow) {
                        sched->current_time = t;

                        /* Check overflow for events at or before this time */
                        overflow_to_wheel(sched);

                        sched->current_delta = 0;
                        found = 1;
                        break;
                    }
                    /* If no events in this slot, skip ahead */
                }

                if (!found && !sched->overflow) {
                    sched->is_done = 1;
                    break;
                }

                if (!found) {
                    sched->current_time = sched->overflow_min_time;
                    overflow_to_wheel(sched);
                    sched->current_delta = 0;
                }
            }

            if (until_time > 0 && sched->current_time >= until_time) {
                sched->is_done = 1;
                break;
            }

            /* Move events from wheel to delta queue */
            wheel_to_delta(sched);

            /* Move NBA updates to delta queue */
            if (sched->nba_pending) {
                qsim_event_node_t *nba = sched->nba_pending;
                sched->nba_pending = NULL;
                sched->nba_pending_tail = NULL;
                while (nba) {
                    qsim_event_node_t *next = nba->next;
                    nba->next = NULL;
                    nba->event.delta = sched->current_delta;
                    delta_queue_insert(sched, nba);
                    nba = next;
                }
            }
        } else {
            /* Process events in current delta queue */
            qsim_event_node_t *node = sched->delta_queue;
            sched->delta_queue = node->next;
            if (!sched->delta_queue) sched->delta_queue_tail = NULL;
            node->next = NULL;

            process_event(sched, node);
            free(node);

            /* Check delta cycle limit */
            if (sched->current_delta > sched->max_delta_limit) {
                if (sched->callback) {
                    qsim_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.kind = QSIM_EVENT_TIMEOUT;
                    ev.time = sched->current_time;
                    ev.delta = sched->current_delta;
                    ev.message = "zero-delay loop detected: max delta cycles exceeded";
                    sched->callback(&ev, sched->user_data);
                }
                sched->is_done = 1;
                break;
            }
        }
    }
}

int qsim_scheduler_step_delta(qsim_scheduler_t *sched)
{
    if (sched->is_done) return 0;

    /* Ensure we have a delta queue */
    if (!sched->delta_queue) {
        sched_run: {
            overflow_to_wheel(sched);

            /* Find next time */
            int found = 0;
            for (uint64_t t = sched->current_time;; t++) {
                size_t slot = (size_t)(t & QSIM_WHEEL_MASK);
                if (sched->wheel.slots[slot]) {
                    sched->current_time = t;
                    sched->current_delta = 0;
                    found = 1;
                    break;
                }
                if (t > sched->current_time + QSIM_WHEEL_SIZE) break;
            }

            if (!found && !sched->nba_pending && !sched->overflow) {
                sched->is_done = 1;
                return 0;
            }

            if (!found && sched->overflow) {
                sched->current_time = sched->overflow_min_time;
                sched->current_delta = 0;
                overflow_to_wheel(sched);
            }

            wheel_to_delta(sched);

            if (sched->nba_pending) {
                qsim_event_node_t *nba = sched->nba_pending;
                sched->nba_pending = NULL;
                sched->nba_pending_tail = NULL;
                while (nba) {
                    qsim_event_node_t *next = nba->next;
                    nba->next = NULL;
                    delta_queue_insert(sched, nba);
                    nba = next;
                }
            }

            if (!sched->delta_queue)
                goto sched_run;
        }
    }

    /* Process one delta: all events at current (time, delta) */
    if (!sched->delta_queue) return 0;

    qsim_event_node_t *node = sched->delta_queue;
    sched->delta_queue = node->next;
    if (!sched->delta_queue) sched->delta_queue_tail = NULL;
    node->next = NULL;

    process_event(sched, node);
    free(node);

    sched->current_delta++;

    if (sched->current_delta > sched->max_delta_limit) {
        sched->is_done = 1;
        return 0;
    }

    return 1;
}

int qsim_scheduler_step_time(qsim_scheduler_t *sched)
{
    if (sched->is_done) return 0;

    /* Process all remaining events at current time */
    while (sched->delta_queue) {
        qsim_event_node_t *node = sched->delta_queue;
        sched->delta_queue = node->next;
        if (!sched->delta_queue) sched->delta_queue_tail = NULL;
        node->next = NULL;
        process_event(sched, node);
        free(node);
    }

    /* Process NBA updates as delta cycles within current time */
    if (sched->nba_pending) {
        sched->current_delta++;
        qsim_event_node_t *nba = sched->nba_pending;
        sched->nba_pending = NULL;
        sched->nba_pending_tail = NULL;
        while (nba) {
            qsim_event_node_t *next = nba->next;
            nba->next = NULL;
            nba->event.delta = sched->current_delta;
            delta_queue_insert(sched, nba);
            nba = next;
        }
        return 1;
    }

    /* Scan forward for the next time with events */
    overflow_to_wheel(sched);

    uint64_t max_scan = sched->current_time + QSIM_WHEEL_SIZE;
    for (uint64_t t = sched->current_time + 1; t <= max_scan; t++) {
        size_t slot = (size_t)(t & QSIM_WHEEL_MASK);
        if (sched->wheel.slots[slot]) {
            sched->current_time = t;
            sched->current_delta = 0;
            wheel_to_delta(sched);
            return 1;
        }
    }

    /* Check overflow */
    if (sched->overflow) {
        sched->current_time = sched->overflow_min_time;
        sched->current_delta = 0;
        overflow_to_wheel(sched);
        wheel_to_delta(sched);
        return 1;
    }

    sched->is_done = 1;
    return 0;
}

/* ── Callback ── */

void qsim_scheduler_set_callback(qsim_scheduler_t *sched,
                                  qsim_event_callback_t cb, void *user_data)
{
    sched->callback = cb;
    sched->user_data = user_data;
}

/* ── Query ── */

uint64_t qsim_scheduler_current_time(const qsim_scheduler_t *sched)
{
    return sched->current_time;
}

uint32_t qsim_scheduler_current_delta(const qsim_scheduler_t *sched)
{
    return sched->current_delta;
}

int qsim_scheduler_is_done(const qsim_scheduler_t *sched)
{
    return sched->is_done;
}

uint64_t qsim_scheduler_event_count(const qsim_scheduler_t *sched)
{
    return sched->event_count;
}

void qsim_scheduler_set_max_delta(qsim_scheduler_t *sched, uint64_t max_delta)
{
    sched->max_delta_limit = max_delta;
}
