#ifndef LIBDSIM_VALUE_H
#define LIBDSIM_VALUE_H

#include <stdint.h>
#include <stddef.h>

/* Four-value logic (Verilog): 0, 1, X (unknown), Z (high-impedance).
 * Extended with VHDL std_logic 9-value states: U, W, L, H, DC (don't care). */
typedef enum qsim_logic_state {
    QSIM_0  = 0,
    QSIM_1  = 1,
    QSIM_X  = 2,
    QSIM_Z  = 3,
    QSIM_U  = 4,  /* VHDL: Uninitialized */
    QSIM_W  = 5,  /* VHDL: Weak unknown */
    QSIM_L  = 6,  /* VHDL: Weak 0 */
    QSIM_H  = 7,  /* VHDL: Weak 1 */
    QSIM_DC = 8,  /* VHDL: Don't care */
} qsim_logic_state_t;

/* Predefined strength levels */
#define QSIM_STRENGTH_SUPPLY   7
#define QSIM_STRENGTH_STRONG   6
#define QSIM_STRENGTH_PULL     5
#define QSIM_STRENGTH_WEAK     4
#define QSIM_STRENGTH_HIGHZ    0

/* Logic value with strength */
typedef struct qsim_value {
    qsim_logic_state_t state;
    uint8_t strength; /* 0 = weakest, 7 = strongest */
} qsim_value_t;

/* Predefined common values */
#define QSIM_VAL_0   ((qsim_value_t){QSIM_0, QSIM_STRENGTH_STRONG})
#define QSIM_VAL_1   ((qsim_value_t){QSIM_1, QSIM_STRENGTH_STRONG})
#define QSIM_VAL_X   ((qsim_value_t){QSIM_X, QSIM_STRENGTH_STRONG})
#define QSIM_VAL_Z   ((qsim_value_t){QSIM_Z, QSIM_STRENGTH_HIGHZ})
#define QSIM_VAL_DC  ((qsim_value_t){QSIM_DC, QSIM_STRENGTH_STRONG})
#define QSIM_VAL_U   ((qsim_value_t){QSIM_U, QSIM_STRENGTH_STRONG})

/* Bit-vector of 4-state values.
 * Bits are stored LSB-first: index 0 = bit 0 = 2^0 (least significant).
 * Display functions (to_str) output MSB-first (Verilog 'b convention).
 * Bit-select vec[0] accesses the LSB, matching Verilog semantics. */
typedef struct qsim_bit_vector {
    uint32_t width;
    qsim_value_t *bits;
} qsim_bit_vector_t;

/* === Core resolution === */

/* Resolve two logic states (without strength). */
qsim_logic_state_t qsim_resolve(qsim_logic_state_t a, qsim_logic_state_t b);

/* Resolve two values considering strength. */
qsim_value_t qsim_resolve_strength(qsim_value_t a, qsim_value_t b);

/* Wired-AND resolution per bit (wand/triand): 0 dominates, Z transparent. */
qsim_logic_state_t qsim_resolve_wand(qsim_logic_state_t a, qsim_logic_state_t b);

/* Wired-OR resolution per bit (wor/trior): 1 dominates, Z transparent. */
qsim_logic_state_t qsim_resolve_wor(qsim_logic_state_t a, qsim_logic_state_t b);

/* Bit-vector wired-AND resolution in-place on 'a'. */
void qsim_bit_vector_resolve_wand(qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* Bit-vector wired-OR resolution in-place on 'a'. */
void qsim_bit_vector_resolve_wor(qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* VHDL std_logic resolution per bit (9-value table). */
qsim_logic_state_t qsim_resolve_std_logic(qsim_logic_state_t a, qsim_logic_state_t b);

/* Bit-vector std_logic resolution in-place on 'a'. */
void qsim_bit_vector_resolve_std_logic(qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* === Bit-vector operations === */

qsim_bit_vector_t *qsim_bit_vector_alloc(uint32_t width);
void qsim_bit_vector_free(qsim_bit_vector_t *v);

qsim_value_t qsim_bit_get(const qsim_bit_vector_t *v, uint32_t index);
void qsim_bit_set(qsim_bit_vector_t *v, uint32_t index, qsim_value_t val);

/* Exact equality: all bits must match (including X/Z). */
int qsim_bit_vector_eq(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* Match: X == anything (for casez / casex semantics). */
int qsim_bit_vector_match(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* Wildcard match: Z bits are treated as don't-care (casez). */
int qsim_bit_vector_match_z(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b);

/* Conversion */
qsim_bit_vector_t *qsim_bit_vector_from_str(const char *str);
char *qsim_bit_vector_to_str(const qsim_bit_vector_t *v);

/* Create a bit-vector with all bits set to a constant state. */
qsim_bit_vector_t *qsim_bit_vector_from_state(uint32_t width, qsim_logic_state_t state);

/* Copy a bit-vector. */
qsim_bit_vector_t *qsim_bit_vector_clone(const qsim_bit_vector_t *v);

/* === Value utilities === */

/* Return a display-friendly string for a single value (static buffer). */
const char *qsim_value_to_str(qsim_value_t v);

/* Return 1 if the value represents a known logic level (0 or 1). */
int qsim_value_is_known(qsim_value_t v);

#endif /* LIBDSIM_VALUE_H */
