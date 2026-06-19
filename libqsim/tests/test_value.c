#include "libqsim/value.h"
#include "minunit.h"
#include <stdlib.h>
#include <string.h>

static void test_resolve_identical(void)
{
    mu_assert(qsim_resolve(QSIM_0, QSIM_0) == QSIM_0, "0+0 = 0");
    mu_assert(qsim_resolve(QSIM_1, QSIM_1) == QSIM_1, "1+1 = 1");
    mu_assert(qsim_resolve(QSIM_X, QSIM_X) == QSIM_X, "X+X = X");
    mu_assert(qsim_resolve(QSIM_Z, QSIM_Z) == QSIM_Z, "Z+Z = Z");
}

static void test_resolve_z(void)
{
    mu_assert(qsim_resolve(QSIM_Z, QSIM_0) == QSIM_0, "Z+0 = 0");
    mu_assert(qsim_resolve(QSIM_Z, QSIM_1) == QSIM_1, "Z+1 = 1");
    mu_assert(qsim_resolve(QSIM_Z, QSIM_X) == QSIM_X, "Z+X = X");
    mu_assert(qsim_resolve(QSIM_0, QSIM_Z) == QSIM_0, "0+Z = 0");
}

static void test_resolve_0_vs_1(void)
{
    mu_assert(qsim_resolve(QSIM_0, QSIM_1) == QSIM_X, "0+1 = X");
    mu_assert(qsim_resolve(QSIM_1, QSIM_0) == QSIM_X, "1+0 = X");
}

static void test_resolve_x(void)
{
    mu_assert(qsim_resolve(QSIM_X, QSIM_0) == QSIM_X, "X+0 = X");
    mu_assert(qsim_resolve(QSIM_X, QSIM_1) == QSIM_X, "X+1 = X");
    mu_assert(qsim_resolve(QSIM_0, QSIM_X) == QSIM_X, "0+X = X");
}

static void test_resolve_strength(void)
{
    qsim_value_t weak0 = {QSIM_0, QSIM_STRENGTH_WEAK};
    qsim_value_t strong1 = {QSIM_1, QSIM_STRENGTH_STRONG};
    qsim_value_t result = qsim_resolve_strength(weak0, strong1);
    mu_assert(result.state == QSIM_1, "strong wins over weak");
    mu_assert(result.strength == QSIM_STRENGTH_STRONG, "strength preserved");
}

static void test_resolve_strength_equal(void)
{
    qsim_value_t v0 = {QSIM_0, QSIM_STRENGTH_STRONG};
    qsim_value_t v1 = {QSIM_1, QSIM_STRENGTH_STRONG};
    qsim_value_t result = qsim_resolve_strength(v0, v1);
    mu_assert(result.state == QSIM_X, "equal strength 0+1 = X");
}

static void test_resolve_strength_z(void)
{
    qsim_value_t z = QSIM_VAL_Z;
    qsim_value_t s0 = QSIM_VAL_0;
    qsim_value_t result = qsim_resolve_strength(z, s0);
    mu_assert(result.state == QSIM_0, "Z resolved to 0");
}

static void test_bit_vector_alloc_free(void)
{
    qsim_bit_vector_t *v = qsim_bit_vector_alloc(8);
    mu_assert_not_null(v);
    mu_assert_eq(v->width, 8, "width 8");
    qsim_bit_vector_free(v);
}

static void test_bit_get_set(void)
{
    qsim_bit_vector_t *v = qsim_bit_vector_alloc(4);
    mu_assert_not_null(v);

    qsim_bit_set(v, 0, QSIM_VAL_1);
    qsim_bit_set(v, 1, QSIM_VAL_0);
    qsim_bit_set(v, 2, QSIM_VAL_X);

    mu_assert(qsim_bit_get(v, 0).state == QSIM_1, "bit0=1");
    mu_assert(qsim_bit_get(v, 1).state == QSIM_0, "bit1=0");
    mu_assert(qsim_bit_get(v, 2).state == QSIM_X, "bit2=X");
    mu_assert(qsim_bit_get(v, 3).state == QSIM_X, "uninit=X");

    mu_assert(qsim_bit_get(v, 10).state == QSIM_X, "out-of-bounds=X");

    qsim_bit_vector_free(v);
}

