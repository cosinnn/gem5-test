"""verify_step2.py -- verify Step 2 checkpoint restore logs"""
import argparse
import m5
from m5.objects import *
from m5.util import addToPath

addToPath('./configs')
from common.FSConfig import (
    makeBareMetalXSCptSystem,
    SysConfig,
)

cpt_path = '/home/cosin/xs-gem5/ready-to-run/coremark-2-iteration.bin'

# === Build system ===
system = makeBareMetalXSCptSystem('atomic', SysConfig(mem='512MB'))
system.restore_from_gcpt = True
system.gcpt_file = cpt_path
system.workload.xiangshan_cpt = True
system.workload.bootloader = ''
system.workload.auto_reset_vect = False
system.workload.reset_vect = 0x80000000

# Clock / voltage domains (required by Bridge, memories, CPUs)
vd = VoltageDomain()
system.voltage_domain = vd
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=vd)

# Add DRAM controller (gem5 requires at least one AbstractMemory)
system.memories = [
    SimpleMemory(range=system.mem_ranges[0])
]

# Add single atomic CPU
system.cpu = [RiscvAtomicSimpleCPU(cpu_id=0)]
system.cpu[0].createThreads()
system.cpu[0].createInterruptController()

# Wire CPU to memory bus
system.cpu[0].icache_port = system.membus.cpu_side_ports
system.cpu[0].dcache_port = system.membus.cpu_side_ports

# Build root
root = Root(full_system=True, system=system)

# Instantiate (triggers initState, which calls tryRestoreFromXSCpt)
m5.instantiate()

# Run a few ticks
exit_event = m5.simulate(1000)
print(f'Exit cause: {exit_event.getCause()}')
