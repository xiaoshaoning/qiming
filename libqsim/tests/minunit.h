/* Minimal unit test framework for C — no external dependencies. */

#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Test state — extern counters (defined in main_test.c), static per-file helpers */
extern int mu_tests_run;
extern int mu_tests_passed;
extern int mu_tests_failed;
static const char *mu_current_test = "";
static int mu_assert_failed = 0;

/* Assertions */
#define mu_assert(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s at %s:%d: %s\n", mu_current_test, __FILE__, __LINE__, msg); \
        mu_assert_failed = 1; \
        mu_tests_failed++; \
        return; \
    } \
} while(0)

#define mu_assert_eq(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL: %s at %s:%d: %s (%lld != %lld)\n", \
                mu_current_test, __FILE__, __LINE__, msg, \
                (long long)(a), (long long)(b)); \
        mu_assert_failed = 1; \
        mu_tests_failed++; \
        return; \
    } \
} while(0)

#define mu_assert_str_eq(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "  FAIL: %s at %s:%d: %s (\"%s\" != \"%s\")\n", \
                mu_current_test, __FILE__, __LINE__, msg, (a), (b)); \
        mu_assert_failed = 1; \
        mu_tests_failed++; \
        return; \
    } \
} while(0)

#define mu_assert_ptr_not_null(p, msg) do { \
    if ((p) == NULL) { \
        fprintf(stderr, "  FAIL: %s at %s:%d: %s (expected non-null)\n", \
                mu_current_test, __FILE__, __LINE__, msg); \
        mu_assert_failed = 1; \
        mu_tests_failed++; \
        return; \
    } \
} while(0)

#define mu_assert_ptr_null(p, msg) do { \
    if ((p) != NULL) { \
        fprintf(stderr, "  FAIL: %s at %s:%d: %s (expected null)\n", \
                mu_current_test, __FILE__, __LINE__, msg); \
        mu_assert_failed = 1; \
        mu_tests_failed++; \
        return; \
    } \
} while(0)

#define mu_assert_not_null(p) mu_assert_ptr_not_null(p, "pointer should not be NULL")
#define mu_assert_null(p) mu_assert_ptr_null(p, "pointer should be NULL")

/* Run a test function */
#define mu_run_test(test) do { \
    mu_current_test = #test; \
    mu_assert_failed = 0; \
    mu_tests_run++; \
    test(); \
    if (!mu_assert_failed) { \
        mu_tests_passed++; \
        printf("  PASS: %s\n", #test); \
    } \
} while(0)

/* Print summary — call at end of main */
#define mu_summary() do { \
    printf("========================================\n"); \
    printf("Results: %d run, %d passed, %d failed\n", \
           mu_tests_run, mu_tests_passed, mu_tests_failed); \
} while(0)

#endif /* MINUNIT_H */