static void test_bit_vector_eq(void)
{
    qsim_bit_vector_t *a = qsim_bit_vector_alloc(4);
    qsim_bit_vector_t *b = qsim_bit_vector_alloc(4);
    mu_assert_not_null(a);
    mu_assert_not_null(b);

    mu_assert(qsim_bit_vector_eq(a, b), "both X equal");

    qsim_bit_set(a, 0, QSIM_VAL_1);
    mu_assert(!qsim_bit_vector_eq(a, b), "diff not equal");

    qsim_bit_set(b, 0, QSIM_VAL_1);
    mu_assert(qsim_bit_vector_eq(a, b), "same equal");

    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);

    a = qsim_bit_vector_alloc(4);
    b = qsim_bit_vector_alloc(8);
    mu_assert(!qsim_bit_vector_eq(a, b), "diff width not equal");
    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);
}

static void test_bit_vector_match(void)
{
    qsim_bit_vector_t *a = qsim_bit_vector_from_state(4, QSIM_1);
    qsim_bit_vector_t *b = qsim_bit_vector_from_state(4, QSIM_X);
    mu_assert_not_null(a);
    mu_assert_not_null(b);

    mu_assert(qsim_bit_vector_match(a, b), "X matches anything");
    mu_assert(qsim_bit_vector_match(b, a), "X matches anything reverse");

    qsim_bit_vector_t *c = qsim_bit_vector_from_state(4, QSIM_0);
    mu_assert(!qsim_bit_vector_match(a, c), "1 vs 0 no X -> no match");

    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);
    qsim_bit_vector_free(c);
}

static void test_bit_vector_match_z(void)
{
    qsim_bit_vector_t *a = qsim_bit_vector_from_state(4, QSIM_1);
    qsim_bit_vector_t *b = qsim_bit_vector_from_state(4, QSIM_Z);
    mu_assert_not_null(a);
    mu_assert_not_null(b);

    mu_assert(qsim_bit_vector_match_z(a, b), "Z is don't-care");
    mu_assert(qsim_bit_vector_match_z(b, a), "Z is don't-care reverse");

    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);
}

static void test_bit_vector_from_str(void)
{
    qsim_bit_vector_t *v = qsim_bit_vector_from_str("'b1010");
    mu_assert_not_null(v);
    mu_assert_eq(v->width, 4, "width 4");
    mu_assert(qsim_bit_get(v, 0).state == QSIM_0, "bit0=0");
    mu_assert(qsim_bit_get(v, 1).state == QSIM_1, "bit1=1");
    mu_assert(qsim_bit_get(v, 2).state == QSIM_0, "bit2=0");
    mu_assert(qsim_bit_get(v, 3).state == QSIM_1, "bit3=1");
    qsim_bit_vector_free(v);

    v = qsim_bit_vector_from_str("'hff");
    mu_assert_not_null(v);
    mu_assert_eq(v->width, 8, "hex width 8");
    for (uint32_t i = 0; i < 8; i++)
        mu_assert(qsim_bit_get(v, i).state == QSIM_1, "hex all 1");
    qsim_bit_vector_free(v);

    v = qsim_bit_vector_from_str("'b10xz");
    mu_assert_not_null(v);
    mu_assert_eq(v->width, 4, "x z width");
    mu_assert(qsim_bit_get(v, 0).state == QSIM_Z, "bit0=Z");
    mu_assert(qsim_bit_get(v, 1).state == QSIM_X, "bit1=X");
    mu_assert(qsim_bit_get(v, 2).state == QSIM_0, "bit2=0");
    mu_assert(qsim_bit_get(v, 3).state == QSIM_1, "bit3=1");
    qsim_bit_vector_free(v);
}

static void test_bit_vector_to_str(void)
{
    qsim_bit_vector_t *v = qsim_bit_vector_from_str("'b1010");
    char *s = qsim_bit_vector_to_str(v);
    mu_assert_not_null(s);
    mu_assert_str_eq(s, "'b1010", "to str");
    free(s);
    qsim_bit_vector_free(v);
}

