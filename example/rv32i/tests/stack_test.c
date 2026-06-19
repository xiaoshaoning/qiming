// Stack test: verifies sp-relative LW/SW at various DMEM offsets.
// Uses volatile locals to prevent constant-folding.
int main(void) {
    volatile int a = 7, b = 6, result = 0;
    while (b > 0) {
        result += a;
        b--;
    }
    return result;  // 42
}
