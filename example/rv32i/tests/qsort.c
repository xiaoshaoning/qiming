// Quicksort: sort a volatile array in DMEM, return checksum.
// Verifies: function calls (JAL/JALR), stack save/restore, nested loops,
//           recursion, and multiple LW/SW via sp-relative addressing.
// The recursive quicksort exercises the RV32I calling convention.

#define N 16

static void swap(volatile int *a, volatile int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

static int partition(volatile int arr[], int lo, int hi) {
    int pivot = arr[hi];
    int i = lo - 1;
    for (int j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            i++;
            swap(&arr[i], &arr[j]);
        }
    }
    swap(&arr[i + 1], &arr[hi]);
    return i + 1;
}

static void quick_sort(volatile int arr[], int lo, int hi) {
    if (lo < hi) {
        int p = partition(arr, lo, hi);
        quick_sort(arr, lo, p - 1);
        quick_sort(arr, p + 1, hi);
    }
}

int main(void) {
    volatile int arr[N];
    volatile int *debug = (volatile int *)0x00010000;
    int i;

    // Initialize in descending order (worst-case for naive quicksort)
    for (i = 0; i < N; i++)
        arr[i] = N - i;

    // Sort using recursive quicksort
    quick_sort(arr, 0, N - 1);

    // Verify sorted order (fast-fail on sort bug)
    for (i = 0; i < N - 1; i++)
        if (arr[i] > arr[i + 1]) return 0xDEAD;

    // Dump sorted array to fixed DMEM address for test verification
    for (i = 0; i < N; i++)
        debug[i] = arr[i];

    // Hash of sorted array (h = h * 31 + v, 32-bit overflow)
    unsigned int cs = 0;
    for (i = 0; i < N; i++)
        cs = cs * 31 + arr[i];

    return cs;  // 0xB4459108 = 3024457992
}
