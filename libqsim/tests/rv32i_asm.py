"""RV32I assembler: assembly source -> uint32 array -> IMEM bit string.

Usage:
    python rv32i_asm.py <source.s>     # assemble and print hex dump
    python rv32i_asm.py <source.s> -o out.bin  # write raw binary

Format: one instruction per line, space-separated, no commas.
Registers: x0-x31.
Immediates: decimal or 0x hex.
Labels: "label:" on its own line, referenced as "label" in branch/jump offsets.
"""

import sys
import struct

# RV32I opcodes
OP_LUI    = 0x37
OP_AUIPC  = 0x17
OP_JAL    = 0x6F
OP_JALR   = 0x67
OP_BRANCH = 0x63
OP_LOAD   = 0x03
OP_STORE  = 0x23
OP_ALUI   = 0x13
OP_ALU    = 0x33
OP_SYSTEM = 0x73

# funct3 decoders
F3_ALU = {
    'add': 0, 'sub': 0, 'sll': 1, 'slt': 2, 'sltu': 3,
    'xor': 4, 'srl': 5, 'sra': 5, 'or': 6, 'and': 7,
}
F3_ALUI = {
    'addi': 0, 'slli': 1, 'slti': 2, 'sltiu': 3,
    'xori': 4, 'srli': 5, 'srai': 5, 'ori': 6, 'andi': 7,
}
F3_BRANCH = {
    'beq': 0, 'bne': 1, 'blt': 4, 'bge': 5, 'bltu': 6, 'bgeu': 7,
}
F3_LOAD = {
    'lb': 0, 'lh': 1, 'lw': 2, 'lbu': 4, 'lhu': 5,
}
F3_STORE = {
    'sb': 0, 'sh': 1, 'sw': 2,
}

# funct7 for ALU
F7_NORMAL = 0x00
F7_SUB_SRA = 0x20


def parse_reg(r):
    """Parse register name (x0-x31) to integer."""
    if r.startswith('x') or r.startswith('X'):
        return int(r[1:])
    raise ValueError(f"Unknown register: {r}")


def parse_imm(s):
    """Parse immediate (decimal or hex) to signed integer."""
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)


def sign_extend(val, bits):
    """Sign-extend val from `bits` to 32 bits."""
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val


