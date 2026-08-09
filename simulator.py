import argparse
import sys
import struct
from unicorn import *
from unicorn.arm_const import *

# BK7231T memory map
FLASH_BASE = 0x00000000
FLASH_SIZE = 2 * 1024 * 1024

RAM_BASE = 0x00400000
RAM_SIZE = 1 * 1024 * 1024

MMIO_BASE = 0x00800000
MMIO_SIZE = 1 * 1024 * 1024

# UART registers
UART1_BASE = 0x00802100
UART2_BASE = 0x00802200

UART1_FIFO_PORT = UART1_BASE + 0x0C
UART2_FIFO_PORT = UART2_BASE + 0x0C
UART1_FIFO_STATUS = UART1_BASE + 0x08
UART2_FIFO_STATUS = UART2_BASE + 0x08

def hook_mem_write(mu, access, address, size, value, user_data):
    if address == UART1_FIFO_PORT or address == UART2_FIFO_PORT:
        char = value & 0xFF
        sys.stdout.write(chr(char))
        sys.stdout.flush()
        # Also need to write to memory? The emulator will do it automatically since it's a write access,
        # but MMIO is mapped, so it's fine.

def hook_unmapped(mu, type, address, size, value, user_data):
    print(f"\n[!] Unmapped memory {'write' if type == UC_MEM_WRITE_UNMAPPED else 'read'} at 0x{address:08x}, size: {size}")
    return False

def main():
    parser = argparse.ArgumentParser(description="Simple BK7231 DRY Code Simulator")
    parser.add_argument("bin_file", help="Decrypted firmware bin file")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=FLASH_BASE, help="Base address to load the bin file")
    
    args = parser.parse_args()

    # Initialize emulator in ARM mode
    try:
        mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    except UcError as e:
        print("ERROR: %s" % e)
        return

    # Map memory
    mu.mem_map(FLASH_BASE, FLASH_SIZE)
    mu.mem_map(RAM_BASE, RAM_SIZE)
    mu.mem_map(MMIO_BASE, MMIO_SIZE)

    # Initialize MMIO with zeroes, then set FIFO status to "empty" and "write ready"
    mu.mem_write(UART1_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20)) # TX empty, RX empty, WR ready
    mu.mem_write(UART2_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))

    # Read binary and write to mapped Flash
    with open(args.bin_file, "rb") as f:
        code = f.read()

    print(f"Loading {len(code)} bytes at 0x{args.base:08x}...", flush=True)
    mu.mem_write(args.base, code)

    # Add hooks
    mu.hook_add(UC_HOOK_MEM_WRITE, hook_mem_write, begin=MMIO_BASE, end=MMIO_BASE + MMIO_SIZE)
    mu.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)

    # Start emulation
    print("Starting emulation...", flush=True)
    try:
        # Start from base address. For bootloader it's 0x00000000.
        mu.emu_start(args.base, args.base + len(code))
    except UcError as e:
        print(f"\nEmulation error: {e}")
        
    print("\nEmulation finished.")

if __name__ == "__main__":
    main()
