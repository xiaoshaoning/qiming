# Multiply: compute 7 * 6 = 42
# a=7(x10), b=6(x11), result=0(x12)
# loop: if b==0 done, result+=a, b--, goto loop
# Uses: ADDI, ADD, BEQ/Branch
addi x10, x0, 7      # a = 7
addi x11, x0, 6      # b = 6
addi x12, x0, 0      # result = 0
loop:
beq  x11, x0, done   # if b == 0, done
add  x12, x12, x10   # result += a
addi x11, x11, -1    # b--
jal  x0, loop        # goto loop
done:
addi x10, x12, 0     # return result in x10
ebreak
