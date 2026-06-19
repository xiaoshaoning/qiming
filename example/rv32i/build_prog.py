#!/usr/bin/env python3
# RV32I toolchain build script.
# Compiles a C file to a hex file loadable into the simulator.
#
# Usage:
#   python build_prog.py tests/addi_test.c     -> outputs tests/addi_test.hex
#   python build_prog.py tests/fib.c --header  -> outputs .hex + C header
#
# Requires: riscv64-unknown-elf-gcc (via WSL or on PATH)

import os, sys, subprocess, json

# Paths
PROJ = os.path.dirname(os.path.abspath(__file__))
LD_SCRIPT = os.path.join(PROJ, "rv32i.ld")
STARTUP = os.path.join(PROJ, "startup.S")

# Library sources linked on request (via --libs flag)
KNOWN_LIBS = {"printf": ["putchar.c", "printf.c"]}

def win_to_wsl(p):
    """Convert a Windows path (D:\foo\bar) to a WSL path (/mnt/d/foo/bar)."""
    if len(p) > 1 and p[1] == ':':
        drive = p[0].lower()
        rest = p[2:].replace('\\', '/')
        return f"/mnt/{drive}{rest}"
    return p.replace('\\', '/')

def run(cmd, **kw):
    print(f"  + {cmd[:80]}{'...' if len(cmd) > 80 else ''}")
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def compile_c(c_path, hex_path=None, gen_header=False, extra_sources=None, keep_elf=False):
    base = os.path.splitext(c_path)[0]
    elf_path = base + ".elf"
    bin_path = base + ".bin"
    if hex_path is None:
        hex_path = base + ".hex"

    # Determine toolchain prefix — try WSL first, then native
    def wsl_cmd(cmd):
        # Convert all Windows paths in the command to WSL paths.
        # Handles standalone paths and -I<path>, -o <path>, etc.
        parts = cmd.split()
        converted = []
        for p in parts:
            # Handle -I<path>, -o <file>, etc. where <path> may be Windows-style
            for prefix in ["-I", "-o", "-c"]:
                if p.startswith(prefix) and len(p) > len(prefix):
                    rest = p[len(prefix):]
                    if len(rest) > 1 and rest[1] == ':':
                        p = prefix + win_to_wsl(rest)
                        break
            else:
                if len(p) > 1 and p[1] == ':':
                    p = win_to_wsl(p)
            converted.append(p)
        return run(["wsl", "bash", "-c", " ".join(converted)])

    if extra_sources is None:
        extra_sources = []

    # Common compiler flags (default -O2, printf uses -Os to fit in IMEM)
    COMMON_CFLAGS = "-march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding -O2"
    PRINTF_CFLAGS = "-march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding -Os" \
                    " -DPRINTF_DISABLE_SUPPORT_FLOAT -DPRINTF_DISABLE_SUPPORT_EXPONENTIAL" \
                    " -DPRINTF_DISABLE_SUPPORT_LONG_LONG -I" + PROJ

    # Step 1: Compile main C file
    cc_cmd = f"riscv64-unknown-elf-gcc {COMMON_CFLAGS} -I{PROJ} -c {c_path} -o {base}.o"
    r = wsl_cmd(cc_cmd)
    if r.returncode != 0:
        print(f"  ERROR compile:\n{r.stderr}")
        return False
    print(f"  Compile {os.path.basename(c_path)} OK")

    # Collect .o files for linking
    objs = [base + ".o"]

    # Step 2: Compile extra library sources
    for src in extra_sources:
        src_path = os.path.join(PROJ, src) if not os.path.isabs(src) else src
        src_base = os.path.splitext(os.path.basename(src))[0]
        src_o = os.path.join(PROJ, src_base + ".o")
        cflags = PRINTF_CFLAGS if "printf" in src else COMMON_CFLAGS + " -I" + PROJ
        r = wsl_cmd(f"riscv64-unknown-elf-gcc {cflags} -c {src_path} -o {src_o}")
        if r.returncode != 0:
            print(f"  ERROR compile lib {src}:\n{r.stderr}")
            return False
        print(f"  Compile lib {src} OK")
        objs.append(src_o)

    # Step 3: Compile startup
    startup_o = os.path.join(PROJ, "startup.o")
    r = wsl_cmd(f"riscv64-unknown-elf-gcc {COMMON_CFLAGS} -c {STARTUP} -o {startup_o}")
    if r.returncode != 0:
        print(f"  ERROR startup compile:\n{r.stderr}")
        return False
    objs.insert(0, startup_o)  # startup must be first

    # Step 4: Link (use gcc driver so libgcc helpers are found)
    objs_str = " ".join(objs)
    ld_cmd = f"riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -T {LD_SCRIPT} {objs_str} -lgcc -o {elf_path}"
    r = wsl_cmd(ld_cmd)
    if r.returncode != 0:
        print(f"  ERROR link:\n{r.stderr}")
        return False
    print(f"  Link OK")

    # Step 5: Convert to flat binary
    r = wsl_cmd(f"riscv64-unknown-elf-objcopy -O binary {elf_path} {bin_path}")
    if r.returncode != 0:
        print(f"  ERROR objcopy:\n{r.stderr}")
        return False
    print(f"  Binary OK")

    # Step 6: Convert binary to hex (one 32-bit word per line, hex)
    with open(bin_path, "rb") as f:
        data = f.read()

    # Pad to 32-bit alignment
    while len(data) % 4 != 0:
        data += b'\x00'

    words = []
    for i in range(0, len(data), 4):
        word = int.from_bytes(data[i:i+4], 'little')
        words.append(f"{word:08X}")

    with open(hex_path, "w") as f:
        for w in words:
            f.write(w + "\n")

    print(f"  Hex: {hex_path} ({len(words)} words)")

    # Optional: generate C header
    if gen_header:
        h_path = base + ".hex.h"
        arr_name = os.path.basename(base).replace("-", "_").replace(".", "_")
        with open(h_path, "w") as f:
            f.write(f"/* Auto-generated from {os.path.basename(c_path)} */\n")
            f.write(f"static uint32_t prog_{arr_name}[] = {{\n")
            for i, w in enumerate(words):
                sep = "," if i < len(words) - 1 else ""
                if i % 8 == 7:
                    f.write(f"    0x{w}{sep}\n")
                else:
                    f.write(f"    0x{w}{sep}")
            if len(words) % 8 != 0:
                f.write("\n")
            f.write(f"}};\n")
            f.write(f"static int prog_{arr_name}_len = {len(words)};\n")
        print(f"  Header: {h_path}")

    # Cleanup intermediate files
    all_objs = objs + [startup_o]
    for f in all_objs:
        try: os.remove(f)
        except: pass
    if not keep_elf:
        for f in [elf_path, bin_path]:
            try: os.remove(f)
            except: pass

    return True


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python build_prog.py <file.c> [--header] [--libs lib1,lib2,...]")
        print("  Known libs: printf")
        sys.exit(1)

    c_path = sys.argv[1]
    gen_header = "--header" in sys.argv
    extra_sources = []

    # Parse --libs
    for i, arg in enumerate(sys.argv):
        if arg == "--libs" and i + 1 < len(sys.argv):
            for lib_name in sys.argv[i + 1].split(","):
                lib_name = lib_name.strip()
                if lib_name in KNOWN_LIBS:
                    extra_sources.extend(KNOWN_LIBS[lib_name])
                else:
                    print(f"Unknown library: {lib_name}")
                    sys.exit(1)

    if not os.path.isfile(c_path):
        c_path = os.path.join(PROJ, "tests", sys.argv[1])

    if not os.path.isfile(c_path):
        print(f"File not found: {sys.argv[1]}")
        sys.exit(1)

    ok = compile_c(c_path, gen_header=gen_header, extra_sources=extra_sources)
    sys.exit(0 if ok else 1)
