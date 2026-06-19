#!/usr/bin/env python3
# RV32I assembly build script.
# Assembles .s files using rv32i_asm.py and generates hex + C headers.
#
# Usage:
#   python asm_build.py tests/addi_test.s          # -> tests/addi_test.hex
#   python asm_build.py tests/addi_test.s -o out.hex  # -> out.hex
#   python asm_build.py --header tests/*.s          # -> .hex + C headers

import os, sys, struct

# Path to the shared RV32I assembler
ASM_PY = os.path.join(os.path.dirname(__file__),
                      "..", "..", "libqsim", "tests", "rv32i_asm.py")

def assemble_file(s_path, hex_path=None, gen_header=False):
    """Assemble a .s file and produce hex output."""
    sys.path.insert(0, os.path.dirname(ASM_PY))
    from rv32i_asm import Rv32iAssembler

    with open(s_path) as f:
        source = f.read()

    asm = Rv32iAssembler()
    words = asm.assemble(source)

    if hex_path is None:
        base = os.path.splitext(s_path)[0]
        hex_path = base + ".hex"

    # Write hex file (one word per line)
    with open(hex_path, "w") as f:
        for w in words:
            f.write(f"{w:08X}\n")

    print(f"  Assembled {os.path.basename(s_path)} -> {hex_path}")
    print(f"    {len(words)} instructions")
    for i, w in enumerate(words):
        print(f"    [{i*4:4d}] 0x{w:08X}")

    # Generate C header if requested
    if gen_header:
        h_path = os.path.splitext(hex_path)[0] + ".hex.h"
        arr_name = os.path.splitext(os.path.basename(s_path))[0].replace("-", "_")
        with open(h_path, "w") as f:
            f.write(f"/* Auto-generated from {os.path.basename(s_path)} */\n")
            f.write(f"#pragma once\n")
            f.write(f"static uint32_t prog_{arr_name}[] = {{\n")
            for i, w in enumerate(words):
                sep = "," if i < len(words) - 1 else ""
                if i % 8 == 7:
                    f.write(f"    0x{w:08X}{sep}\n")
                else:
                    f.write(f"    0x{w:08X}{sep}")
            if len(words) % 8 != 0:
                f.write("\n")
            f.write(f"}};\n")
            f.write(f"static int prog_{arr_name}_len = {len(words)};\n")
        print(f"  Header: {h_path}")

    return True


def hex_to_c_header(hex_path, h_path=None, arr_name=None):
    """Convert a hex file to a C header."""
    if h_path is None:
        h_path = os.path.splitext(hex_path)[0] + ".hex.h"
    if arr_name is None:
        arr_name = os.path.splitext(os.path.basename(hex_path))[0].replace("-", "_")

    with open(hex_path) as f:
        lines = [l.strip() for l in f if l.strip()]

    words = [int(l, 16) for l in lines]

    with open(h_path, "w") as f:
        f.write(f"/* Auto-generated from {os.path.basename(hex_path)} */\n")
        f.write(f"#pragma once\n")
        f.write(f"static uint32_t prog_{arr_name}[] = {{\n")
        for i, w in enumerate(words):
            sep = "," if i < len(words) - 1 else ""
            if i % 8 == 7:
                f.write(f"    0x{w:08X}{sep}\n")
            else:
                f.write(f"    0x{w:08X}{sep}")
        if len(words) % 8 != 0:
            f.write("\n")
        f.write(f"}};\n")
        f.write(f"static int prog_{arr_name}_len = {len(words)};\n")

    print(f"  Header: {h_path} ({len(words)} words)")
    return True


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="RV32I assembly build tool")
    parser.add_argument("files", nargs="+", help=".s files to assemble")
    parser.add_argument("-o", "--output", help="Output hex file (single file only)")
    parser.add_argument("--header", action="store_true", help="Generate C header")
    args = parser.parse_args()

    for f in args.files:
        hex_out = args.output if len(args.files) == 1 else None
        assemble_file(f, hex_path=hex_out, gen_header=args.header)
