// Multiply: compute 7 * 6 = 42 via repeated addition
// Verifies: loops, BEQ, ADD/ADDI, JAL
int main(void) {
    int a = 7, b = 6, result = 0;
    while (b > 0) {
        result += a;
        b--;
    }
    return result;  // 42
}
