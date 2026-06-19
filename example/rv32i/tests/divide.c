// Integer division test for RV32I.
// Implements unsigned division and modulo using shift-and-subtract
// (binary long division). Only uses RV32I base instructions.
// Verifies: SLLI/SRLI, ANDI, SLTU, conditional branch, subtraction.

#define N 16

// Unsigned division: quotient = dividend / divisor
static unsigned int udiv(unsigned int dividend, unsigned int divisor) {
    unsigned int quotient = 0;
    unsigned int rem = 0;
    if (divisor == 0) return 0;  // div by zero -> 0
    for (int i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((dividend >> i) & 1);
        if (rem >= divisor) {
            rem -= divisor;
            quotient |= (1u << i);
        }
    }
    return quotient;
}

// Unsigned modulo: remainder = dividend % divisor
static unsigned int umod(unsigned int dividend, unsigned int divisor) {
    unsigned int rem = 0;
    if (divisor == 0) return 0;
    for (int i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((dividend >> i) & 1);
        if (rem >= divisor)
            rem -= divisor;
    }
    return rem;
}

int main(void) {
    volatile int *debug = (volatile int *)0x00010000;
    unsigned int a, b, q, r;
    int idx = 0;

    // Test 1: 12345 / 10
    a = 12345; b = 10;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 1234
    debug[idx++] = r;  // 5

    // Test 2: 1000000 / 3
    a = 1000000; b = 3;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 333333
    debug[idx++] = r;  // 1

    // Test 3: 999 / 999 (divide by self)
    a = 999; b = 999;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 1
    debug[idx++] = r;  // 0

    // Test 4: 0 / 1234 (zero dividend)
    a = 0; b = 1234;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 0
    debug[idx++] = r;  // 0

    // Test 5: 65535 / 1 (divide by 1)
    a = 65535; b = 1;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 65535
    debug[idx++] = r;  // 0

    // Test 6: 0xDEAD / 0xFF
    a = 0xDEAD; b = 0xFF;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 223
    debug[idx++] = r;  // 140

    // Test 7: 0x10000 / 0x100 (power-of-2 divisor)
    a = 0x10000; b = 0x100;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 0x100 (256)
    debug[idx++] = r;  // 0

    // Test 8: largest near 32-bit: 0xFFFFFFFE / 2
    a = 0xFFFFFFFEu; b = 2;
    q = udiv(a, b);
    r = umod(a, b);
    debug[idx++] = q;  // 0x7FFFFFFF
    debug[idx++] = r;  // 0

    // Combined checksum for quick verification
    unsigned int cs = 0;
    for (int i = 0; i < idx; i++)
        cs = cs * 31 + debug[i];

    return cs;
}
