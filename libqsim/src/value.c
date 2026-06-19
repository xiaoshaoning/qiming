#include "libqsim/value.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

qsim_logic_state_t qsim_resolve(qsim_logic_state_t a, qsim_logic_state_t b)
{
    if (a == b) return a;
    if (a == QSIM_Z) return b;
    if (b == QSIM_Z) return a;
    return QSIM_X;
}

qsim_value_t qsim_resolve_strength(qsim_value_t a, qsim_value_t b)
{
    if (a.state == b.state) return a;
    if (a.state == QSIM_Z) return b;
    if (b.state == QSIM_Z) return a;
    if (a.strength > b.strength) return a;
    if (b.strength > a.strength) return b;
    /* Equal strength: fall back to resolution table */
    a.state = qsim_resolve(a.state, b.state);
    return a;
}

qsim_logic_state_t qsim_resolve_wand(qsim_logic_state_t a, qsim_logic_state_t b)
{
    if (a == QSIM_0 || b == QSIM_0) return QSIM_0;
    if (a == QSIM_Z) return b;
    if (b == QSIM_Z) return a;
    if (a == QSIM_X || b == QSIM_X) return QSIM_X;
    return QSIM_1;
}

qsim_logic_state_t qsim_resolve_wor(qsim_logic_state_t a, qsim_logic_state_t b)
{
    if (a == QSIM_1 || b == QSIM_1) return QSIM_1;
    if (a == QSIM_Z) return b;
    if (b == QSIM_Z) return a;
    if (a == QSIM_X || b == QSIM_X) return QSIM_X;
    return QSIM_0;
}

void qsim_bit_vector_resolve_wand(qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (!a || !b) return;
    uint32_t w = a->width < b->width ? a->width : b->width;
    for (uint32_t i = 0; i < w; i++)
        a->bits[i].state = qsim_resolve_wand(a->bits[i].state, b->bits[i].state);
}

void qsim_bit_vector_resolve_wor(qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (!a || !b) return;
    uint32_t w = a->width < b->width ? a->width : b->width;
    for (uint32_t i = 0; i < w; i++)
        a->bits[i].state = qsim_resolve_wor(a->bits[i].state, b->bits[i].state);
}

/* IEEE std_logic 9-value resolution table.
 * Row = first driver, column = second driver. Symmetric. */
qsim_logic_state_t qsim_resolve_std_logic(qsim_logic_state_t a, qsim_logic_state_t b)
{
    static const qsim_logic_state_t table[9][9] = {
        /* Our enum order: 0, 1, X, Z, U, W, L, H, DC */
        /*       0   1   X   Z   U   W   L   H   -   */
        /* 0 */ {QSIM_0, QSIM_X, QSIM_X, QSIM_0, QSIM_U, QSIM_X, QSIM_0, QSIM_X, QSIM_X},
        /* 1 */ {QSIM_X, QSIM_1, QSIM_X, QSIM_1, QSIM_U, QSIM_X, QSIM_X, QSIM_1, QSIM_X},
        /* X */ {QSIM_X, QSIM_X, QSIM_X, QSIM_X, QSIM_U, QSIM_X, QSIM_X, QSIM_X, QSIM_X},
        /* Z */ {QSIM_0, QSIM_1, QSIM_X, QSIM_Z, QSIM_U, QSIM_W, QSIM_L, QSIM_H, QSIM_X},
        /* U */ {QSIM_U, QSIM_U, QSIM_U, QSIM_U, QSIM_U, QSIM_U, QSIM_U, QSIM_U, QSIM_U},
        /* W */ {QSIM_X, QSIM_X, QSIM_X, QSIM_W, QSIM_U, QSIM_W, QSIM_W, QSIM_W, QSIM_X},
        /* L */ {QSIM_0, QSIM_X, QSIM_X, QSIM_L, QSIM_U, QSIM_W, QSIM_L, QSIM_W, QSIM_X},
        /* H */ {QSIM_X, QSIM_1, QSIM_X, QSIM_H, QSIM_U, QSIM_W, QSIM_W, QSIM_H, QSIM_X},
        /* - */ {QSIM_X, QSIM_X, QSIM_X, QSIM_X, QSIM_U, QSIM_X, QSIM_X, QSIM_X, QSIM_DC},
    };
    if ((unsigned)a < 9 && (unsigned)b < 9)
        return table[a][b];
    return QSIM_X;
}

