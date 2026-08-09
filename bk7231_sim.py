import argparse
import sys
import struct
import io
import os
from unicorn import *
from unicorn.arm_const import *
from capstone import *

def parse_args():
    parser = argparse.ArgumentParser(description="BK7231T Emulator")
    parser.add_argument("dump_file", help="Path to the raw BK7231T flash dump")
    parser.add_argument("--only-uart", action="store_true", help="Only print UART output (suppress MMIO/Trace/Info logs)")
    return parser.parse_args()

ARGS = parse_args()

if ARGS.only_uart:
    sys.stdout = open(os.devnull, 'w')

# --- BK7231 CRYPTO AND EXTRACTION LOGIC ---

def crc16(data: bytes, initial_value: int = 0x0000) -> int:
    reg = initial_value
    poly = 0x8005
    for octet in data:
        for i in range(8):
            topbit = reg & 0x8000
            if octet & (0x80 >> i):
                topbit ^= 0x8000
            reg <<= 1
            if topbit:
                reg ^= poly
        reg &= 0xFFFF
    return reg

def block_crc_check(block: bytes, crc_bytes: bytes) -> bool:
    calculated = crc16(block, initial_value=0xFFFF)
    unpacked_crc = struct.unpack(">H", crc_bytes)[0] & 0xFFFF
    return calculated == unpacked_crc

def strip_crcs(data: bytes) -> bytes:
    if len(data) < 36:
        return data
    offset = 0
    if block_crc_check(data[:32], data[32:34]):
        offset = 0
    elif block_crc_check(data[2:34], data[34:36]):
        offset = 2
    else:
        return data
        
    out = bytearray()
    for i in range(offset, len(data), 34):
        out.extend(data[i:i+32])
    return bytes(out)

def uint8(x): return x & 0xFF
def uint16(x): return x & 0xFFFF
def uint32(x): return x & 0xFFFFFFFF

def _generate_uint_pn15(index_mask, flag):
    if flag: return 0
    PN15_AND_CONST = 0x6371
    val_rshift_5 = uint16(index_mask >> 5)
    val_rshift_5_nibble = val_rshift_5 & 0xF
    xor_lhs = uint16(uint16(index_mask >> 7) + uint16(index_mask * 0x200))
    xor_rhs = (uint16(val_rshift_5 * 0x1000) + uint16(val_rshift_5_nibble * 0x100) + uint8(val_rshift_5 * 0x10) + val_rshift_5_nibble)
    xor_rhs &= PN15_AND_CONST
    return uint16(xor_lhs ^ xor_rhs)

def _generate_uint_pn16(index_mask, flag):
    if flag: return 0
    PN16_AND_CONST = 0x13659
    part1 = (((index_mask >> 13) & 1) + (((index_mask >> 9) & 1) * 2) + (((index_mask >> 5) & 1) * 4) + (((index_mask >> 1) & 1) * 8))
    xor_lhs = ((index_mask & 0x3FF) << 7) + ((index_mask >> 10) & 0x7F)
    xor_rhs = uint32((((index_mask >> 4) & 1) * 0x10000) + (part1 * 0x1000) + (part1 * 0x111))
    xor_rhs &= PN16_AND_CONST
    return uint32(xor_lhs ^ xor_rhs)

def _generate_uint_pn32(index_mask, flag):
    if flag: return 0
    PN32_AND_CONST = 0xE519A4F1
    xor_lhs = uint32(index_mask >> 0xF | index_mask << 0x11)
    xor_rhs_start = (index_mask >> 2) & 0xF
    xor_rhs = (uint32(xor_rhs_start * 0x10000000) + uint32(xor_rhs_start * 0x01000000) + uint32(xor_rhs_start * 0x00100000) + uint32(xor_rhs_start * 0x00010000) + uint32(xor_rhs_start * 0x00001111))
    xor_rhs &= PN32_AND_CONST
    return xor_lhs ^ xor_rhs