static void test_value_is_known(void)
{
    mu_assert(qsim_value_is_known(QSIM_VAL_0), "0 is known");
    mu_assert(qsim_value_is_known(QSIM_VAL_1), "1 is known");
    mu_assert(!qsim_value_is_known(QSIM_VAL_X), "X not known");
    mu_assert(!qsim_value_is_known(QSIM_VAL_Z), "Z not known");
}

static void test_value_to_str(void)
{
    const char *s = qsim_value_to_str(QSIM_VAL_1);
    mu_assert_not_null(s);
    mu_assert(strstr(s, "1") != NULL, "contains 1");
}

static void test_bit_vector_clone(void)
{
    qsim_bit_vector_t *a = qsim_bit_vector_from_str("'b1010");
    qsim_bit_vector_t *b = qsim_bit_vector_clone(a);
    mu_assert_not_null(b);
    mu_assert(qsim_bit_vector_eq(a, b), "clone equal");
    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);
}

static void test_bit_vector_from_state(void)
{
    qsim_bit_vector_t *v = qsim_bit_vector_from_state(4, QSIM_1);
    mu_assert_not_null(v);
    mu_assert_eq(v->width, 4, "width");
    for (uint32_t i = 0; i < 4; i++)
        mu_assert(qsim_bit_get(v, i).state == QSIM_1, "all 1");
    qsim_bit_vector_free(v);
}

/* ── IEEE std_logic 9-value resolution tests ── */

static void test_resolve_std_logic_identical(void)
{
    mu_assert(qsim_resolve_std_logic(QSIM_U, QSIM_U) == QSIM_U, "U+U = U");
    mu_assert(qsim_resolve_std_logic(QSIM_X, QSIM_X) == QSIM_X, "X+X = X");
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_0) == QSIM_0, "0+0 = 0");
    mu_assert(qsim_resolve_std_logic(QSIM_1, QSIM_1) == QSIM_1, "1+1 = 1");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_Z) == QSIM_Z, "Z+Z = Z");
    mu_assert(qsim_resolve_std_logic(QSIM_W, QSIM_W) == QSIM_W, "W+W = W");
    mu_assert(qsim_resolve_std_logic(QSIM_L, QSIM_L) == QSIM_L, "L+L = L");
    mu_assert(qsim_resolve_std_logic(QSIM_H, QSIM_H) == QSIM_H, "H+H = H");
    mu_assert(qsim_resolve_std_logic(QSIM_DC, QSIM_DC) == QSIM_DC, "-+- = -");
}

static void test_resolve_std_logic_z(void)
{
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_0) == QSIM_0, "Z+0 = 0");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_1) == QSIM_1, "Z+1 = 1");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_X) == QSIM_X, "Z+X = X");
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_Z) == QSIM_0, "0+Z = 0");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_W) == QSIM_W, "Z+W = W");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_L) == QSIM_L, "Z+L = L");
    mu_assert(qsim_resolve_std_logic(QSIM_Z, QSIM_H) == QSIM_H, "Z+H = H");
}

static void test_resolve_std_logic_0_vs_1(void)
{
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_1) == QSIM_X, "0+1 = X");
    mu_assert(qsim_resolve_std_logic(QSIM_1, QSIM_0) == QSIM_X, "1+0 = X");
}

static void test_resolve_std_logic_u_dominates(void)
{
    /* U dominates everything */
    mu_assert(qsim_resolve_std_logic(QSIM_U, QSIM_0) == QSIM_U, "U+0 = U");
    mu_assert(qsim_resolve_std_logic(QSIM_U, QSIM_1) == QSIM_U, "U+1 = U");
    mu_assert(qsim_resolve_std_logic(QSIM_U, QSIM_X) == QSIM_U, "U+X = U");
    mu_assert(qsim_resolve_std_logic(QSIM_U, QSIM_Z) == QSIM_U, "U+Z = U");
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_U) == QSIM_U, "0+U = U");
    mu_assert(qsim_resolve_std_logic(QSIM_X, QSIM_U) == QSIM_U, "X+U = U");
}

