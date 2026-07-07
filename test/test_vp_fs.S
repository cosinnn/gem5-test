# test_vp_fs.S - FS-mode IdealConstantLVP test (bare-metal)
# Loads from binary start (0x80000000, fixed value) 200 times.
# With satCounterBits=3 (max_confidence=7), prediction triggers after ~8 commits.
# No exit instruction — exits via infinite loop + difftest timeout.

.section .text
.globl _start
_start:
    # Point to binary start (first 4 bytes are fixed code)
    li t1, 0x80000000

    # Loop: load 200 times from same address
    li t2, 200
1:
    lw t0, 0(t1)              # LOAD from same address -> canLVP()=true
    addi t2, t2, -1
    bnez t2, 1b

    # NemuOp exit (opcode 0x1a → m5_exit)
    .word 0x0000001A

done:
    j done
