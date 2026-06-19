// CRC16-CCITT (polynomial 0x1021) computation over an int array in DMEM.
// The generator polynomial g(x) = x^16 + x^12 + x^5 + 1 is used in
// LTE for DCI (Downlink Control Information) error detection.
// Verifies: bitwise XOR/AND/shift, unsigned short masking, loops,
//           and LW/SW via volatile int array.

#define N 16

// Process a single byte through CRC16
static unsigned short crc_byte(unsigned short crc, unsigned char byte) {
    crc ^= (unsigned short)byte << 8;
    for (int j = 0; j < 8; j++) {
        if (crc & 0x8000)
            crc = (crc << 1) ^ 0x1021;
        else
            crc <<= 1;
    }
    return crc;
}

int main(void) {
    volatile int data[N];
    volatile int *debug = (volatile int *)0x00010000;
    int i;

    // Initialize int array in DMEM (each value's low byte is the test data)
    for (i = 0; i < N; i++)
        data[i] = i * 0x37;

    // Dump input data to DMEM for test verification
    for (i = 0; i < N; i++)
        debug[i] = data[i];

    // Compute CRC16-CCITT over the N bytes
    unsigned short crc = 0xFFFF;
    for (i = 0; i < N; i++)
        crc = crc_byte(crc, (unsigned char)(data[i] & 0xFF));

    crc ^= 0xFFFF;

    // Dump CRC result to DMEM slot 16 for test verification
    debug[16] = crc;

    // Return the CRC16 value (fits in 16 bits, returned in x10)
    return crc;  // 0xD99A = 55706
}