static void test_resolve_std_logic_weak(void)
{
    /* H + L = W */
    mu_assert(qsim_resolve_std_logic(QSIM_H, QSIM_L) == QSIM_W, "H+L = W");
    mu_assert(qsim_resolve_std_logic(QSIM_L, QSIM_H) == QSIM_W, "L+H = W");
    /* H + 0 = X (strong 0 vs weak 1) */
    mu_assert(qsim_resolve_std_logic(QSIM_H, QSIM_0) == QSIM_X, "H+0 = X");
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_H) == QSIM_X, "0+H = X");
    /* L + 1 = X (strong 1 vs weak 0) */
    mu_assert(qsim_resolve_std_logic(QSIM_L, QSIM_1) == QSIM_X, "L+1 = X");
    mu_assert(qsim_resolve_std_logic(QSIM_1, QSIM_L) == QSIM_X, "1+L = X");
    /* H + H = H */
    mu_assert(qsim_resolve_std_logic(QSIM_H, QSIM_H) == QSIM_H, "H+H = H");
    /* L + L = L */
    mu_assert(qsim_resolve_std_logic(QSIM_L, QSIM_L) == QSIM_L, "L+L = L");
}

static void test_resolve_std_logic_dc(void)
{
    /* DC resolves with most things to X */
    mu_assert(qsim_resolve_std_logic(QSIM_DC, QSIM_0) == QSIM_X, "-+0 = X");
    mu_assert(qsim_resolve_std_logic(QSIM_DC, QSIM_1) == QSIM_X, "-+1 = X");
    mu_assert(qsim_resolve_std_logic(QSIM_DC, QSIM_X) == QSIM_X, "-+X = X");
    mu_assert(qsim_resolve_std_logic(QSIM_0, QSIM_DC) == QSIM_X, "0+- = X");
}

static void test_resolve_std_logic_bit_vector(void)
{
    qsim_bit_vector_t *a = qsim_bit_vector_from_state(4, QSIM_0);
    qsim_bit_vector_t *b = qsim_bit_vector_from_state(4, QSIM_1);
    mu_assert_not_null(a);
    mu_assert_not_null(b);

    /* Resolve 0 vs 1: all bits should become X */
    qsim_bit_vector_resolve_std_logic(a, b);
    for (uint32_t i = 0; i < 4; i++)
        mu_assert(a->bits[i].state == QSIM_X, "0+1 per bit = X");

    qsim_bit_vector_free(a);
    qsim_bit_vector_free(b);
}

void register_value_tests(void)
{
    printf("[Value System]\n");
    mu_run_test(test_resolve_identical);
    mu_run_test(test_resolve_z);
    mu_run_test(test_resolve_0_vs_1);
    mu_run_test(test_resolve_x);
    mu_run_test(test_resolve_strength);
    mu_run_test(test_resolve_strength_equal);
    mu_run_test(test_resolve_strength_z);
    mu_run_test(test_bit_vector_alloc_free);
    mu_run_test(test_bit_get_set);
    mu_run_test(test_bit_vector_eq);
    mu_run_test(test_bit_vector_match);
    mu_run_test(test_bit_vector_match_z);
    mu_run_test(test_bit_vector_from_str);
    mu_run_test(test_bit_vector_to_str);
    mu_run_test(test_value_is_known);
    mu_run_test(test_value_to_str);
    mu_run_test(test_bit_vector_clone);
    mu_run_test(test_bit_vector_from_state);
    mu_run_test(test_resolve_std_logic_identical);
    mu_run_test(test_resolve_std_logic_z);
    mu_run_test(test_resolve_std_logic_0_vs_1);
    mu_run_test(test_resolve_std_logic_u_dominates);
    mu_run_test(test_resolve_std_logic_weak);
    mu_run_test(test_resolve_std_logic_dc);
    mu_run_test(test_resolve_std_logic_bit_vector);
    printf("\n");
}
