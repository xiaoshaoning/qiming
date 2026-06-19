#include "libqsim/wave_buffer.h"
#include "minunit.h"
#include <stdlib.h>

static void test_create_destroy(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(4, 1024);
    mu_assert_not_null(buf);
    qsim_wave_buffer_destroy(buf);
}

static void test_record_and_query(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(2, 1024);
    mu_assert_not_null(buf);

    qsim_wave_buffer_record(buf, 0, QSIM_VAL_0, 0);
    qsim_wave_buffer_record(buf, 0, QSIM_VAL_1, 10);
    qsim_wave_buffer_record(buf, 0, QSIM_VAL_0, 20);

    qsim_value_t out_vals[16];
    uint64_t out_times[16];

    size_t n = qsim_wave_buffer_query(buf, 0, 0, 25, out_vals, out_times, 16);
    mu_assert_eq(n, 3, "3 transitions");
    mu_assert_eq(out_times[0], 0, "t=0");
    mu_assert(out_vals[0].state == QSIM_0, "val=0");
    mu_assert_eq(out_times[1], 10, "t=10");
    mu_assert(out_vals[1].state == QSIM_1, "val=1");
    mu_assert_eq(out_times[2], 20, "t=20");
    mu_assert(out_vals[2].state == QSIM_0, "val=0");

    qsim_wave_buffer_destroy(buf);
}

static void test_time_window_query(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(1, 1024);

    qsim_wave_buffer_record(buf, 0, QSIM_VAL_1, 5);
    qsim_wave_buffer_record(buf, 0, QSIM_VAL_0, 10);
    qsim_wave_buffer_record(buf, 0, QSIM_VAL_1, 15);
    qsim_wave_buffer_record(buf, 0, QSIM_VAL_0, 20);

    qsim_value_t out_vals[16];
    uint64_t out_times[16];

    size_t n = qsim_wave_buffer_query(buf, 0, 8, 18, out_vals, out_times, 16);
    mu_assert_eq(n, 2, "2 in window");
    mu_assert_eq(out_times[0], 10, "first at 10");
    mu_assert_eq(out_times[1], 15, "second at 15");

    qsim_wave_buffer_destroy(buf);
}

static void test_initial_value(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(1, 1024);

    qsim_value_t init = qsim_wave_buffer_initial(buf, 0);
    mu_assert(init.state == QSIM_X, "initial is X");

    qsim_wave_buffer_record(buf, 0, QSIM_VAL_1, 0);
    init = qsim_wave_buffer_initial(buf, 0);
    mu_assert(init.state == QSIM_1, "initial updated");

    qsim_wave_buffer_destroy(buf);
}

static void test_signal_names(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(2, 64);
    const char *names[] = {"top.clk", "top.rst"};
    qsim_wave_buffer_set_names(buf, names, 2);
    qsim_wave_buffer_destroy(buf);
}

static void test_ring_buffer_wraparound(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(1, 4);

    for (int i = 0; i < 10; i++)
        qsim_wave_buffer_record(buf, 0, (i % 2) ? QSIM_VAL_1 : QSIM_VAL_0, i * 10);

    qsim_value_t out_vals[16];
    uint64_t out_times[16];

    size_t n = qsim_wave_buffer_query(buf, 0, 0, 100, out_vals, out_times, 16);
    mu_assert_eq(n, 4, "only last 4 retained");
    mu_assert_eq(out_times[0], 60, "first retained at 60");

    qsim_wave_buffer_destroy(buf);
}

static void test_multi_signal(void)
{
    qsim_wave_buffer_t *buf = qsim_wave_buffer_create(3, 64);

    qsim_wave_buffer_record(buf, 0, QSIM_VAL_1, 5);
    qsim_wave_buffer_record(buf, 1, QSIM_VAL_0, 5);
    qsim_wave_buffer_record(buf, 2, QSIM_VAL_X, 5);

    qsim_value_t v0[4], v1[4], v2[4];
    uint64_t t0[4], t1[4], t2[4];

    mu_assert_eq(qsim_wave_buffer_query(buf, 0, 0, 10, v0, t0, 4), 1, "sig0 1 event");
    mu_assert_eq(qsim_wave_buffer_query(buf, 1, 0, 10, v1, t1, 4), 1, "sig1 1 event");
    mu_assert_eq(qsim_wave_buffer_query(buf, 2, 0, 10, v2, t2, 4), 1, "sig2 1 event");

    qsim_wave_buffer_destroy(buf);
}

void register_wave_buffer_tests(void)
{
    printf("[Wave Buffer]\n");
    mu_run_test(test_create_destroy);
    mu_run_test(test_record_and_query);
    mu_run_test(test_time_window_query);
    mu_run_test(test_initial_value);
    mu_run_test(test_signal_names);
    mu_run_test(test_ring_buffer_wraparound);
    mu_run_test(test_multi_signal);
    printf("\n");
}
