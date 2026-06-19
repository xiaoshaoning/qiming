# RV32I test: load/store word
# x10 = value to store (42), x11 = address base (0)
# SW x10, 0(x11) -> mem[0] = 42
# LW x12, 0(x11) -> x12 = 42
# ADDI x10, x12, 0 -> x10 = x12 = 42
addi x10, x0, 42    # x10 = 42
addi x11, x0, 0     # x11 = 0
sw   x10, 0(x11)    # mem[0] = x10
addi x10, x0, 0     # x10 = 0 (clobber)
lw   x12, 0(x11)    # x12 = mem[0]
addi x10, x12, 0    # x10 = x12 = 42
ebreak
