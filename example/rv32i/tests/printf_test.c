// Simple printf test for RV32I memory-mapped UART.
// Uses mpaland/printf via putchar -> UART at 0x10000000.
#include "printf.h"

int main(void) {
    printf("Hello, RV32I! x10=%d\n", 42);
    return 0;
}
