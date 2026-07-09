# test_estride_fs.S - FS-mode EStride functional test
#
# This program repeatedly executes the same load PC while updating the loaded
# memory location with a fixed stride (+4). That gives EStride a stable value
# stream to learn:
#
#   100, 104, 108, 112, ...
#
# After enough training iterations, EStride should start speculatively
# predicting the load value. A dependent addi immediately consumes the load
# result, so the pipeline must both predict and recover correctly.
#
# Architectural correctness is checked by comparing the final observed value
# with the mathematically expected value.

.section .text
.globl _start
_start:
    # Writable memory in the SimpleMemory region.
    li   t1, 0x80001000

    # Base value = 100, stride = 4, iterations = 32.
    li   t2, 100
    li   t3, 4
    li   t4, 32
    sd   t2, 0(t1)

    # s0 keeps the last dependent result observed in the loop.
    li   s0, 0

loop_body:
    ld   t0, 0(t1)                  # PC_LOAD
    addi s0, t0, 0                  # dependent consumer

    add  t2, t2, t3                 # next memory value = current + stride
    sd   t2, 0(t1)

    addi t4, t4, -1
    bnez t4, loop_body

    # The last load should have seen 100 + 4 * (32 - 1) = 224.
    li   t5, 224
    bne  s0, t5, fail

pass:
    li   a0, 0
    .word 0x4200007b               # m5_exit

fail:
    li   a0, 1
    .word 0x4400007b               # m5_fail
