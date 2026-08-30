# Qiming: RISC-V U74 Evaluation & Improvement Report

Evaluated qsim v0.2.0 (prebuilt `target/release/qsim.exe`) against the
U74 RISC-V core (`D:\Projects\codes\risc_v`, behavioral Verilog, 198-check
regression suite on iverilog). Goal: replace iverilog — specifically to
get correct variable-indexed array access, which iverilog's "constant
selects" quirk corrupts and which is blocking an L1 cache implementation.

**Verdict: not usable for the project today.** 15/19 RTL modules compile,
but a simulator-core loop bug, broken indexed array reads on wires, and
several grammar gaps block everything else. Repros are below with code
locations so each fix has a starting point.

---

## 1. Procedural `for`/`while` loops never iterate  (SIMULATOR BUG)

Loop bodies execute zero times (or once with the initial index). Observed:
a `for (i=0;i<5;i++) sum += i` leaves `sum == 0`; a loop filling an array
leaves only `mem[0]` written. Same in `initial` and `always` blocks.

Repro:

```verilog
module t;
    integer i, sum;
    initial begin
        sum = 0;
        for (i = 0; i < 5; i = i + 1) sum = sum + i;  // stays 0
        $display("sum=%d", sum);                       // 0, expected 10
        i = 0; sum = 0;
        while (i < 5) begin sum = sum + i; i = i + 1; end // stays 0
        $display("sum2=%d", sum);                      // 0, expected 10
    end
endmodule
```

**Impact:** every testbench that inits memory with a loop breaks; our whole
tb layer depends on this.

**Code map:**
- Parser: `libqsim/src/verilog/grammar.peg:523` (`for_stmt`), `:536`
  (`while_stmt`), `:531` (`repeat_stmt`) — check the UIR emission
  (`do_*_enter` actions).
- Executor: `libqsim/src/uir_sim.c:5330` `case UIR_LOOP:` runs
  `init_stmt` + `body` once; `:5336` `case UIR_LOOP_BACK:` evaluates the
  condition and schedules the body re-run via `schedule_stmt_event`. The
  body is an "augmented block" forming a cycle (`UIR_LOOP_BACK.body`
  points back to it — see comments at `:1211` and `:2153`). Suspects:
  loop-back not emitted by the parser, the scheduled re-run never firing,
  or the condition evaluating false on the first back-edge.
- Quick sanity test to isolate parser vs scheduler: print the UIR after
  parse (`qsim compile` diagnostics) and check a `UIR_LOOP_BACK` node
  exists.

---

## 2. Variable-indexed reads of unpacked-array wires return X

Constant-index reads work; runtime-indexed reads of an unpacked array via
a wire (or directly in `$display`) return X. Also breaks generate loops
that assign into arrays (all elements read back X).

Repro (the pattern our L1 cache uses, register-file style):

```verilog
module t;
    reg [38:0] tag_arr [0:7];
    reg [2:0]  set_idx;
    wire [38:0] tag_rd = tag_arr[set_idx];   // always X
    initial begin
        tag_arr[7] = 39'h1234; set_idx = 3'd7;
        $display("%h", tag_rd);              // x, expected 1234
        $display("%h", tag_arr[7]);          // 1234 (constant ok)
    end
endmodule
```

Note: `reg` arrays read/written with plain variable indices **inside
`initial`/`always` blocks work fine** — it is the wire/continuous-assign
side and array-of-wire reads that go X.

