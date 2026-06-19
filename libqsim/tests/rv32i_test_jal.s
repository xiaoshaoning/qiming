# RV32I test: JAL, JALR
# JAL x1, target -> link to x1 (PC+4), jump to target
# target: set x10 = 42, JALR x0, x1, 0 -> return
# After return: ebreak -> x10 should = 42

.text
jal  x1, target     # x1 = PC+4 = 4, jump to target (PC=12)
nop                  # (not reached, jal skips this)
ebreak
target:
addi x10, x0, 42    # x10 = 42
jalr x0, x1, 0      # return to x1+0 = 4
nop                  # (not reached, jalr skips this)
