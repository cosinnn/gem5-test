# vp_test_se.py - SE-mode IdealConstantLVP + difftest test
import os
import m5
from m5.objects import *
from m5.objects.ValuePredictor import IdealConstantLVP

binary = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '../../test/test_vp_se.elf')
ref_so = os.environ.get('GCBV_REF_SO',
    '/home/cosin/xs-gem5/NEMU/build/riscv64-nemu-interpreter-so')

print(f"Binary: {binary}")
print(f"Difftest SO: {ref_so}")

cpu = RiscvO3CPU(cpu_id=0)
cpu.isa = [RiscvISA(enable_rvv=False)]
cpu.valuePred = IdealConstantLVP(satCounterBits=3)
cpu.enable_difftest = True
cpu.difftest_ref_so = ref_so
cpu.createThreads()
cpu.createInterruptController()
print("CPU setup done")

process = Process()
process.executable = binary
process.cmd = [binary]
cpu.workload = process

system = System(cpu=[cpu], mem_mode='timing',
                mem_ranges=[AddrRange('512MiB')],
                cache_line_size=64)

system.clk_domain = SrcClockDomain(clock='1GHz',
    voltage_domain=VoltageDomain())

system.membus = SystemXBar()

cpu.icache = Cache(size='32KiB', assoc=2, tag_latency=1,
    data_latency=1, response_latency=1, mshrs=4, tgts_per_mshr=8,
    is_read_only=True, writeback_clean=False,
    clk_domain=system.clk_domain)
cpu.icache.cpu_side = cpu.icache_port
cpu.icache.mem_side = system.membus.cpu_side_ports

cpu.dcache = Cache(size='32KiB', assoc=2, tag_latency=1,
    data_latency=1, response_latency=1, mshrs=4, tgts_per_mshr=8,
    is_read_only=False, writeback_clean=False,
    clk_domain=system.clk_domain)
cpu.dcache.cpu_side = cpu.dcache_port
cpu.dcache.mem_side = system.membus.cpu_side_ports

system.mem_ctrls = [SimpleMemory(range=system.mem_ranges[0])]
system.mem_ctrls[0].port = system.membus.mem_side_ports

system.workload = SEWorkload.init_compatible(binary)

root = Root(full_system=False, system=system)

print("Instantiating...")
m5.instantiate()
print("Starting simulation...")
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
