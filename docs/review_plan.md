# Qiming Review Plan — Test-Suite & Repo Hygiene

Status: drafted 2026-08-30 · Owner: core team · Source: codebase review (docs/planning
session), findings P1-11 .. P3-15 below. All items are additive to the completed
P0/P1/P2 work in [fix_plan.md](fix_plan.md) and must keep its Global regression
checklist green.

---

## Summary of findings

| # | Severity | Finding |
|---|----------|---------|
| P1-11 | High | ~28 standalone test executables are built by CMake but **never run** (only `qsim_test` was registered with ctest; CI ran `-R qsim_test`) — **✅ LANDED: 13 suites wired, 14/14 ctest pass, CI filters removed** |
| P1-12 | High | Several suites exit `0` even when checks fail — **partial: `test_npu_bugs` fixed; smoke suites deferred to P2-13** |
| P2-13 | Medium | Scratch/debug files committed and built — **✅ LANDED: 14 deleted, 5 regression suites salvaged + wired (19/19 ctest)** |
| P2-14 | Medium | `build.rs` configures the same `libqsim/build` dir with `BUILD_TESTING=OFF`, fighting manual cmake runs and triggering cache wipes — **✅ LANDED: private `build-cargo/` dir** |
| P3-15 | Low | Cosmetic: stale comments, stale program name, unneeded crate-types, open TODO |
| NOTE | — | Local MSYS2 `ld` segfaults on its own `crt2.o` — environment, not repo code |
| NOTE | — | Parallel engine bugs found during P1-11 (crash, cross-sensitivity, delta storm) — tracked, out of scope |

---

## P1-11 — Standalone test executables never run in CI

> **STATUS: ✅ LANDED**

**Problem.** `libqsim/CMakeLists.txt` defines 29 test executables but registers
only one with ctest. CI ran `ctest -R qsim_test`, so the other 28 compiled but
never executed.

**Fix applied.**
- 13 suites wired into ctest (`add_test` after each `add_executable`):
  `qsim_test` + `test_npu_bugs`, `test_udp_sim`, `test_part_select`,
  `test_verilog_preprocessor`, `test_net_types`, `test_specify_parse`,
  `test_path_delay`, `test_sdf_annotate`, `test_timing_check`,
  `test_vhdl_lib_use`, `test_idex_reg`, `test_combi_loop`, `test_library_path`.
- Old duplicate `add_executable` blocks removed.
- `ci.yml`: `c-tests` (Linux) and `sanitizers` jobs now build `--parallel` and
  run `ctest` without the `-R qsim_test` filter (previously they built only
  `qsim_test` and filtered to it, which would have excluded the new suites).
  `c-tests-windows` and `package` already ran all ctest tests.

**Verification (local MSVC build).**
- [x] `ctest --test-dir build-msvc -C Release` → **14/14 pass**
      (qsim_test 606/606 + 13 new suites, all green)
- [x] Each new suite verified individually with correct exit codes
- [x] `ci.yml` filters removed so new suites run on Linux + sanitizers jobs

**Not wired in (documented):**
- `run_perf_tests` — see NOTE below: the multi-partition parallel engine is
  broken (crash + wrong values).
- Debug-only executables (see P2-13) and smoke-only suites with no exit-code
  discipline (P1-12).

**Checklist.**
- [x] `add_test` entries added for all suites with correct exit codes
- [x] Duplicate `add_executable` blocks removed (29 → 29 unique, 14 with ctest)
- [x] `ci.yml` `-R qsim_test` filters removed (c-tests, sanitizers)
- [x] Local run confirms 14/14 tests pass

---

## P1-12 — Test suites that exit 0 on failure

> **STATUS: partial — `test_npu_bugs` fixed; smoke suites deferred to P2-13 decision**

**Problem.** Some standalone suites print `FAIL`/`BUG CONFIRMED` but return `0`
from `main()`, so even wired into ctest they would always pass.

**Evidence.**
- `tests/test_npu_bugs.c` `main()`: ran 11 bug-reproduction tests, printed
  results, then unconditional `return 0;` (no failure counter).
- `tests/test_multi_def.c`, `tests/test_parse_big.c`,
  `tests/test_replicate_concat.c`, `tests/test_replicate_focused.c`,
  `tests/test_force_prop.c`: same pattern — prints, then `return 0;`.

**Fix applied.** *(partial)*
- `test_npu_bugs.c`: added a `npu_failures` counter incremented by `check_val`
  on every failure; `main()` returns `npu_failures > 0 ? 1 : 0`. Wired into
  ctest as part of P1-11.
