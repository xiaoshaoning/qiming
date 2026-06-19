# Test: return 42 via x10 (a0)
# ADDI x10, x0, 42; EBREAK => x10 = 42
addi x10, x0, 42
ebreak
