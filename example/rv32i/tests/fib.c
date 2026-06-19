// Fibonacci: compute Fib(10) = 55
// Verifies: branches, JAL, ADD/ADDI, multiple registers
int main(void) {
    int a = 0, b = 1, n = 10;
    while (n > 0) {
        int t = a + b;
        a = b;
        b = t;
        n--;
    }
    return a;  // Fib(10) = 55
}
