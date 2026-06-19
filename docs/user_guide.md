# Qiming User Guide

## Installation

### Prerequisites

- Rust 2021 edition or later
- CMake 3.20+
- C compiler (MSVC, GCC, or Clang)
- Node.js 18+ and npm (for GUI only)

### Build from Source

```bash
# Clone the repository
git clone <repo-url> qiming
cd qiming

# Build the CLI and C library
cargo build

# Build the GUI (optional)
cd webview
npm install
npm run build
cd ..

# Run tests
cargo test                 # Rust tests (18)
cd libqsim && ctest --test-dir build   # C tests (309)
```

### Desktop App (Tauri)

```bash
# Install Tauri prerequisites (first time only)
# See https://v2.tauri.app/start/prerequisites/

# Run in development mode
cargo tauri dev
```

## CLI Usage

The `qsim` binary provides four subcommands: `compile`, `simulate`, `bench`, and `mcp`.

### compile

Compile a Verilog or VHDL design file and print diagnostics.

```bash
qsim compile counter.v
qsim compile top.vhdl
```

Output shows compile status and any diagnostics (warnings/errors).

### simulate

Compile, elaborate, and simulate a design for a configurable number of delta steps.

```bash
qsim simulate counter.v           # 10 delta steps (default)
qsim simulate counter.v 100       # 100 delta steps
```

After each step all signal values are printed. Final wave entry count is displayed.

### bench

Run the performance benchmark — compiles a 4-bit counter, drives the clock via force/release for 10,000 delta steps, and measures throughput.

```bash
qsim bench                        # Run benchmark
qsim bench --save baseline.json   # Save results as JSON baseline
```

Example output:
```
Qiming Performance Benchmark
============================
Design: 4-bit counter, clock driven via force/release
Delta steps: 10000

  Compile:   0.1 ms
  Elaborate: 0.0 ms
  Simulation:    2.5 ms
  Steps done:    10000
  Wave entries:  10001
  Steps/sec:     3963850
  Wave entries/sec: 3964246
```

### mcp

Start the MCP (Model Context Protocol) server for AI agent integration.

```bash
qsim mcp                          # STDIO mode (for Claude Code integration)
qsim mcp --tcp 0.0.0.0:9876       # TCP mode (for remote agents)
```

## GUI Usage

Launch the desktop application:

```bash
cargo tauri dev
```

### Main Window

The GUI is organized as a multi-panel layout:

- **Left panel**: File/project tree and design hierarchy
- **Center**: Monaco editor with Verilog/VHDL syntax highlighting
- **Bottom panels**: Waveform viewer and debug panel

### Workflow

1. **Enter source code** in the editor panel
2. **Click Compile** to parse the design
3. **Click Elaborate** to build the hierarchical design
4. **Click Step Delta** to run single delta steps, or set a step count
5. **View signals** in the signal list with current values
6. **Click Show Waveform** to open the waveform viewer
7. **Use the debug panel** to set breakpoints, watch signals, and control execution

### Waveform Viewer

- **Zoom**: Mouse wheel to zoom in/out horizontally
- **Pan**: Click and drag to scroll through time
- **Cursor**: Hover to see the time value at the cursor position
- Signal names are displayed in the left margin; values shown as digital waveforms

### Debug Panel

- **Signal Watch**: Add signals by name to watch their values update on each step
- **Breakpoints**: Add/remove breakpoints by file and line number; use Continue to run until breakpoint
- **Log**: View simulation log with diagnostics and messages
- **Controls**: Step delta, Continue run, Stop

## Example

### Counter Simulation

Create `counter.v`:
```verilog
module counter(input clk, output reg [3:0] count);
  always @(posedge clk) begin
    count <= count + 4'b1;
  end
endmodule
```

```bash
# Compile
qsim compile counter.v

# Simulate for 20 delta steps
qsim simulate counter.v 20
```

## Troubleshooting

| Problem | Likely Cause | Solution |
|---------|-------------|----------|
| `compile failed` | Syntax error in source | Check diagnostics output |
| `elaboration failed` | Unresolved module reference | Verify module/entity names match |
| `dsim_session_create` returns null | C library not initialized | Ensure `dsim_init()` called before session create |
| C tests fail to build | Missing CUnit | `apt install libcunit1-dev` or `brew install cunit` |
| Tauri build fails | Missing system dependencies | See [Tauri prerequisites](https://v2.tauri.app/start/prerequisites/) |