class BekenCodeCipher:
    def __init__(self, coef0, coef1, coef2, coef3):
        self._coef0 = coef0
        self._coef1 = coef1
        self._coef2 = coef2
        self._coef3 = coef3

    def decrypt(self, data: bytes, stream_start_offset: int = 0):
        encrypted = bytearray()
        for i in range(0, len(data), 32):
            block = data[i : i + 32]
            block_start_offset = i + stream_start_offset
            
            # Encrypt/Decrypt block
            for j in range(0, len(block), 4):
                word = int.from_bytes(block[j : j + 4], byteorder="little")
                encrypted_word = self._encrypt_word(block_start_offset + j, word)
                encrypted.extend(encrypted_word.to_bytes(4, byteorder="little"))
        return bytes(encrypted)

    def _encrypt_word(self, index: int, word: int):
        coef3_highbyte_cond = ((self._coef3 & 0xFF000000) == 0xFF000000) or ((self._coef3 & 0xFF000000) == 0)
        coef3_1_bit = coef3_2_bit = coef3_4_bit = coef3_8_bit = coef3_highbyte_cond
        if self._coef3 & 1: coef3_1_bit = True
        if self._coef3 & 2: coef3_2_bit = True
        if self._coef3 & 4: coef3_4_bit = True
        if self._coef3 & 8: coef3_8_bit = True

        coef3_4_rsh = self._coef3 >> 4
        coef3_5_rsh = (self._coef3 >> 5) & 3
        coef3_8_rsh = (self._coef3 >> 8) & 3
        coef3_11_rsh = (self._coef3 >> 11) & 3
        index_mask_16_rsh = uint16(index >> 16)
        index_mask_seq = uint16(index >> 8)

        if coef3_5_rsh == 0:
            pn15_word = (uint8(index_mask_16_rsh) + uint16((index >> 24) << 8)) ^ uint16(index)
        elif coef3_5_rsh == 1:
            pn15_word = uint8(index_mask_16_rsh) + uint16((index >> 24) << 8)
            pn15_word ^= uint8(index_mask_seq) + uint16(index << 8)
        elif coef3_5_rsh == 2:
            pn15_word = ((index_mask_16_rsh >> 8) + uint16((index >> 16) << 8)) ^ uint16(index)
        else:
            pn15_word = (index_mask_16_rsh >> 8) + uint16((index >> 16) << 8)
            pn15_word ^= uint8(index_mask_seq) + uint16(index << 8)

        pn16_word = (index >> coef3_8_rsh) & 0x1FFFF
        PN32_SHIFTS = ((0, 0), (8, 24), (16, 16), (24, 8))
        pn32_word = uint32(index >> PN32_SHIFTS[coef3_11_rsh][0] | index << PN32_SHIFTS[coef3_11_rsh][1])

        pn15_index_mask = uint16((self._coef1 >> 16) ^ pn15_word)
        pn16_index_mask = uint8(self._coef1) + (uint8(self._coef1 >> 8) * 0x200) + uint8(coef3_4_rsh & 1) * 0x100
        pn16_index_mask ^= pn16_word
        pn32_index_mask = pn32_word ^ self._coef0

        pn15_val = _generate_uint_pn15(pn15_index_mask, coef3_1_bit)
        pn16_val = _generate_uint_pn16(pn16_index_mask, coef3_2_bit)
        pn32_val = _generate_uint_pn32(pn32_index_mask, coef3_4_bit)

        final_val = 0 if coef3_8_bit else self._coef2
        word_encryption_mask = pn15_val * 0x10000 + pn16_val
        return word_encryption_mask ^ pn32_val ^ final_val ^ word


def extract_rbl(data: bytes, target_name: str = "bootloader"):
    # Clean CRC first
    data = strip_crcs(data)
    
    # RBL magic
    MAGIC = b"RBL\x00"
    idx = 0
    while True:
        idx = data.find(MAGIC, idx)
        if idx == -1:
            break
        
        # Parse RBL header (96 bytes)
        # struct FORMAT: <4sII16s24s24sIIIII
        if len(data) >= idx + 96:
            header_bytes = data[idx:idx+96]
            unpacked = struct.unpack("<4sII16s24s24sIIIII", header_bytes)
            name = unpacked[3].split(b'\x00')[0].decode('ascii', errors='ignore')
            size_raw = unpacked[8]
            size_package = unpacked[9]
            
            if name == target_name:
                print(f"Found RBL '{name}' at {hex(idx)}. Payload size: {size_package}")
                payload = data[idx+96:idx+96+size_package]
                return payload
        idx += 1
    return None

# Default keys for Beken
DEFAULT_COEFS = (0, 0, 0, 0) # usually 0, 0, 0, 0 ? Actually they are often 0.
# Wait, bk7231tools extracts the coefficients from the RBL header? No, it uses the flash keys, but wait:
# OTA algorithm CRYPT_XOR or something? In many dumps it's actually just unencrypted or keys are 0.
# Actually bk7231tools defaults to no encryption if the algorithm is NONE! 
# Let's check: OTAAlgorithm.NONE = 0.
# If RBL says NONE, we just return the payload directly!
# Let's modify extract_rbl to handle that.

