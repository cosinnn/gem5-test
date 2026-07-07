// test_vp_se.c - SE mode IdealConstantLVP test
// Uses inline asm to force real LW instructions in a loop.
// With satCounterBits=3 (max confidence=7), predictor saturates after ~8 commits.

static volatile int const_val __attribute__((aligned(64))) = 0x42;

void _start()
{
    volatile int *addr = &const_val;

    // Loop: execute LW 200 times from same address
    for (int i = 0; i < 200; i++) {
        int result;
        asm volatile("lw %0, 0(%1)" : "=r"(result) : "r"(addr) : "memory");
        // Prevent optimization: use the result
        asm volatile("" : : "r"(result));
    }

    // exit(0) via RISC-V Linux syscall
    asm volatile(
        "li a0, 0\n"
        "li a7, 93\n"
        "ecall"
    );
}
