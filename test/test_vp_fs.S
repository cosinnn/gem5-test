# test_vp_fs.S — VP misprediction recovery bug test
#
# Trains IdealConstantLVP (satCounterBits=3) on VALUE_A (0x42) for 8
# iterations at the same PC, then changes data to VALUE_B (0x99) and
# executes the same ld one more time.
#
# VP predicts 0x42; actual load returns 0x99.  A dependent addi
# immediately after the ld reads t0 from the phys reg file at rename
# time, which still contains the predicted value (0x42) because no
# misprediction recovery is triggered.
#
# If the bug exists:  s2 == 0x42 → branch to fail
# If no bug:          s2 == 0x99 → branch to pass

.section .text
.globl _start
_start:
    # Data address in writable SimpleMemory region
    li   t1, 0x80001000

    # Write VALUE_A = 0x42
    li   t2, 0x42
    sd   t2, 0(t1)

    # s0 = phase (0 = training, 1 = misprediction)
    # s1 = capture flag (0 = not captured yet)
    li   s0, 0
    li   s1, 0
    li   t3, 8                     # 8 training iterations
    j    loop_body

# -------------------------------------------------------------------
phase_setup:
    li   s0, 1                     # switch to misprediction phase
    li   t2, 0x99                  # VALUE_B = 0x99
    sd   t2, 0(t1)                 # overwrite data
    li   t3, 1                     # 1 misprediction iteration
    # fall through to loop_body

# -------------------------------------------------------------------
loop_body:
    ld   t0, 0(t1)                 # PC_LOAD — VP predicts after saturation
    addi t4, t0, 0                 # DEPENDENT: t4 = predicted or actual

    beqz s0, skip_save             # skip during training
    bnez s1, skip_save             # skip if already captured
    addi s2, t4, 0                 # s2 = first-phase dependent result
    li   s1, 1
skip_save:
    addi t3, t3, -1
    bnez t3, loop_body

    bnez s0, check_result          # phase 1 done → check
    j    phase_setup               # phase 0 done → setup phase 1

# -------------------------------------------------------------------
check_result:
    li   t5, 0x99
    bne  s2, t5, fail              # s2 should be 0x99 if no VP bug

pass:
    li   a0, 0
    .word 0x0000001A               # m5_exit

fail:
    li   a0, 1
    .word 0x0000001A
