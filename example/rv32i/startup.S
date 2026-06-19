.globl _start
.type _start, @function

.text

_start:
    # Clear BSS (optional, not needed for our minimal programs)
    # Set up stack pointer to top of DMEM
    lui   sp, 0x10001      # sp = 0x10001000 (top of 64KB DMEM)

    # Jump to main
    jal   ra, main

    # EBREAK on return (halts simulation)
    ebreak

    # Infinite loop (safety)
1:  j     1b
