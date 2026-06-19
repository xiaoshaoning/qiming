# Fibonacci: compute Fib(10) = 55
# a=0(x10), b=1(x11), n=10(x12), loop: t=a+b(x13), a=b, b=t, n--, if n>0 goto loop
# Uses: ADDI, ADD, BNE/Branch, JAL (pseudo for branch)
addi x10, x0, 0      # a = 0
addi x11, x0, 1      # b = 1
addi x12, x0, 10     # n = 10
loop:
add  x13, x10, x11   # t = a + b
addi x10, x11, 0     # a = b
addi x11, x13, 0     # b = t
addi x12, x12, -1    # n--
bne  x12, x0, loop   # if n != 0, goto loop
ebreak               # done, x10 = 55