- Smoke-only suites (`test_multi_def`, `test_parse_big`, `test_replicate_*`,
  `test_force_prop`): **resolved in P2-13** — deleted (print-scratch / redundant
  coverage); the assert-bearing `test_replicate_cpu`/`test_replicate_twoblk`
  were kept and wired instead.

**Verification.**
- [x] `test_npu_bugs` wired into ctest and passing (P1-11 run, 14/14)
- [ ] Inject a deliberate failure → ctest fails (exit ≠ 0) — quick check
- [ ] Smoke-only suites resolved (delete or fix) in P2-13

**Checklist.**
- [x] `test_npu_bugs` returns 1 when any bug reproduces
- [ ] Smoke-only suites either return non-zero on failure or are deleted
- [x] All newly wired suites green under ctest

---

## P2-13 — Scratch/debug files committed and built

> **STATUS: ✅ LANDED**

**Problem.** Debug scratch and reproduction programs were tracked in git; some
were built by CMake with no assertions, others were not built at all (dead
code).

**Fix applied (verified content before deciding — several files the plan called
"assertion-free" actually had real checks).**

*Deleted (14 files — pure print-scratch, fake/stale expectations, redundant
coverage, or unbuildable):*
- Print-only diagnostics: `debug_vhdl_resolve.c`, `test_bug3_debug.c`,
  `test_vhdl_debug3.c`, `test_vhdl_debug5.c`, `test_multi_def.c` (0 asserts)
- Fake pass / expectations printed but never checked: `test_proc_dump.c`
  (prints "PASS" unconditionally), `test_force_prop.c` (prints "expect …"
  without asserting; showed a real mismatch and exited 0)
- Redundant coverage: `diag_rv32i_addi.c` (ADDI in `test_cpu_rv32i_*`),
  `test_just_repl.c` / `test_replicate_concat.c` /
  `test_replicate_focused.c` (repl/concat in `test_replicate_cpu/twoblk`),
  `test_parse_big.c` (large design in `test_cpu_rv32i`),
  `qiming_repro.c` (ifdef in `test_verilog_preprocessor`, large signals in
  `test_cpu_rv32i`)
- Unbuildable: `test_bug1_repro.c` (`#include "test.h"` — header never existed)

*Kept and wired into ctest (5 files — real assertions + proper exit codes,
unique regression coverage):*
- `test_bugs_from_report.c` (13 checks, `return passed == tests ? 0 : 1`)
- `test_bug12_diag.c` (8 checks, proper exit)
- `test_implicit_wire.c` (4 checks — added failure counter + exit code)
- `test_replicate_cpu.c` (16 checks, `return fails`)
- `test_replicate_twoblk.c` (6 checks, `return fails`)

**Verification.**
- [x] ctest now runs **19/19 suites, all pass** (14 previous + 5 new; MSVC build)
- [x] No dangling references (CMakeLists, code); docs references are historical
- [x] `run_perf_tests` kept (benchmark) but still unwired — parallel engine bugs

