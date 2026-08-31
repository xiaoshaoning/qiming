# Qiming Re-Evaluation: U74 RISC-V Core (2026-08-31)

Re-tested qsim against the U74 behavioral RISC-V core after the fix batch
(commits `a578a11`..`736c0ac` + uncommitted WIP parser changes: `real`,
hierarchical references, procedural loops, `define` refs, `longint`).
Tested the prebuilt CLI (`target/release/qsim.exe`) only — no qiming source
was modified.

**Verdict: real progress on the grammar gaps — loops, `real`, hierarchical
refs, `$readmemh`, `define` all work now, and `u74_fpu.v` compiles. But the
one bug that motivated the evaluation — variable-indexed continuous-assign
array reads — still returns X, and the CLI's delta-only time model still
blocks the project's testbenches.** The U74 core therefore still cannot be
simulated correctly in qiming; iverilog + ModelSim remain the project's
simulators.

Also flagged: the uncommitted WIP parser builds with `YY_DEBUG=1`, flooding
stderr with hundreds of thousands of token traces per compile — this makes
large-file compiles take minutes and renders CLI output unusable.

---

## 1. Fixed and verified

### 1.1 Procedural loops iterate; loop-based array fills work
```verilog
module t;
    integer i, sum;
    reg [63:0] mem [0:15];
    initial begin
        sum = 0;
        for (i = 0; i < 5; i = i + 1) sum = sum + i;       // 10
        for (integer j = 0; j < 5; j = j + 1) sum = sum + j; // 20
        i = 0; sum = 0;
        while (i < 5) begin sum = sum + i; i = i + 1; end   // 10
        for (i = 0; i < 16; i = i + 1) mem[i] = 64'h1000 + i;
        $display("%h", mem[3]);   // 1003
    end
endmodule
```
All values correct. The previous "loop bodies never execute" bug is gone,
including the `for (integer i = 0; ...)` declaration-in-loop form.

### 1.2 `real` type works (single-name declarations)
```verilog
module t;
    real a, c;              // NOTE: multiple names per decl is BROKEN — see 2.3
    initial begin
        a = 3.5;
        c = a + 2.25;
        $display("%h", $realtobits(c));  // 4017000000000000 (5.75)
    end
endmodule
```
Literals, arithmetic, `$bitstoreal`/`$realtobits` all work — **`u74_fpu.v`
compiles** (previously a hard blocker).

### 1.3 Hierarchical references
```verilog
module child; reg [7:0] val; endmodule
module top;
    child u_child ();
    initial begin
        u_child.val = 8'hAB;
        $display("%h", u_child.val);   // ab
    end
endmodule
```
Instance field reads/writes work. (`$readmemh` into a top-level array also
works; into an instance array was not separately re-tested — the parser now
accepts hierarchical `$readmemh` targets per commit `a7be8dc`.)

