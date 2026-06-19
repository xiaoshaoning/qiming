# RV32I test: fibonacci(10) = 55
# x10 = fib result (should be 55 = 0x37)
# Uses x10=accumulator, x11=current, x12=counter
addi x10, x0, 0     # a = 0
addi x11, x0, 1     # b = 1
addi x12, x0, 10    # n = 10
loop:
add  x13, x10, x11  # t = a + b
addi x10, x11, 0    # a = b
addi x11, x13, 0    # b = t
addi x12, x12, -1   # n--
bne  x12, x0, loop  # if n != 0, goto loop
ebreak