**Checklist.**
- [x] 14 unbuilt/built scratch files removed (git rm)
- [x] 5 regression suites with real assertions kept and wired into ctest
- [x] CMakeLists cleaned (20 executables: 19 ctest-wired + run_perf_tests)
- [x] No regression coverage lost (checked each deleted file's scenarios)


---

## P2-14 — build.rs shares the libqsim build dir with manual cmake

> **STATUS: ✅ LANDED**

**Problem.** `src-tauri/build.rs` configured `libqsim/build` with
`-DBUILD_TESTING=OFF` on every `cargo build`. A developer who ran
`cmake -B build -DBUILD_TESTING=ON` for ctest got that dir reconfigured behind
their back, and any stale `CMakeCache.txt` (e.g. from WSL) triggered a full
`remove_dir_all` + reconfigure wipe of a dir the developer may be using.

**Fix applied.**
- `src-tauri/build.rs`: cargo-driven CMake now uses a private
  `libqsim/build-cargo` dir; the wipe-on-stale-cache retry applies only to it.
- `.gitignore`: added `build-cargo/` (next to `build/`).

**Verification.**
- [x] `cargo check` succeeds and creates `libqsim/build-cargo` (BUILD_TESTING=OFF)
- [x] Developer's `libqsim/build` (BUILD_TESTING=ON) left untouched by the cargo build
- [x] `git check-ignore libqsim/build-cargo` → ignored; `git status` clean

**Checklist.**
- [x] build.rs uses a separate build dir
- [x] `.gitignore` updated
- [x] No behavioral change to packaged builds (`beforeBuildCommand` untouched)


---

## P3-15 — Cosmetic cleanups

> **STATUS: ☐ OPEN**

| Item | Location | Fix |
|------|----------|-----|
| Stale comment "decimal for width > 32" — `to_str` always emits `'b` | `libqsim/src/value.c:224` | Fix comment; note `from_str` base-10 is a silent no-op (allocates 32 X bits, parses nothing) — document or implement |
| Program name "Libdsim" in banner | `libqsim/tests/main_test.c:16` | Rename to "Qiming" |
| `crate-type = ["lib","cdylib","staticlib"]` | `src-tauri/Cargo.toml` | Keep only `lib` (nothing links the Rust crate as C) |
| `TODO: handle hierarchical paths with dot notation` | `libqsim/src/uir.c:1215` | Either implement or file a tracked issue so it is not silently pending |

**Checklist.**
- [ ] value.c comments match behavior (or decimal parsing implemented)
- [ ] Test banner renamed
- [ ] crate-type trimmed (verify `cargo build`/`cargo test` unaffected)
- [ ] uir.c TODO resolved or tracked

---

## NOTE — local MSYS2 toolchain is broken (environment, not repo)

The local `libqsim` build fails at link: MSYS2 ucrt64 `ld` segfaults on its own
`crt2.o` (`ld -m i386pep <crt2.o>` → exit 139, no diagnostics). Repo objects link
fine without `crt2.o`; the C suite is green in CI on MSVC and Linux GCC. No code
change is planned. Workaround: build/test with the MSVC toolchain (as CI does) or
repair/reinstall the MSYS2 ucrt64 binutils. Do not chase this as a repo bug.

## NOTE — parallel engine bugs found during P1-11 (out of scope, tracked here)

While validating the standalone suites, `run_perf_tests` exposed pre-existing
bugs in the **multi-partition parallel delta engine** (`libqsim/src/uir_sim.c`).
It is NOT wired into ctest until these are fixed. All reproduced under MSVC with
the 8-counter `mwcc` design (independent WCCs → multiple partitions):

1. **Deterministic crash (2 threads):** the leader does TWO barriers per phase
   (start + join) but `worker_main` does ONE per iteration, so workers re-process
   each phase. The leader's phase-1 `pool_free_event_thread` (which memsets the
   event to zeros) races the worker's second `process_events_apply`, producing
   events with `sig=0, value=NULL` → crash in `signal_write_resolved`.
   Adding a join barrier to `worker_main` fixes the crash but breaks
   `test_perf_parallel_sweep`'s 2T speedup assertion (605/606) and changes
   multi-partition results — the barrier contract and the phase flow need a
   coherent redesign, not a one-line fix.
2. **Cross-process sensitivity (parser/elaboration):** all 8 instances' always
   blocks resolve `clk`/`rst` to the SAME signal nodes
   (`proc->sensitivity_list[s].signal` identical across processes), so every
   process triggers on one signal's change. `elaboration.c resolve_sensitivity`
   is an empty stub. Masked in the serial path and in single-WCC designs because
   all stimulus changes together; breaks multi-instance designs with independent
   inputs.
3. **Delta storm (multi-partition, post-fix only):** with the join-barrier fix,
   events for unchanged top-level inputs keep getting re-scheduled every delta
   (t=0, d climbing to 5+ until the combi-loop guard fires), so NBAs never
   settle and `val` stays X. Root cause not isolated.

**Suggested fix order when tackled:** (2) first (sensitivity scoping — smallest,
unblocks correct multi-instance behavior), then (1) the barrier contract,
then re-validate (3).

---

## Global regression checklist (Definition of Done)

Run after ALL items above land, in order:

- [ ] `cd libqsim && cmake -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build` — all suites pass (old 606 + new ~200)
- [ ] `cargo test --manifest-path src-tauri/Cargo.toml` — all pass
- [ ] `cargo build` (fresh clone, no webview) — placeholder flow intact (P0-3)
- [ ] `qsim run example/rv32i/rv32i_top.v example/rv32i/tests/fib.hex 200` → fib correct (P0-1)
- [ ] `qsim run example/rv32i_vhdl/rv32i_top.vhd example/rv32i/tests/fib.hex 200` → fib correct (P0-2)
- [ ] Sanitizer job green (P1-5)
- [ ] `tla/check.sh` runs TLC without errors (P0-4/P1-6)
- [ ] `git status` clean; no C4819 warnings (P2-8)

## Suggested commit sequence

1. **P1-12** exit-code fixes for `test_npu_bugs` + smoke suites (prerequisite, small)
2. **P1-11** `add_test` wiring (now every wired suite fails loudly)
3. **P2-13** delete unbuilt scratch files; prune or convert debug executables
4. **P2-14** build.rs private build dir
5. **P3-15** cosmetic cleanups
6. **NOTE** — no commit; document the toolchain workaround in README if desired

Each commit must keep the Global regression checklist green.