def extract_and_decrypt(data: bytes, target_name: str = "bootloader"):
    # Find RBL containers
    containers = []
    idx = 0
    while idx < len(data) - 4:
        idx = data.find(b"RBL\x00", idx)
        if idx == -1:
            break
        containers.append(idx)
        idx += 4
        
    for idx in containers:
        header_data = data[idx:idx+96]
        if len(header_data) < 96:
            continue
            
        try:
            unpacked = struct.unpack("<4sII16s24s24sIIIII", header_data)
        except:
            continue
            
        algo = unpacked[1]
        name_bytes = unpacked[3]
        size_raw = unpacked[8]
        size_package = unpacked[9]
            
        name = name_bytes.split(b"\x00")[0].decode("ascii", "ignore").strip()
        print(f"DEBUG: Comparing {repr(name)} with {repr(target_name)}")
        if name == target_name:
            # print(f"Found RBL '{name}' at {hex(idx)}. Algo: {algo}")
            mapped_address = 0x00000000 if target_name == "bootloader" else 0x00010000
            payload = data[mapped_address:mapped_address+size_package]
            
            # If NONE, it's not OTA compressed/encrypted, but still flash-encrypted
            if algo == 0:
                padding = size_package - size_raw
                if padding > 0:
                    payload = payload[:size_raw] + (bytes([padding]) * padding)
            else:
                # print(f"Warning: Unsupported OTA algorithm {algo}")
                pass
            
            # Tuya BK7231 default firmware keys: "UQ+wk6PL6txZk6F+x63rAw=="
            import base64
            key_bytes = base64.b64decode("UQ+wk6PL6txZk6F+x63rAw==")
            coefs = tuple(int.from_bytes(key_bytes[i:i+4], byteorder="big") for i in range(0, 16, 4))
            cipher = BekenCodeCipher(*coefs)
            return cipher.decrypt(payload, stream_start_offset=mapped_address)
    return None

# --- SIMULATOR LOGIC ---

FLASH_BASE = 0x00000000
FLASH_SIZE = 2 * 1024 * 1024
RAM_BASE = 0x00400000
RAM_SIZE = 1 * 1024 * 1024
MMIO_BASE = 0x00800000
MMIO_SIZE = 1 * 1024 * 1024

UART1_BASE = 0x00802100
UART2_BASE = 0x00802200
UART1_FIFO_PORT = UART1_BASE + 0x0C
UART2_FIFO_PORT = UART2_BASE + 0x0C
UART1_FIFO_STATUS = UART1_BASE + 0x08
UART2_FIFO_STATUS = UART2_BASE + 0x08

def hook_mem_write(mu, access, address, size, value, user_data):
    if address == UART1_FIFO_PORT or address == UART2_FIFO_PORT:
        sys.stdout.write(chr(value & 0xFF))
        sys.stdout.flush()
    # Accept all other MMIO writes silently

def hook_mem_read(mu, access, address, size, value, user_data):
    # Dummy read for UART FIFO STATUS to prevent infinite loops waiting for TX ready
    if address == UART1_FIFO_STATUS or address == UART2_FIFO_STATUS:
        return # handeled by the initial mem_write, it's mapped.
    pass

def hook_unmapped(mu, type, address, size, value, user_data):
    # Instead of crashing, just return 0 for reads and ignore writes.
    # This acts as a global mock for all unmapped memory!
    print(f"[!] Unmapped access at 0x{address:08x} (type={type})")
    page = address & ~0xFFF
    try:
        if type == UC_MEM_READ_UNMAPPED:
            mu.mem_map(page, 0x1000)
            mu.mem_write(page, b"\x00" * 0x1000)
            return True
        elif type == UC_MEM_WRITE_UNMAPPED:
            mu.mem_map(page, 0x1000)
            return True
    except Exception as e:
        print(f"Failed to map page 0x{page:08x}: {e}")
    return False

