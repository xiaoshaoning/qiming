#include "libqsim/logger.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

static void test_create_destroy(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    mu_assert_not_null(logger);
    mu_assert_eq(qsim_logger_entry_count(logger), 0, "0 entries");
    qsim_logger_destroy(logger);
}

static void test_log_info(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    qsim_logger_info(logger, 0, 0, "simulation started");
    mu_assert_eq(qsim_logger_entry_count(logger), 1, "1 entry");
    qsim_logger_destroy(logger);
}

static void test_log_severity_counts(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    qsim_logger_info(logger, 0, 0, "info");
    qsim_logger_warn(logger, 5, 0, "warning");
    qsim_logger_error(logger, 10, 0, "error");

    mu_assert_eq(qsim_logger_entry_count(logger), 3, "3 entries");
    mu_assert_eq(qsim_logger_warning_count(logger), 1, "1 warning");
    mu_assert_eq(qsim_logger_error_count(logger), 1, "1 error");
    qsim_logger_destroy(logger);
}

static void test_to_json(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    qsim_logger_info(logger, 0, 0, "start");
    qsim_logger_error(logger, 10, 0, "fail");

    char *json = qsim_logger_to_json(logger);
    mu_assert_not_null(json);
    mu_assert(strstr(json, "start") != NULL, "has start");
    mu_assert(strstr(json, "fail") != NULL, "has fail");
    mu_assert(strstr(json, "info") != NULL, "has info");
    mu_assert(strstr(json, "error") != NULL, "has error");
    free(json);

    qsim_logger_destroy(logger);
}

static void test_clear(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    qsim_logger_info(logger, 0, 0, "msg");
    mu_assert_eq(qsim_logger_entry_count(logger), 1, "before clear");

    qsim_logger_clear(logger);
    mu_assert_eq(qsim_logger_entry_count(logger), 0, "after clear");
    qsim_logger_destroy(logger);
}

static void test_empty_json(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    char *json = qsim_logger_to_json(logger);
    mu_assert_not_null(json);
    mu_assert_str_eq(json, "[]", "empty array");
    free(json);
    qsim_logger_destroy(logger);
}

static void test_log_with_time_delta(void)
{
    qsim_logger_t *logger = qsim_logger_create();
    qsim_logger_info(logger, 100, 5, "event at time");

    char *json = qsim_logger_to_json(logger);
    mu_assert_not_null(json);
    mu_assert(strstr(json, "100") != NULL, "time 100");
    mu_assert(strstr(json, "5") != NULL, "delta 5");
    free(json);
    qsim_logger_destroy(logger);
}

void register_logger_tests(void)
{
    printf("[Logger]\n");
    mu_run_test(test_create_destroy);
    mu_run_test(test_log_info);
    mu_run_test(test_log_severity_counts);
    mu_run_test(test_to_json);
    mu_run_test(test_clear);
    mu_run_test(test_empty_json);
    mu_run_test(test_log_with_time_delta);
    printf("\n");
}