### 1.4 `define` macro refs and `longint`
```verilog
`define MAGIC 42
module t;
    longint l;
    initial begin
        l = 64'h123456789;
        $display("%h %d", l, `MAGIC);  // 123456789 42
    end
endmodule
```
Both work.

### 1.5 RTL compile status: 18/22
Compile OK: `u74_pipeline_regs`, `u74_pc_unit`, `u74_alu`,
`u74_compressed_decoder`, `u74_register_file`, `u74_hazard_unit`,
`u74_csr_unit`, `u74_trap_unit`, `u74_mul_div_unit`, `u74_amo_unit`,
`u74_fpu`, `u74_fp_register_file`, `u74_mmu`, `u74_l1i_cache`,
`u74_if_stage`, `u74_id_stage`, `u74_ex_stage`, `u74_mem_stage`.

Not yet confirmed: `u74_decoder`, `u74_pmp`, `u74_l1d_cache`,
`u74_core_top` — all large files whose compiles crawl under the `YY_DEBUG`
flood (section 4). These are believed to be timeouts rather than parse
errors (each compiled OK in an earlier state of the branch).

---

## 2. Still broken (blockers)

### 2.1 Variable-indexed continuous-assign array reads return X  (CRITICAL)
The register-file / L1-cache pattern:
```verilog
module t;
    reg [24:0] tag_arr [0:511];
    reg [8:0]  lid;
    wire [24:0] tag_rd = tag_arr[lid];   // always X
    initial begin
        tag_arr[256] = 25'h12345;
        lid = 9'd256;
        $display("%h", tag_rd);          // x, expected 12345
    end
endmodule
```
`tag_rd` is permanently X. Direct reads **inside `initial` blocks work**
(`rd = mem[idx];`), but the wire / continuous-assign side does not — the
same bug class as iverilog's "constant selects" corruption. This is the
exact reason the U74 L1D cache is verified under ModelSim, and it means
qiming still cannot execute the U74 data path correctly (the D-cache and
MMU are built on this pattern).

**Likely location:** the indexed-read evaluation in
`libqsim/src/uir_sim.c` ("Indexed read: array element access or bit-select",
~line 3262) — the `is_array` detection and the storage lookup for a
continuous-assign/`assign` LHS read.

### 2.2 Generate loops assigning into unpacked arrays produce X
```verilog
genvar g;
generate
    for (g = 0; g < 4; g = g + 1) begin : gb
        assign a[g] = 8'd10 + g;      // all a[g] stay X
    end
endgenerate
```
Likely the same underlying array-wire-read issue as 2.1.

### 2.3 `real a, b;` (multiple names in one declaration) breaks the block
```verilog
module t;
    real a, b;          // two names in one decl
    initial begin
        a = 3.5;
        $display("after-a");          // never printed
    end
endmodule
```
With a single name (`real a;`) the same code works. The multi-name
declaration silently stops the enclosing `initial` block.

### 2.4 `$finish` does not halt the run
```verilog
initial begin
    $display("before");   // printed
    $finish;
    $display("after");    // also printed — finish is a no-op
end
```

### 2.5 CLI is delta-only — no time advancement
`qsim simulate` steps delta cycles; `#10 clk = ~clk;`-style testbenches
never advance time and hang. The C scheduler has
`qsim_scheduler_step_time` / `qsim_scheduler_run(until_time)`
(`libqsim/src/scheduler.c:475` / `:309`) but the Rust session only exposes
`step_delta` (`src-tauri/src/session/mod.rs:77`,
`src-tauri/src/ffi/mod.rs:157`). Without a time-advancing API + a CLI mode
that prints `session.get_log()` (the `$display` log), the project's
time-based testbenches cannot run under qiming at all.

---

## 3. Verified working environment notes

- Build on this machine (windows-gnu toolchain): the CMake generator must be
  `MinGW Makefiles` or the VS-built `qsim.lib` won't link:
  ```bash
  rm -rf libqsim/build-cargo
  CMAKE_GENERATOR="MinGW Makefiles" cargo build --release --offline
  ```
  (`libqsim/build-cargo` is gitignored.)
- Suppress the parser debug flood while testing:
  `qsim compile f.v 2>/dev/null` — results go to stdout, traces to stderr.

---

## 4. Parser debug flood (ship-blocker for the WIP)

The uncommitted grammar/parser changes compile `libqsim/generated/grammar.peg.c`
with `#define YY_DEBUG 1` (line 46), which routes every token match/fail
through `yyprintf` to stderr. Measured: **~477,000 trace lines for one small
module**. Effects:
- large-file compiles take minutes (the U74 decoder was still running after
  15 minutes);
- CLI output is unusable without `2>/dev/null`;
- any tooling that captures stderr fills up.

Turn `YY_DEBUG` off (or gate it behind a build flag) before shipping.

---

## 5. Recommended priorities for the next round

1. **Variable-indexed continuous-assign array reads** (2.1) — the only
   remaining reason the U74 cannot run; also fixes generate-array assigns
   (2.2).
2. **Time advancement in the session API + CLI** (2.5) — expose
   `step_time`/`run_until`, and print the `$display` log after stepping.
3. `real` multi-name declarations (2.3), `$finish` halt (2.4).
4. Disable `YY_DEBUG` (section 4).
5. Re-verify the four large RTL modules compile once #4 lands (1.5).

Each repro above is self-contained; the previous report
(`docs/risc-v-sim-evaluation.md`) retains the original findings and the
code-location map for the earlier issues.