class Rv32iAssembler:
    """Two-pass assembler for RV32I."""

    def __init__(self):
        self.labels = {}

    def first_pass(self, lines):
        """Record label positions."""
        addr = 0
        for raw in lines:
            line = raw.split('#')[0].split('//')[0].strip()
            if not line:
                continue
            if line.endswith(':'):
                label = line[:-1].strip()
                self.labels[label] = addr
            else:
                addr += 4

    def resolve(self, token, cur_addr):
        """Resolve token to integer: label address or immediate."""
        t = token.strip()
        # Try label first
        if t in self.labels:
            return self.labels[t]
        # Try immediate
        try:
            return parse_imm(t)
        except ValueError:
            raise ValueError(f"Unknown label or immediate: {t}")

    def encode(self, tokens, addr):
        """Encode one instruction from token list at given address."""
        if not tokens:
            return None
        mnemonic = tokens[0].lower()
        rest = tokens[1:]

        # --- Pseudo-instruction: nop ---
        if mnemonic == 'nop':
            return 0x00000013  # addi x0, x0, 0

        # --- Pseudo-instruction: li (load immediate, max 2 instructions) ---
        if mnemonic == 'li':
            rd = parse_reg(rest[0])
            try:
                imm = parse_imm(rest[1])
            except ValueError:
                imm = self.resolve(rest[1], addr)  # label address
            if -2048 <= imm <= 2047:
                return (0x13 | (rd << 7) | (0 << 15) | ((imm & 0xFFF) << 20))
            else:
                upper = (imm + 0x800) >> 12
                lower = imm & 0xFFF
                lui = (0x37 | (rd << 7) | ((upper & 0xFFFFF) << 12))
                addi = (0x13 | (rd << 7) | (rd << 15) | ((lower & 0xFFF) << 20))
                return (lui, addi)

        # --- Pseudo-instruction: la (load address, max 2 instructions) ---
        if mnemonic == 'la':
            rd = parse_reg(rest[0])
            addr_val = self.resolve(rest[1], addr)
            if -2048 <= addr_val <= 2047:
                return (0x13 | (rd << 7) | (0 << 15) | ((addr_val & 0xFFF) << 20))
            else:
                upper = (addr_val + 0x800) >> 12
                lower = addr_val & 0xFFF
                lui = (0x37 | (rd << 7) | ((upper & 0xFFFFF) << 12))
                addi = (0x13 | (rd << 7) | (rd << 15) | ((lower & 0xFFF) << 20))
                return (lui, addi)

        # --- Pseudo-instruction: mv (move) ---
        if mnemonic == 'mv':
            rd = parse_reg(rest[0])
            rs = parse_reg(rest[1])
            return (0x13 | (rd << 7) | (rs << 15) | (0 << 20))  # addi rd, rs, 0

        # --- Pseudo-instruction: j (jal x0) ---
        if mnemonic == 'j':
            rd = 0
            target = self.resolve(rest[0], addr)
            offset = target - addr
            return encode_jal(rd, offset)

        # --- Pseudo-instruction: jal (jal ra) ---
        if mnemonic == 'jal' and len(rest) == 1:
            rd = 1  # ra
            target = self.resolve(rest[0], addr)
            offset = target - addr
            return encode_jal(rd, offset)

        # --- Pseudo-instruction: ret (jalr x0, x1, 0) ---
        if mnemonic == 'ret':
            return (0x67 | (0 << 7) | (1 << 15) | (0 << 20))  # jalr x0, x1, 0

        # --- LUI ---
        if mnemonic == 'lui':
            rd = parse_reg(rest[0])
            imm = parse_imm(rest[1])
            upper = imm >> 12
            return (OP_LUI | (rd << 7) | ((upper & 0xFFFFF) << 12))

        # --- AUIPC ---
        if mnemonic == 'auipc':
            rd = parse_reg(rest[0])
            imm = parse_imm(rest[1])
            upper = imm >> 12
            return (OP_AUIPC | (rd << 7) | ((upper & 0xFFFFF) << 12))

        # --- JAL ---
        if mnemonic == 'jal' and len(rest) >= 2:
            rd = parse_reg(rest[0])
            target = self.resolve(rest[1], addr)
            offset = target - addr
            return encode_jal(rd, offset)

        # --- JALR ---
        if mnemonic == 'jalr':
            rd = parse_reg(rest[0])
            rs1 = parse_reg(rest[1])
            imm = parse_imm(rest[2]) if len(rest) > 2 else 0
            return (OP_JALR | (rd << 7) | (rs1 << 15) | ((imm & 0xFFF) << 20))

        # --- B-type branches ---
        if mnemonic in F3_BRANCH:
            rs1 = parse_reg(rest[0])
            rs2 = parse_reg(rest[1])
            target = self.resolve(rest[2], addr)
            offset = target - addr
            return encode_branch(F3_BRANCH[mnemonic], rs1, rs2, offset)

        # --- Loads ---
        if mnemonic in F3_LOAD:
            rd = parse_reg(rest[0])
            # syntax: lw rd, offset(rs1)
            mem = rest[1]
            if '(' in mem:
                imm_str = mem.split('(')[0]
                rs1_str = mem.split('(')[1].rstrip(')')
                rs1 = parse_reg(rs1_str)
                imm = parse_imm(imm_str) if imm_str else 0
            else:
                rs1 = parse_reg(rest[2])
                imm = parse_imm(rest[1])
            return (OP_LOAD | (rd << 7) | (rs1 << 15) | (F3_LOAD[mnemonic] << 12) | ((imm & 0xFFF) << 20))

        # --- Stores ---
        if mnemonic in F3_STORE:
            rs2 = parse_reg(rest[0])
            mem = rest[1]
            if '(' in mem:
                imm_str = mem.split('(')[0]
                rs1_str = mem.split('(')[1].rstrip(')')
                rs1 = parse_reg(rs1_str)
                imm = parse_imm(imm_str) if imm_str else 0
            else:
                rs1 = parse_reg(rest[2])
                imm = parse_imm(rest[1])
            return encode_store(F3_STORE[mnemonic], rs2, rs1, imm)

        # --- ALU immediate ---
        if mnemonic in F3_ALUI:
            rd = parse_reg(rest[0])
            rs1 = parse_reg(rest[1])
            imm = parse_imm(rest[2])
            if mnemonic in ('srli', 'srai'):
                return (OP_ALUI | (rd << 7) | (rs1 << 15) | (F3_ALUI[mnemonic] << 12)
                        | ((imm & 0x1F) << 20) | ((0x400 if mnemonic == 'srai' else 0) << 20))
            return (OP_ALUI | (rd << 7) | (rs1 << 15) | (F3_ALUI[mnemonic] << 12)
                    | ((imm & 0xFFF) << 20))

        # --- ALU register ---
        if mnemonic in F3_ALU:
            rd = parse_reg(rest[0])
            rs1 = parse_reg(rest[1])
            rs2 = parse_reg(rest[2])
            funct7 = F7_SUB_SRA if mnemonic in ('sub', 'sra') else F7_NORMAL
            return (OP_ALU | (rd << 7) | (rs1 << 15) | (rs2 << 20)
                    | (F3_ALU[mnemonic] << 12) | (funct7 << 25))

        # --- EBREAK / ECALL ---
        if mnemonic == 'ebreak':
            return 0x00100073
        if mnemonic == 'ecall':
            return 0x00000073

        # --- MRET ---
        if mnemonic == 'mret':
            return 0x30200073

        # --- CSR instructions ---
        if mnemonic in ('csrrw', 'csrrs', 'csrrc', 'csrrwi', 'csrrsi', 'csrrci'):
            rd = parse_reg(rest[0])
            csr_addr = parse_imm(rest[2])
            funct3_map = {'csrrw': 1, 'csrrs': 2, 'csrrc': 3,
                          'csrrwi': 5, 'csrrsi': 6, 'csrrci': 7}
            funct3 = funct3_map[mnemonic]
            if mnemonic in ('csrrwi', 'csrrsi', 'csrrci'):
                uimm = parse_reg(rest[1])  # zimm uses register name encoding
                return (OP_SYSTEM | (rd << 7) | ((uimm & 0x1F) << 15)
                        | (funct3 << 12) | ((csr_addr & 0xFFF) << 20))
            else:
                rs1 = parse_reg(rest[1])
                return (OP_SYSTEM | (rd << 7) | (rs1 << 15)
                        | (funct3 << 12) | ((csr_addr & 0xFFF) << 20))

        # --- CSR read/write aliases ---
        if mnemonic == 'csrr':
            rd = parse_reg(rest[0])
            csr_addr = parse_imm(rest[1])
            return (OP_SYSTEM | (rd << 7) | (0 << 15)
                    | (2 << 12) | ((csr_addr & 0xFFF) << 20))  # CSRRS rd, x0, csr

        if mnemonic == 'csrw':
            csr_addr = parse_imm(rest[0])
            rs1 = parse_reg(rest[1])
            return (OP_SYSTEM | (0 << 7) | (rs1 << 15)
                    | (1 << 12) | ((csr_addr & 0xFFF) << 20))  # CSRRW x0, rs1, csr

        raise ValueError(f"Unknown instruction: {mnemonic}")

    def assemble(self, source):
        """Assemble source string into list of uint32 instructions."""
        lines = source.split('\n')
        # Strip comments
        clean_lines = []
        for raw in lines:
            line = raw.split('#')[0].split('//')[0].strip()
            if line and not line.endswith(':'):
                clean_lines.append(line)
            elif line and line.endswith(':'):
                clean_lines.append(line)  # keep labels for first pass

        self.first_pass(lines)

        result = []
        addr = 0
        for raw in lines:
            line = raw.split('#')[0].split('//')[0].strip()
            if not line:
                continue
            if line.endswith(':'):
                continue  # label, no instruction
            tokens = line.replace(',', ' ').split()
            if not tokens:
                continue
            enc = self.encode(tokens, addr)
            if enc is None:
                continue
            if isinstance(enc, tuple):
                for e in enc:
                    result.append(e & 0xFFFFFFFF)
                addr += 4 * len(enc)
            else:
                result.append(enc & 0xFFFFFFFF)
                addr += 4

        return result


