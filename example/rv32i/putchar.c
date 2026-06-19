// putchar for RV32I memory-mapped UART at address 0x10000000
void _putchar(char character) {
    volatile unsigned int *uart = (volatile unsigned int *)0x10000000;
    *uart = (unsigned int)(unsigned char)character;
}
