"""Run the EStride FS microtest or a XiangShan GCPT with difftest."""
import argparse
import os
import m5
from m5.objects import *
from m5.util import addToPath

addToPath(os.path.dirname(os.path.abspath(__file__)) + '/../configs')
from common.FSConfig import makeBareMetalXSCptSystem
from common.Benchmarks import SysConfig
from common import CacheConfig
from m5.objects.ValuePredictor import EStride


def parse_args():
    parser = argparse.ArgumentParser(
        description='Run EStride with difftest in FS microtest or GCPT mode')
    parser.add_argument(
        '--generic-rv-cpt',
        help='Path to a XiangShan GCPT (.gz or .zstd); enables checkpoint mode')
    parser.add_argument(
        '--mem-size',
        default='512MiB',
        help='Physical memory size; use 2GB for _6881_0.962556_memory_06.zstd')
    parser.add_argument(
        '--maxinsts',
        type=int,
        default=0,
        help='Stop after this many committed instructions; 0 means unlimited')
    parser.add_argument(
        '--difftest-ref-so',
        default=os.environ.get(
            'GCBV_REF_SO',
            '/home/cosin/xs-gem5/NEMU/build/riscv64-nemu-interpreter-so'),
        help='Path to the NEMU difftest shared object')
    args = parser.parse_args()

    if args.maxinsts < 0:
        parser.error('--maxinsts must be non-negative')
    if args.generic_rv_cpt and not os.path.isfile(args.generic_rv_cpt):
        parser.error(f'checkpoint not found: {args.generic_rv_cpt}')
    if not os.path.isfile(args.difftest_ref_so):
        parser.error(f'difftest reference not found: {args.difftest_ref_so}')

    return args


args = parse_args()
this_dir = os.path.dirname(os.path.abspath(__file__))
binary = os.path.join(this_dir, 'test_estride_fs.elf')
ref_so = args.difftest_ref_so

if args.generic_rv_cpt:
    print(f"GCPT: {args.generic_rv_cpt}")
else:
    print(f"Binary: {binary}")
print(f"Difftest SO: {ref_so}")

system = makeBareMetalXSCptSystem('timing', SysConfig(mem=args.mem_size))

system.uartlite = UartLite()
system.uartlite.pio = system.iobus.mem_side_ports
system.clint = Clint()
system.clint.pio = system.iobus.mem_side_ports
system.clint.pio_addr = 0x38000000
system.clint.num_threads = 1
system.rtc = RiscvRTC(frequency=Frequency('1MHz'))
system.clint.int_pin = system.rtc.int_pin

system.bridge.ranges = [
    AddrRange(system.uartlite.pio_addr,
              system.uartlite.pio_addr + system.uartlite.pio_size),
    AddrRange(system.clint.pio_addr,
              system.clint.pio_addr + system.clint.pio_size),
]

system.restore_from_gcpt = bool(args.generic_rv_cpt)
system.workload.xiangshan_cpt = bool(args.generic_rv_cpt)
system.workload.bootloader = '' if args.generic_rv_cpt else binary
system.workload.auto_reset_vect = False
system.workload.reset_vect = 0x80000000
if args.generic_rv_cpt:
    system.gcpt_file = args.generic_rv_cpt

system.voltage_domain = VoltageDomain(voltage='1V')
system.clk_domain = SrcClockDomain(
    clock='1GHz', voltage_domain=system.voltage_domain)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock='1GHz', voltage_domain=system.cpu_voltage_domain)

system.mem_ctrls = [SimpleMemory(range=system.mem_ranges[0])]
system.mem_ctrls[0].port = system.membus.mem_side_ports

cpu = RiscvO3CPU(clk_domain=system.cpu_clk_domain, cpu_id=0)
cpu.isa = [RiscvISA(enable_rvv=True, vlen=128, elen=64)]
cpu.enable_riscv_vector = True
cpu.mmu.pma_checker = PMAChecker(uncacheable=[AddrRange(0, size=0x80000000)])
cpu.createThreads()
cpu.createInterruptController()
if args.maxinsts:
    cpu.max_insts_any_thread = args.maxinsts

cpu.valuePred = EStride(
    ways=3,
    strideWidth=20,
    tagWidth=16,
    logESTBEntrys=7,
    logMaxConfidence=3,
    thresholdPercent=0.25,
    idealWindow=True,
    inflightWindowTagLength=64,
)
cpu.enable_difftest = True
cpu.difftest_ref_so = ref_so
print('EStride + difftest enabled')
if args.maxinsts:
    print(f'Max instructions: {args.maxinsts}')

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
print('Starting simulation...')
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
