"""xs_cpt.py -- minimal Xiangshan checkpoint runner for vanilla gem5"""
import argparse
import m5
from m5.objects import *
from m5.params import Frequency
from m5.util import addToPath
addToPath('..')                    # configs/example/ → configs/
from common.FSConfig import makeBareMetalXSCptSystem
from common.Benchmarks import SysConfig


def parse_args():
    parser = argparse.ArgumentParser(
        description='Run Xiangshan checkpoint on vanilla gem5')
    parser.add_argument('--generic-rv-cpt', type=str, required=True,
                        help='Path to the checkpoint binary file')
    parser.add_argument('--raw-cpt', action='store_true',
                        help='The checkpoint is a raw binary (not gzip/zstd)')
    parser.add_argument('--cpu-type', type=str, default='RiscvTimingSimpleCPU',
                        help='CPU type (default: RiscvTimingSimpleCPU)')
    parser.add_argument('--mem-size', type=str, default='512MB',
                        help='Physical memory size')
    return parser.parse_args()


def build_system(args):
    system = makeBareMetalXSCptSystem('timing', SysConfig(mem=args.mem_size))

    # --- IO devices ---
    system.uartlite = UartLite()
    system.uartlite.pio = system.iobus.mem_side_ports

    system.clint = Clint()
    system.clint.pio = system.iobus.mem_side_ports
    system.clint.pio_addr = 0x38000000
    system.clint.num_threads = 1
    system.rtc = RiscvRTC(frequency=Frequency("1MHz"))
    system.clint.int_pin = system.rtc.int_pin

    # Bridge ranges: route device addresses to IO bus
    system.bridge.ranges = [
        AddrRange(system.uartlite.pio_addr,
                  system.uartlite.pio_addr + system.uartlite.pio_size),
        AddrRange(system.clint.pio_addr,
                  system.clint.pio_addr + system.clint.pio_size),
    ]

    # --- Checkpoint params ---
    system.restore_from_gcpt = True
    system.gcpt_file = args.generic_rv_cpt
    system.workload.xiangshan_cpt = True
    system.workload.bootloader = ''
    system.workload.auto_reset_vect = False
    system.workload.reset_vect = 0x80000000

    # --- Clock / voltage domains ---
    system.voltage_domain = VoltageDomain(voltage='1V')
    system.clk_domain = SrcClockDomain(clock='1GHz',
                                       voltage_domain=system.voltage_domain)
    system.cpu_voltage_domain = VoltageDomain()
    system.cpu_clk_domain = SrcClockDomain(
        clock='1GHz', voltage_domain=system.cpu_voltage_domain)

    # --- Memory controller ---
    system.memories = [SimpleMemory(range=system.mem_ranges[0],
                                      port=system.membus.mem_side_ports)]

    # --- CPU ---
    cpu_class = globals()[args.cpu_type]
    system.cpu = [cpu_class(clk_domain=system.cpu_clk_domain, cpu_id=0)]
    system.cpu[0].createThreads()
    system.cpu[0].createInterruptController()

    # Wire CPU to memory bus (no caches, direct connection)
    system.cpu[0].icache_port = system.membus.cpu_side_ports
    system.cpu[0].dcache_port = system.membus.cpu_side_ports

    return system


def main():
    args = parse_args()
    system = build_system(args)

    root = Root(full_system=True, system=system)

    m5.instantiate()
    exit_event = m5.simulate()
    print(f'Simulation exit: {exit_event.getCause()}')


if __name__ == '__m5_main__':
    main()