**Code map:**
- Indexed read evaluation: `libqsim/src/uir_sim.c:3262` ("Indexed read:
  array element access or bit-select"). Check that an array-element read
  on a net resolves to the array storage (vs a 1-bit net with X) — the
  `is_array` detection only looks at `UIR_SIGNAL`/`UIR_PORT` node kinds.
- Indexed writes: `:3842`, `:4367` ("Indexed write").
- Array-wire reads in `$display` also show a delta-0 ordering issue with
  plain scalar wires (`assign a0 = 8'd10; $display("%d", a0);` prints x at
  delta 0; on a posedge it reads correctly) — see #10 below.

---

## 3. `real` type missing

The grammar has no `real` keyword; `u74_fpu.v` (behavioral FPU on `real`
registers) fails to parse. `real` needs: lexer token, `reg_decl` variant,
UIR storage (64-bit float or double), and eval in expressions/arithmetic.
Our FPU uses `real` + `$rtoi`/`$itor`/`$bitstoreal`-style conversions;
check whether the conversion system tasks exist in the grammar
(`SYS_*` list at `grammar.peg:406-440`).

---

## 4. Ranged `localparam` / `parameter` not supported

`localparam [63:0] X = ...;` fails; only un-ranged parses. Affects 4/19
RTL files (`u74_csr_unit.v`, `u74_fpu.v`, `u74_pmp.v`, `u74_core_top.v`).

**Code map:** `libqsim/src/verilog/grammar.peg:206-207`
(`param_decl` / `param_item <- ID ASSIGN const_expr`). Add an optional
`range_opt` before the ID and carry the width into the constant.

Workaround meanwhile: un-range the localparams in RTL (iverilog-compatible).

---

## 5. Inline register initializers ignored / not parsed

`reg [3:0] a = 4'h5;` parses but `a` stays X. `integer i = 7;` is a parse
error.

**Code map:** `grammar.peg:185` (`reg_decl`), `integer_decl` — add an
optional `( ASSIGN const_expr )?` and emit an init (e.g. a UIR init node
evaluated at elaboration).

Workaround meanwhile: move inits into `initial` blocks.

---

## 6. No hierarchical references (`dut.x`, `$readmemh("f", dut.mem)`)

The grammar has no hierarchical paths; all our tbs reference `dut.*`
internals and load memories via `$readmemh(file, dut.imem)`.

**Code map:** identifier rule in `grammar.peg` (around `ID` +
`hier_identifier`), the signal-name resolution in `uir_sim.c`
(`find_signal_idx`, `:2423` area), and `SYS_READMEMH` at `grammar.peg:406`
(accepts a bare `ID` only — needs a hierarchical path expression and a
target memory that can be an array element under a module instance).

**Impact:** without this, only self-contained testbenches (DUT + harness
in one top module, memories as top-level arrays) can run.

---

## 7. No `$finish`

`SYS_FINISH` is absent; only `SYS_STOP LPAREN expr RPAREN` exists
(`grammar.peg:424-425`). `$stop` without an argument also fails.
Add `$finish`/`$stop` (optionally with 0/1 arg) and end the session when
called (check `do_sys_stop_finish_with_arg`).

---

## 8. CLI is delta-only; time never advances

`qsim simulate` steps delta cycles; `#1`-style delays never fire, so any
time-based testbench (`#10 clk = ~clk;`) hangs. The C scheduler HAS time
support (`qsim_scheduler_run(until_time)` `scheduler.c:309`,
`qsim_scheduler_step_time` `:475`) but the Rust session only exposes
`step_delta` (`src-tauri/src/session/mod.rs:77`,
`src-tauri/src/ffi/mod.rs:157`).

**Code map:** add `qsim_session_step_time` / `qsim_session_run_until` to
`ffi/mod.rs` + `session/mod.rs`, then a `simulate-until` mode in
`src-tauri/src/bin/qsim.rs`.

---

## 9. CLI never prints the `$display` log

`$display` output is routed to a callback → accumulated log
(`session.get_log()` at `session/mod.rs:333`), but `cmd_simulate`
(`bin/qsim.rs:91`) only prints signal dumps. Add
`println!("{}", session.get_log())` after stepping (and clear via
`clear_wave`/log reset) so `$display`-based checks are visible from the
CLI. This alone makes self-contained testbenches usable once #1/#2/#7 are
fixed.

---

## 10. `$display` format flags not substituted

`%d/%h/%b/%o` work; `%0d`, `%02x`, `%-8s` etc. print literally (the
engine switches only on the single char after `%`).

**Code map:** `libqsim/src/uir_sim.c:4556` `sys_format_output` — parse
`%[flags][width][.prec]` before the conversion char.

Also observed: at delta 0, `$display` of a driven wire can read X before
the continuous assign settles (scalar `assign` + immediate `$display`).
Reading on a posedge works. Worth confirming the delta ordering of
`$display` args vs pending assigns.

---

## 11. Build note: windows-gnu toolchain vs MSVC-built libqsim

On this machine the Rust toolchain is `stable-x86_64-pc-windows-gnu` but
`libqsim/build-cargo` was configured with the VS generator → produces
`qsim.lib`; `cargo build` then fails with "could not find native static
library `qsim`" (needs `libqsim.a`). Fix:

```bash
rm -rf libqsim/build-cargo
CMAKE_GENERATOR="MinGW Makefiles" cargo build --release --offline
```

(`build.rs` wipes/retries configure on a stale cache, so the env var is
picked up on the second attempt.) `libqsim/build-cargo` is gitignored.

---

## Priority order (what unblocks the U74 project)

1. **#1 loops** — blocks every memory-init in testbenches.
2. **#2 indexed array reads on wires** — the reason we evaluated qiming
   (L1 cache verification); also fixes generate-array assigns.
3. **#9 log printing + #8 time advancement** — makes CLI-driven
   testbenches actually run and observable.
4. Grammar completeness: #4 ranged localparam (trivial), #5 inline reg
   init, #7 `$finish`, #10 format flags (small), then #3 `real`, #6
   hierarchical refs (larger).

## Suggested regression checklist after fixes

```verilog
// loops
integer i, sum; initial begin sum=0;
  for (i=0;i<5;i=i+1) sum=sum+i; $display("loop=%d", sum); /* 10 */ end
// indexed wire read
reg [63:0] m[0:3]; reg [1:0] k; wire [63:0] r = m[k];
  // after m[3]=..., k=3: r == value
// generate array assign
genvar g; generate for (g=0;g<4;g=g+1) begin:gb assign a[g]=8'd10+g; end
  endgenerate // a[3] == 13
// $finish + time
initial begin #5 x=1; #5 $display("done=%d", x); $finish; end
```

Each repro from this doc is in the U74 repo at
`docs/qiming-evaluation.md` (shorter form) — the `risc_v` working tree was
not otherwise modified for this evaluation.