def encode_jal(rd, offset):
    """Encode J-type instruction."""
    imm21 = offset & 0x1FFFFF
    bit20 = (imm21 >> 20) & 1
    bits_10_1 = (imm21 >> 1) & 0x3FF
    bit11 = (imm21 >> 11) & 1
    bits_19_12 = (imm21 >> 12) & 0xFF
    return (OP_JAL | (rd << 7) | (bit20 << 31) | (bits_10_1 << 21)
            | (bit11 << 20) | (bits_19_12 << 12))


def encode_branch(funct3, rs1, rs2, offset):
    """Encode B-type instruction."""
    imm13 = offset & 0x1FFF
    bit12 = (imm13 >> 12) & 1
    bit11 = (imm13 >> 11) & 1
    bits_10_5 = (imm13 >> 5) & 0x3F
    bits_4_1 = (imm13 >> 1) & 0xF
    return (OP_BRANCH | (bit12 << 31) | (bits_10_5 << 25) | (rs2 << 20)
            | (rs1 << 15) | (funct3 << 12) | (bits_4_1 << 8) | (bit11 << 7))


def encode_store(funct3, rs2, rs1, imm):
    """Encode S-type instruction."""
    imm12 = imm & 0xFFF
    return (OP_STORE | (((imm12 >> 5) & 0x7F) << 25) | (rs2 << 20)
            | (rs1 << 15) | (funct3 << 12) | ((imm12 & 0x1F) << 7))


