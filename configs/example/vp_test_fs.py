# vp_test_fs.py - FS-mode IdealConstantLVP + difftest test
import os
import m5
from m5.objects import *
from m5.util import addToPath

addToPath(os.path.dirname(os.path.abspath(__file__)) + '/../')
from common.FSConfig import makeBareMetalXSCptSystem
from common.Benchmarks import SysConfig
from common.Caches import *
from common import CacheConfig
from m5.objects.ValuePredictor import IdealConstantLVP

binary = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '../../test/test_vp_fs.elf')
ref_so = os.environ.get('GCBV_REF_SO',
    '/home/cosin/xs-gem5/NEMU/build/riscv64-nemu-interpreter-so')

print(f"Binary: {binary}")
print(f"Difftest SO: {ref_so}")

system = makeBareMetalXSCptSystem('timing', SysConfig(mem='512MiB'))

system.uartlite = UartLite()
system.uartlite.pio = system.iobus.mem_side_ports
system.clint = Clint()
system.clint.pio = system.iobus.mem_side_ports
system.clint.pio_addr = 0x38000000
system.clint.num_threads = 1
system.rtc = RiscvRTC(frequency=Frequency("1MHz"))
system.clint.int_pin = system.rtc.int_pin

system.bridge.ranges = [
    AddrRange(system.uartlite.pio_addr,
              system.uartlite.pio_addr + system.uartlite.pio_size),
    AddrRange(system.clint.pio_addr,
              system.clint.pio_addr + system.clint.pio_size),
]

# Load ELF as bootloader (not checkpoint restore)
system.restore_from_gcpt = False
system.workload.xiangshan_cpt = False
system.workload.bootloader = binary
system.workload.auto_reset_vect = False
system.workload.reset_vect = 0x80000000

system.voltage_domain = VoltageDomain(voltage='1V')
system.clk_domain = SrcClockDomain(clock='1GHz',
    voltage_domain=system.voltage_domain)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(clock='1GHz',
    voltage_domain=system.cpu_voltage_domain)

# Memory at 0x80000000 (bootloader region)
system.mem_ctrls = [SimpleMemory(range=system.mem_ranges[0])]
system.mem_ctrls[0].port = system.membus.mem_side_ports

cpu = RiscvO3CPU(clk_domain=system.cpu_clk_domain, cpu_id=0)
cpu.isa = [RiscvISA(enable_rvv=False)]
cpu.mmu.pma_checker = PMAChecker(uncacheable=[
    AddrRange(0, size=0x80000000)])
cpu.createThreads()
cpu.createInterruptController()

cpu.valuePred = IdealConstantLVP(satCounterBits=3)
cpu.enable_difftest = True
cpu.difftest_ref_so = ref_so
print("VP + difftest enabled")

system.cpu = [cpu]

args = type('Args', (), {})()
args.cpu_type = 'O3CPU'
args.caches = True
args.l1i_size = '32kB'
args.l1d_size = '32kB'
args.l2cache = False
args.cacheline_size = 64
args.num_cpus = 1
args.external_memory_system = False
args.memchecker = False
args.elastic_trace_en = False
CacheConfig.config_cache(args, system)

system.iobridge = Bridge(delay='50ns', ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

root = Root(full_system=True, system=system)

m5.instantiate()
print("Starting simulation...")
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