void qsim_bit_vector_resolve_std_logic(qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (!a || !b) return;
    uint32_t w = a->width < b->width ? a->width : b->width;
    for (uint32_t i = 0; i < w; i++)
        a->bits[i].state = qsim_resolve_std_logic(a->bits[i].state, b->bits[i].state);
}

qsim_value_t qsim_bit_get(const qsim_bit_vector_t *v, uint32_t index)
{
    if (index >= v->width) {
        qsim_value_t x = QSIM_VAL_X;
        return x;
    }
    return v->bits[index];
}

void qsim_bit_set(qsim_bit_vector_t *v, uint32_t index, qsim_value_t val)
{
    if (index >= v->width) return;
    v->bits[index] = val;
}

qsim_bit_vector_t *qsim_bit_vector_alloc(uint32_t width)
{
    qsim_bit_vector_t *v = calloc(1, sizeof(qsim_bit_vector_t));
    if (!v) return NULL;
    v->width = width;
    v->bits = calloc(width, sizeof(qsim_value_t));
    if (!v->bits) {
        free(v);
        return NULL;
    }
    /* Initialize all bits to X (calloc gives 0 = QSIM_0) */
    for (uint32_t i = 0; i < width; i++) {
        v->bits[i].state = QSIM_X;
        v->bits[i].strength = QSIM_STRENGTH_STRONG;
    }
    return v;
}

void qsim_bit_vector_free(qsim_bit_vector_t *v)
{
    if (!v) return;
    free(v->bits);
    free(v);
}

int qsim_bit_vector_eq(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (a->width != b->width) return 0;
    for (uint32_t i = 0; i < a->width; i++) {
        if (a->bits[i].state != b->bits[i].state) return 0;
        if (a->bits[i].strength != b->bits[i].strength) return 0;
    }
    return 1;
}

int qsim_bit_vector_match(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (a->width != b->width) return 0;
    for (uint32_t i = 0; i < a->width; i++) {
        qsim_logic_state_t sa = a->bits[i].state;
        qsim_logic_state_t sb = b->bits[i].state;
        if (sa == QSIM_X || sb == QSIM_X) continue;
        if (sa != sb) return 0;
    }
    return 1;
}

int qsim_bit_vector_match_z(const qsim_bit_vector_t *a, const qsim_bit_vector_t *b)
{
    if (a->width != b->width) return 0;
    for (uint32_t i = 0; i < a->width; i++) {
        qsim_logic_state_t sa = a->bits[i].state;
        qsim_logic_state_t sb = b->bits[i].state;
        if (sa == QSIM_Z || sb == QSIM_Z) continue;
        if (sa == QSIM_X || sb == QSIM_X) continue;
        if (sa != sb) return 0;
    }
    return 1;
}