def main():
    with open(ARGS.dump_file, "rb") as f:
        raw_data = f.read()

    flash_data = strip_crcs(raw_data)

    bootloader = extract_and_decrypt(flash_data, "bootloader")
    app = extract_and_decrypt(flash_data, "app")
    
    if app:
        with open("app_decrypted.bin", "wb") as f:
            f.write(app)

    if bootloader is None:
        print("Warning: Could not find 'bootloader' RBL container. Booting from 0x10000.")
    else:
        print(f"Loaded 'bootloader' payload, size: {len(bootloader)} bytes")
        
    if app is None:
        print("Warning: Could not find 'app' RBL container.")
    else:
        print(f"Loaded 'app' payload, size: {len(app)} bytes")

    # Hardware base addresses
    MMIO_BASE = 0x00800000
    MMIO_SIZE = 0x010000
    PERIPH_BASE = 0xc0000000
    PERIPH_SIZE = 0x100000
    VECTORS_BASE = 0x00000000
    VECTORS_SIZE = 0x010000

    # UART Registers
    UART1_FIFO_PORT = 0x0080210c
    UART1_FIFO_STATUS = 0x00802108
    UART2_FIFO_PORT = 0x0080220c
    UART2_FIFO_STATUS = 0x00802208

    # Setup memory
    try:
        mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    except UcError as e:
        print("Unicorn error:", e)
        sys.exit(1)

    mu.mem_map(FLASH_BASE, FLASH_SIZE)
    mu.mem_map(RAM_BASE, RAM_SIZE)
    mu.mem_map(MMIO_BASE, MMIO_SIZE)
    mu.mem_map(PERIPH_BASE, PERIPH_SIZE)
    
    # Write the ENTIRE raw flash image to flash memory so config partitions are available
    with open(ARGS.dump_file, "rb") as f:
        raw_flash = f.read()
    mu.mem_write(FLASH_BASE, raw_flash[:FLASH_SIZE])
    
    # Load bootloader and app over it (though they are already in the flash dump, they might be encrypted in the raw dump)
    # So we write the DECRYPTED payloads at their correct addresses
    mu.mem_write(0x00000000, bootloader)
    mu.mem_write(0x00010000, app)

    # Initialize UART status as TX empty, RX empty, WR ready
    mu.mem_write(UART1_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))
    mu.mem_write(UART2_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))

    # Initialize SARADC config to indicate FIFO empty
    SARADC_ADC_CONFIG = 0x00802c00
    mu.mem_write(SARADC_ADC_CONFIG, struct.pack("<I", 1 << 28 | 1 << 30))
    # Initialize WDT to disabled/cleared (0x00802900)
    mu.mem_write(0x00802900, struct.pack("<I", 0))

    # We need to simulate flash MMIO reads.
    # The firmware writes the address to 0x00803000, then reads data from 0x00803008.

    # ICU Registers
    ICU_INT_STATUS = 0x0080204c
    ICU_INT_RAW_STATUS = 0x00802048
    ICU_INT_ENABLE = 0x00802040
    ICU_GLOBAL_INT_EN = 0x00802044

    # Hardware state
    state = type('', (), {})()
    state.pending_irqs = 0
    state.icu_int_enable = 0
    state.icu_global_int_en = 0
    state.uart1_int_enable = 0
    state.uart2_int_enable = 0
    state.insn_count = 0

    def trigger_irq():
        if state.icu_global_int_en == 0:
            return # Global interrupts disabled
            
        cpsr = mu.reg_read(UC_ARM_REG_CPSR)
        if (cpsr & 0x80) == 0:  # I bit is clear (IRQs enabled)
            pc = mu.reg_read(UC_ARM_REG_PC)
            print(f"[IRQ] Injecting IRQ! PC=0x{pc:08x}, CPSR=0x{cpsr:08x}, Pending=0x{state.pending_irqs:08x}")
            
            # Switch to IRQ mode (0x12), disable IRQs (0x80), clear Thumb bit (0x20)
            new_cpsr = (cpsr & ~0x3F) | 0x92
            
            # Save CPSR to SPSR_irq
            mu.reg_write(UC_ARM_REG_CPSR, new_cpsr)
            
            # Now we are in IRQ mode. Write SPSR and LR.
            mu.reg_write(UC_ARM_REG_SPSR, cpsr)
            mu.reg_write(UC_ARM_REG_LR, pc + 4)
            
            # Jump to IRQ vector
            mu.reg_write(UC_ARM_REG_PC, 0x00010018)
        else:
            # print(f"[IRQ] Skipped because CPSR=0x{cpsr:08x}")
            pass

    def hook_intr(mu, intno, user_data):
        pc = mu.reg_read(UC_ARM_REG_PC)
        if intno == 2: # EXCP_SWI
            cpsr = mu.reg_read(UC_ARM_REG_CPSR)
            # Change mode to SVC (0x13), disable IRQ (0x80)
            new_cpsr = (cpsr & ~0x3F) | 0x13 | 0x80
            mu.reg_write(UC_ARM_REG_CPSR, new_cpsr)
            
            # Now SPSR and LR map to SPSR_svc and LR_svc
            mu.reg_write(UC_ARM_REG_SPSR, cpsr)
            mu.reg_write(UC_ARM_REG_LR, pc)
            
            # Jump to SWI vector
            mu.reg_write(UC_ARM_REG_PC, 0x00000008)

    def hook_code(mu, address, size, user_data):
        state.insn_count += 1

        if state.insn_count % 100000 == 0:
            print(f"[TRACE] Executing at PC=0x{address:08x}, icu_global_int_en={state.icu_global_int_en}")

        # Every 10000 instructions, trigger a Timer/PWM interrupt if enabled
        if state.insn_count % 10000 == 0:
            if state.icu_int_enable & (1 << 9): # PWM/Timer
                state.pending_irqs |= (1 << 9)
                trigger_irq()
                
        # Check UART interrupts
        if state.insn_count % 1000 == 0:
            # If TX interrupt enabled, fire it since our mock FIFO is always empty
            if (state.uart1_int_enable & 0x01) and (state.icu_int_enable & (1 << 0)):
                state.pending_irqs |= (1 << 0)
                trigger_irq()
            if (state.uart2_int_enable & 0x01) and (state.icu_int_enable & (1 << 1)):
                state.pending_irqs |= (1 << 1)
                trigger_irq()

    # --- MEMORY HOOKS ---
    class FlashState:
        def __init__(self):
            self.addr = 0
            
    flash_state = FlashState()

    def hook_mem_write_mmio(mu, access, address, size, value, user_data):
        # Exclude noisy registers
        if address not in [UART1_FIFO_STATUS, UART2_FIFO_STATUS, SARADC_ADC_CONFIG, 0x00802c00, 0x00802c04]:
            pass #print(f"[MMIO] Write 0x{value:08x} to 0x{address:08x}")

        if address == ICU_INT_ENABLE: state.icu_int_enable = value
        if address == ICU_GLOBAL_INT_EN: state.icu_global_int_en = value
        if address == ICU_INT_STATUS: state.pending_irqs &= ~value # W1C
        if address == 0x00802110: state.uart1_int_enable = value
        if address == 0x00802210: state.uart2_int_enable = value

        if address == UART1_FIFO_PORT or address == UART2_FIFO_PORT:
            sys.__stdout__.write(chr(value))
            sys.__stdout__.flush()
            return
            
        if address == 0x00803000: # REG_FLASH_OPERATE_SW
            flash_state.addr = value & 0x00FFFFFF
            op_type = (value >> 28) & 0xF
            if op_type == 6: # READ
                try:
                    # Read 4 bytes from flash
                    flash_state.data = mu.mem_read(FLASH_BASE + flash_state.addr, 4)
                except Exception:
                    flash_state.data = b'\xff\xff\xff\xff'
            # Clear the BUSY bit (bit 31) immediately when written, so polling finishes instantly
            value = value & ~(1 << 31)
            
        if address == 0x00802c00: # REG_WDT_CONFIG / some other clock config
            # It sets a bit and polls for it to be cleared by hardware
            value = 0
            
        try:
            mu.mem_write(address, struct.pack("<I" if size == 4 else "<H" if size == 2 else "B", value))
        except Exception:
            pass

    def hook_mem_read_mmio(mu, access, address, size, value, user_data):
        # Exclude noisy registers
        if address in [UART1_FIFO_STATUS, UART2_FIFO_STATUS, 0x00802c04]:
            pass
        else:
            pass # Keep it clean

        if address == 0x00802c00:
            # Bit 8 must be 0 (clears busy flag)
            # Bit 30 must be 1 (ADC busy / ready flag)
            mu.mem_write(address, struct.pack("<I", 1 << 30))
            return
            
        if address == 0xc0008050:
            # Hardware should clear the bit that firmware just wrote
            mu.mem_write(address, b'\x00\x00\x00\x00')
            return
            
        if address == 0x00802c04: # SARADC_ADC_DATA
            # Bit 30 or 31 is the DATA_READY or BUSY bit it's polling
            mu.mem_write(address, struct.pack("<I", 0xFFFFFFFF))
            return

        if address == 0x00803004: # REG_FLASH_DATA_SW_FLASH
            if hasattr(flash_state, 'data'):
                mu.mem_write(address, flash_state.data)
            return
            
        if address == ICU_INT_STATUS or address == ICU_INT_RAW_STATUS:
            val = state.pending_irqs & state.icu_int_enable
            mu.mem_write(address, struct.pack("<I", val))
            return
            
        if address == ICU_INT_ENABLE:
            mu.mem_write(address, struct.pack("<I", state.icu_int_enable))
            return
            
        if address == ICU_GLOBAL_INT_EN:
            mu.mem_write(address, struct.pack("<I", state.icu_global_int_en))
            return

        if address == 0x00802110:
            mu.mem_write(address, struct.pack("<I", state.uart1_int_enable))
            return
            
        if address == 0x00802210:
            mu.mem_write(address, struct.pack("<I", state.uart2_int_enable))
            return

        if address == UART1_FIFO_STATUS or address == UART2_FIFO_STATUS:
            # Bit 17: TX_FIFO_EMPTY (1 = empty)
            # Bit 19: RX_FIFO_EMPTY?
            # Bit 20: FIFO_WR_READY (1 = ready)
            mu.mem_write(address, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))
            return

        if address == 0x00803008: # REG_FLASH_DATA_FLASH_SW
            # Read 4 bytes from flash_data at flash_state.addr
            if flash_state.addr + 4 <= len(flash_data):
                val = struct.unpack("<I", flash_data[flash_state.addr : flash_state.addr + 4])[0]
            else:
                val = 0xFFFFFFFF
            # Auto-increment address for sequential reads (firmware expects this in some modes? Actually, the controller does it if burst reading)
            # Actually we'll just return the value.
            mu.mem_write(address, struct.pack("<I", val))
            return

        if address in [UART1_FIFO_STATUS, UART2_FIFO_STATUS, SARADC_ADC_CONFIG]:
            return
        
        try:
            mem_val = mu.mem_read(address, size)
            val = struct.unpack("<I" if size == 4 else "<H" if size == 2 else "B", mem_val)[0]
            print(f"[MMIO] Read 0x{val:08x} from 0x{address:08x}", flush=True)
        except Exception:
            pass

    if bootloader:
        mu.mem_write(0x00000000, bootloader)
    if app:
        mu.mem_write(0x00010000, app)

    # Map the SPI flash memory space at 0x00200000
    SPI_FLASH_BASE = 0x00200000
    SPI_FLASH_SIZE = (len(flash_data) + 0xFFF) & ~0xFFF
    mu.mem_map(SPI_FLASH_BASE, SPI_FLASH_SIZE)
    mu.mem_write(SPI_FLASH_BASE, flash_data)

    mu.hook_add(UC_HOOK_MEM_WRITE, hook_mem_write_mmio, begin=MMIO_BASE, end=MMIO_BASE + MMIO_SIZE)
    mu.hook_add(UC_HOOK_MEM_READ, hook_mem_read_mmio, begin=MMIO_BASE, end=MMIO_BASE + MMIO_SIZE)
    mu.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    mu.hook_add(UC_HOOK_CODE, hook_code)
    mu.hook_add(UC_HOOK_INTR, hook_intr)

    # Force starting from app to skip bootloader rabbit hole
    base_addr = 0x00010000
    print("App header:", app[:32].hex())
    
    # Run indefinitely (or until crash)
    print(f"Starting emulation at 0x{base_addr:08x}...")
    try:
        from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_CPSR
        from unicorn import UcError
        # Run for a limited number of instructions to avoid infinite hang
        mu.emu_start(base_addr, 0xFFFFFFFF, count=20000000)
    except UcError as e:
        pc = mu.reg_read(UC_ARM_REG_PC)
        cpsr = mu.reg_read(UC_ARM_REG_CPSR)
        print(f"Emulation finished with error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
    except Exception as e:
        pc = mu.reg_read(UC_ARM_REG_PC)
        cpsr = mu.reg_read(UC_ARM_REG_CPSR)
        print(f"Emulation finished with python error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
    except KeyboardInterrupt:
        print("\nEmulation stopped by user.")

    pc = mu.reg_read(UC_ARM_REG_PC)
    cpsr = mu.reg_read(UC_ARM_REG_CPSR)
    print(f"Emulation finished. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x} (T-bit: {(cpsr & 0x20) >> 5})")
    print("\nDone.")

if __name__ == "__main__":
    main()