def uint32_to_lsbfirst(val):
    """Convert uint32 to 32-char LSB-first bit string."""
    chars = []
    for b in range(32):
        chars.append('1' if (val >> b) & 1 else '0')
    return ''.join(chars)


IMEM_WORDS = 1024

def build_imem_str(words):
    """Build IMEM LSB-first bit string from uint32 list."""
    chars = []
    for i in range(IMEM_WORDS):
        val = words[i] if i < len(words) else 0x00100073  # EBREAK for unused slots
        for b in range(32):
            chars.append('1' if (val >> b) & 1 else '0')
    return ''.join(chars)


def main():
    if len(sys.argv) < 2:
        print("Usage: python rv32i_asm.py <source.s> [-o out.bin] [-c]")
        print("  -o out.bin : write raw binary (4 bytes per instruction)")
        print("  -c         : output C uint32_t array for test harness")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    asm = Rv32iAssembler()
    words = asm.assemble(source)

    # Write output
    if '-o' in sys.argv:
        idx = sys.argv.index('-o')
        path = sys.argv[idx + 1]
        with open(path, 'wb') as f:
            for w in words:
                f.write(struct.pack('<I', w))
        print(f"Wrote {len(words)} instructions ({len(words)*4} bytes) to {path}")

    if '-c' in sys.argv:
        print(f"uint32_t program[] = {{")
        for i, w in enumerate(words):
            print(f"    0x{w:08X}, /* {i*4}: */")
        print(f"}};")
        print(f"/* {len(words)} instructions */")

    # Always print hex dump
    print(f"\nHex dump ({len(words)} instructions):")
    for i, w in enumerate(words):
        print(f"  {i*4:4d}: 0x{w:08X}")

    # Print IMEM LSB-first strings if requested
    if '--imem' in sys.argv:
        imem = build_imem_str(words)
        print(f"\nIMEM bit string ({len(imem)} chars):")
        print(imem)


if __name__ == '__main__':
    main()
