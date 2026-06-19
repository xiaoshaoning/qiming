# Qiming

Multi-language HDL simulator supporting Verilog (IEEE 1364-2005) and VHDL (IEEE 1076-2008) with mixed-language simulation via a unified intermediate representation (UIR).

## Features

- **Verilog & VHDL** — Parse and simulate both languages, including mixed-language designs
- **Event-driven engine** — Delta-cycle simulation with 4-value logic (0, 1, X, Z)
- **Full RISC-V CPU demo** — 5-stage pipeline RV32I in both Verilog and VHDL
- **CLI tools** — Compile, simulate, benchmark, and run programs from the command line
- **Desktop GUI** — Tauri-based app with Monaco editor and Canvas waveform viewer
- **MCP server** — JSON-RPC interface for AI agent integration
- **TLA+ verified** — Scheduler formally modeled and model-checked

## Quick Start

### Prerequisites

- Rust 2021 edition or later
- CMake 3.20+
- C compiler (MSVC, GCC, or Clang)
- Node.js 18+ and npm (optional, for GUI)

### Build

```bash
# Build the CLI and C library
cargo build

# Run the test suite
cd libqsim && cmake -B build && cmake --build build && ctest --test-dir build
cargo test
```

### CLI Usage

```bash
# Compile a design
qsim compile counter.v

# Simulate for N delta steps
qsim simulate counter.v 100

# Run a RISC-V CPU with a hex program
qsim run example/rv32i/rv32i_top.v example/rv32i/tests/fib.hex 200

# Performance benchmark
qsim bench

# Start MCP server (STDIO or TCP)
qsim mcp
qsim mcp --tcp 0.0.0.0:9876
```

## Project Structure

```
qiming/
├── libqsim/            C core library (UIR, scheduler, Verilog/VHDL parsers)
│   ├── include/        Public C headers
│   ├── src/            Simulation engine, parsers, value system
│   └── tests/          C test suite
├── src-tauri/          Rust/Tauri backend (CLI, GUI, MCP server, FFI)
├── webview/            TypeScript/React frontend (Monaco editor, waveform viewer)
├── python/             Python client library
├── example/            Example designs
│   ├── rv32i/          5-stage RV32I CPU (Verilog)
│   └── rv32i_vhdl/     5-stage RV32I CPU (VHDL)
├── tla/                TLA+ formal verification of scheduler
├── skills/             MCP skill definitions for AI agents
└── docs/               Documentation
    └── user_guide.md   Installation, CLI, GUI walkthrough
```

## Examples

Two complete RISC-V RV32I CPU implementations are included:

| Example | Language | Description |
|---------|----------|-------------|
| `example/rv32i/` | Verilog | 5-stage pipeline with C test programs |
| `example/rv32i_vhdl/` | VHDL | 5-stage pipeline, same architecture |

```bash
# Verilog RV32I
qsim run example/rv32i/rv32i_top.v example/rv32i/tests/fib.hex 200

# VHDL RV32I
qsim run example/rv32i_vhdl/rv32i_top.vhd example/rv32i/tests/fib.hex 200
```

## Documentation

- [User Guide](docs/user_guide.md) — Full installation, CLI commands, GUI walkthrough

## License

MIT
