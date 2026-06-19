#include "libqsim/scheduler.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    qsim_event_t events[256];
    size_t count;
} event_collector_t;

static void collector_callback(const qsim_event_t *event, void *user_data)
{
    event_collector_t *col = (event_collector_t *)user_data;
    if (col->count < 256)
        col->events[col->count++] = *event;
}

static void test_create_destroy(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    mu_assert_not_null(sched);
    mu_assert_eq(qsim_scheduler_current_time(sched), 0, "time 0");
    mu_assert_eq(qsim_scheduler_current_delta(sched), 0, "delta 0");
    mu_assert(!qsim_scheduler_is_done(sched), "not done");
    qsim_scheduler_destroy(sched);
}

static void test_single_event(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};

    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_value_t v1 = QSIM_VAL_1;
    qsim_sched_signal_update(sched, 0, v1, 10, 0);
    qsim_scheduler_run(sched, 20);

    mu_assert_eq(qsim_scheduler_current_time(sched), 10, "time=10");
    mu_assert(col.count >= 1, "event recorded");
    mu_assert(col.events[0].kind == QSIM_EVENT_SIGNAL_UPDATE, "signal update");
    mu_assert_eq(col.events[0].time, 10, "event at time 10");

    qsim_scheduler_destroy(sched);
}

static void test_two_events_diff_time(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 5, 0);
    qsim_sched_signal_update(sched, 0, QSIM_VAL_0, 10, 0);
    qsim_scheduler_run(sched, 0);

    mu_assert_eq(col.count, 2, "2 events");
    mu_assert_eq(col.events[0].time, 5, "first at 5");
    mu_assert_eq(col.events[1].time, 10, "second at 10");

    qsim_scheduler_destroy(sched);
}

static void test_events_same_time_diff_delta(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 0, 0);
    qsim_sched_signal_update(sched, 1, QSIM_VAL_1, 0, 1);
    qsim_scheduler_run(sched, 1);

    mu_assert_eq(col.count, 2, "2 delta events");

    qsim_scheduler_destroy(sched);
}

static void test_nba_update(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_nba_update(sched, 0, QSIM_VAL_1, 0);
    qsim_scheduler_run(sched, 1);

    mu_assert(col.count >= 1, "NBA recorded");
    mu_assert(col.events[0].kind == QSIM_EVENT_NBA_UPDATE, "NBA event");

    qsim_scheduler_destroy(sched);
}

static void test_step_delta(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 0, 0);
    int result = qsim_scheduler_step_delta(sched);

    mu_assert(result != 0, "step returned event");
    mu_assert(col.count >= 1, "event during step");

    qsim_scheduler_destroy(sched);
}

static void test_step_time(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 5, 0);
    int result = qsim_scheduler_step_time(sched);

    mu_assert(result != 0, "step time advanced");

    qsim_scheduler_destroy(sched);
}

static void test_max_delta_limit(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);
    qsim_scheduler_set_max_delta(sched, 5);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 0, 0);
    qsim_scheduler_run(sched, 10);

    mu_assert(col.count >= 1, "events processed");

    qsim_scheduler_destroy(sched);
}

static void test_event_count(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    mu_assert_eq(qsim_scheduler_event_count(sched), 0, "0 events initially");

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 5, 0);
    qsim_scheduler_run(sched, 10);

    mu_assert(qsim_scheduler_event_count(sched) >= 1, "events counted");

    qsim_scheduler_destroy(sched);
}

static void test_schedule_at_current_time(void)
{
    qsim_scheduler_t *sched = qsim_scheduler_create();
    event_collector_t col = {0};
    qsim_scheduler_set_callback(sched, collector_callback, &col);

    qsim_sched_signal_update(sched, 0, QSIM_VAL_1, 0, 0);
    qsim_scheduler_run(sched, 1);

    mu_assert(col.count >= 1, "time 0 event processed");

    qsim_scheduler_destroy(sched);
}

void register_scheduler_tests(void)
{
    printf("[Scheduler]\n");
    mu_run_test(test_create_destroy);
    mu_run_test(test_single_event);
    mu_run_test(test_two_events_diff_time);
    mu_run_test(test_events_same_time_diff_delta);
    mu_run_test(test_nba_update);
    mu_run_test(test_step_delta);
    mu_run_test(test_step_time);
    mu_run_test(test_max_delta_limit);
    mu_run_test(test_event_count);
    mu_run_test(test_schedule_at_current_time);
    printf("\n");
}
