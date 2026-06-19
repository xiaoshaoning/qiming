# RV32I test: all ALU operations
# Tests: ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND
# x10 = ADD, x11 = SUB, x12 = SLL, x13 = SLT, x14 = SLTU
# x15 = XOR, x16 = SRL, x17 = SRA, x18 = OR, x19 = AND

# Setup values
addi x5, x0, 7       # x5 = 7 (0b111)
addi x6, x0, 3       # x6 = 3 (0b011)

# ADD: x10 = x5 + x6 = 7 + 3 = 10
add  x10, x5, x6

# SUB: x11 = x5 - x6 = 7 - 3 = 4
sub  x11, x5, x6

# SLL: x12 = x5 << x6[4:0] = 7 << 3 = 56
addi x5, x0, 7
addi x6, x0, 3
sll  x12, x5, x6

# SLT: x13 = (7 < 3) ? 1 : 0 = 0
addi x5, x0, 7
addi x6, x0, 3
slt  x13, x5, x6

# SLTU: x14 = (7 < 3) ? 1 : 0 = 0
sltu x14, x5, x6

# XOR: x15 = 7 ^ 3 = 4
xor  x15, x5, x6

# SRL: x16 = 7 >> 3 = 0
addi x7, x0, 3
srl  x16, x5, x7

# SRA: x17 = (-8) >> 2 = -2 (arithmetic)
addi x8, x0, -8
addi x9, x0, 2
sra  x17, x8, x9

# OR: x18 = 7 | 3 = 7
or   x18, x5, x6

# AND: x19 = 7 & 3 = 3
and  x19, x5, x6

ebreak
