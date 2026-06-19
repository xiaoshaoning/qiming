// Minimal test: return 42 via ADDI to x10 (a0)
// Verifies: ADDI, register writeback, EBREAK halts
int main(void) {
    return 42;
}
