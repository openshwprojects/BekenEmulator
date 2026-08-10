import struct

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

def extract_and_decrypt(data: bytes, target_name: str = "bootloader"):
    # Find RBL containers
    containers = []
    idx = 0
    while idx < len(data) - 4:
        idx = data.find(b"\x02\x19\x9a\x01", idx)
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
        if name == target_name:
            mapped_address = 0x00000000 if target_name == "bootloader" else 0x00010000
            payload = data[mapped_address:mapped_address+size_package]
            
            # If NONE, it's not OTA compressed/encrypted, but still flash-encrypted
            if algo == 0:
                padding = size_package - size_raw
                if padding > 0:
                    payload = payload[:size_raw] + (bytes([padding]) * padding)
            else:
                pass
            
            # Tuya BK7231 default firmware keys: "UQ+wk6PL6txZk6F+x63rAw=="
            import base64
            key_bytes = base64.b64decode("UQ+wk6PL6txZk6F+x63rAw==")
            coefs = tuple(int.from_bytes(key_bytes[i:i+4], byteorder="big") for i in range(0, 16, 4))
            cipher = BekenCodeCipher(*coefs)
            return cipher.decrypt(payload, stream_start_offset=mapped_address)
            
    # If no RBL container found (e.g. raw QIO dump), fallback to raw unencrypted slice assumptions
    print(f"Warning: '{target_name}' RBL container not found. Assuming raw payload and applying default flash decryption.")
    
    import base64
    key_bytes = base64.b64decode("UQ+wk6PL6txZk6F+x63rAw==")
    coefs = tuple(int.from_bytes(key_bytes[i:i+4], byteorder="big") for i in range(0, 16, 4))
    cipher = BekenCodeCipher(*coefs)
    
    if target_name == "bootloader":
        return cipher.decrypt(data[0:0x10000], stream_start_offset=0x00000000)
    else:
        return cipher.decrypt(data[0x10000:0x110000], stream_start_offset=0x00010000)