qsim_bit_vector_t *qsim_bit_vector_from_str(const char *str)
{
    if (!str || !*str) return NULL;

    uint32_t width = 0;
    int base = 2;
    const char *p = str;

    if (*p == '\'') {
        p++;
        switch (*p) {
            case 'b': case 'B': base = 2; p++; break;
            case 'o': case 'O': base = 8; p++; break;
            case 'd': case 'D': base = 10; p++; break;
            case 'h': case 'H': base = 16; p++; break;
            default: base = 2; break;
        }
    }

    const char *digits = p;
    size_t digit_len = strlen(digits);

    if (base == 2) {
        width = (uint32_t)digit_len;
    } else if (base == 8) {
        width = (uint32_t)digit_len * 3;
    } else if (base == 16) {
        width = (uint32_t)digit_len * 4;
    } else {
        /* decimal: allocate enough bits */
        width = 32;
    }

    qsim_bit_vector_t *v = qsim_bit_vector_alloc(width);
    if (!v) return NULL;
    for (uint32_t i = 0; i < width; i++) {
        v->bits[i].state = QSIM_X;
        v->bits[i].strength = QSIM_STRENGTH_STRONG;
    }

    if (base == 2) {
        /* Store LSB-first: rightmost character in string goes to bit 0 */
        for (size_t i = 0; i < digit_len; i++) {
            qsim_logic_state_t s = QSIM_X;
            switch (digits[digit_len - 1 - i]) {
                case '0': s = QSIM_0; break;
                case '1': s = QSIM_1; break;
                case 'x': case 'X': s = QSIM_X; break;
                case 'z': case 'Z': s = QSIM_Z; break;
                case 'u': case 'U': s = QSIM_U; break;
                case 'w': case 'W': s = QSIM_W; break;
                case 'l': case 'L': s = QSIM_L; break;
                case 'h': case 'H': s = QSIM_H; break;
                case '-': s = QSIM_DC; break;
                default: break;
            }
            v->bits[i].state = s;
        }
    } else if (base == 16) {
        for (size_t i = 0; i < digit_len; i++) {
            uint8_t nibble = 0;
            char c = (char)tolower(digits[digit_len - 1 - i]);
            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
            else continue;
            for (int b = 0; b < 4; b++) {
                uint32_t idx = (uint32_t)(i * 4 + b);
                if (idx < width) {
                    v->bits[idx].state = (nibble >> b) & 1 ? QSIM_1 : QSIM_0;
                }
            }
        }
    }

    return v;
}

char *qsim_bit_vector_to_str(const qsim_bit_vector_t *v)
{
    if (!v) return NULL;
    /* Format: 'b followed by bits, or decimal for width > 32 */
    char *buf = malloc(v->width + 4);
    if (!buf) return NULL;

    buf[0] = '\'';
    buf[1] = 'b';
    for (uint32_t i = 0; i < v->width; i++) {
        /* Output MSB-first: last bit (highest index) first */
        uint32_t idx = v->width - 1 - i;
        switch (v->bits[idx].state) {
            case QSIM_0: buf[i + 2] = '0'; break;
            case QSIM_1: buf[i + 2] = '1'; break;
            case QSIM_X: buf[i + 2] = 'x'; break;
            case QSIM_Z: buf[i + 2] = 'z'; break;
            case QSIM_U: buf[i + 2] = 'u'; break;
            case QSIM_W: buf[i + 2] = 'w'; break;
            case QSIM_L: buf[i + 2] = 'l'; break;
            case QSIM_H: buf[i + 2] = 'h'; break;
            case QSIM_DC: buf[i + 2] = '-'; break;
        }
    }
    buf[v->width + 2] = '\0';
    return buf;
}

qsim_bit_vector_t *qsim_bit_vector_from_state(uint32_t width, qsim_logic_state_t state)
{
    qsim_bit_vector_t *v = qsim_bit_vector_alloc(width);
    if (!v) return NULL;
    for (uint32_t i = 0; i < width; i++) {
        v->bits[i].state = state;
        v->bits[i].strength = QSIM_STRENGTH_STRONG;
    }
    return v;
}

qsim_bit_vector_t *qsim_bit_vector_clone(const qsim_bit_vector_t *src)
{
    if (!src) return NULL;
    qsim_bit_vector_t *v = qsim_bit_vector_alloc(src->width);
    if (!v) return NULL;
    memcpy(v->bits, src->bits, src->width * sizeof(qsim_value_t));
    return v;
}

const char *qsim_value_to_str(qsim_value_t v)
{
    static char buf[32];
    const char *s = "?";
    switch (v.state) {
        case QSIM_0: s = "0"; break;
        case QSIM_1: s = "1"; break;
        case QSIM_X: s = "X"; break;
        case QSIM_Z: s = "Z"; break;
        case QSIM_U: s = "U"; break;
        case QSIM_W: s = "W"; break;
        case QSIM_L: s = "L"; break;
        case QSIM_H: s = "H"; break;
        case QSIM_DC: s = "-"; break;
    }
    snprintf(buf, sizeof(buf), "%s@%u", s, (unsigned)v.strength);
    return buf;
}

int qsim_value_is_known(qsim_value_t v)
{
    return v.state == QSIM_0 || v.state == QSIM_1;
}
