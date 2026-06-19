# RV32I test: branch instructions
# BEQ test: x10 = 1 if beq taken correctly, else 0
# BNE test: x11 = 1 if bne taken correctly, else 0
# BLT test: x12 = 1 if blt taken correctly, else 0
# BGE test: x13 = 1 if bge taken correctly, else 0

# BEQ: 5 == 5 should take branch
addi x14, x0, 5     # x14 = 5
addi x15, x0, 5     # x15 = 5
addi x10, x0, 0     # x10 = 0
beq  x14, x15, beq_ok  # should branch (5 == 5)
addi x10, x0, 0     # x10 = 0 (not reached)
beq_ok:
addi x10, x0, 1     # x10 = 1 (beq worked)

# BNE: 5 != 6 should take branch
addi x16, x0, 5     # x16 = 5
addi x17, x0, 6     # x17 = 6
addi x11, x0, 0     # x11 = 0
bne  x16, x17, bne_ok  # should branch (5 != 6)
addi x11, x0, 0     # x11 = 0 (not reached)
bne_ok:
addi x11, x0, 1     # x11 = 1 (bne worked)

# BLT: -3 < 2 should take branch (signed)
addi x18, x0, -3    # x18 = -3
addi x19, x0, 2     # x19 = 2
addi x12, x0, 0     # x12 = 0
blt  x18, x19, blt_ok  # should branch (-3 < 2)
addi x12, x0, 0     # x12 = 0 (not reached)
blt_ok:
addi x12, x0, 1     # x12 = 1 (blt worked)

# BGE: 7 >= 7 should take branch (also tests BGE with equality)
addi x20, x0, 7     # x20 = 7
addi x21, x0, 7     # x21 = 7
addi x13, x0, 0     # x13 = 0
bge  x20, x21, bge_ok  # should branch (7 >= 7)
addi x13, x0, 0     # x13 = 0 (not reached)
bge_ok:
addi x13, x0, 1     # x13 = 1 (bge worked)

ebreak
