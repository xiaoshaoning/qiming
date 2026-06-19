// Bubble sort: sort a small array in DMEM, return hash checksum.
// Verifies: LW/SW, nested loops, branches, stack usage.
// The array is declared volatile to force DMEM access.

#define N 16

int main(void) {
    volatile int arr[N];
    volatile int *debug = (volatile int *)0x00010000;
    int i, j;

    // Initialize array in descending order (worst-case for bubble sort)
    for (i = 0; i < N; i++)
        arr[i] = N - i;

    // Bubble sort
    for (i = 0; i < N - 1; i++) {
        for (j = 0; j < N - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int t = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = t;
            }
        }
    }

    // Dump sorted array to fixed DMEM address for test verification
    for (i = 0; i < N; i++)
        debug[i] = arr[i];

    // Hash of sorted array (h = h * 31 + v, 32-bit overflow)
    unsigned int cs = 0;
    for (i = 0; i < N; i++)
        cs = cs * 31 + arr[i];

    return cs;  // 0xB4459108 = 3024457992
}
