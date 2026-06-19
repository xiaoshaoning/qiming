# auto_debug — Root-Cause Diagnosis from Assertion Failure

Given a failing assertion or unexpected signal value, trace the dependency
chain to identify the root cause.

## Prerequisites

- Design source files available on disk
- MCP server running (`qsim --mcp` or `qsim --tcp <addr>`)
- List of MCP tools: `compile`, `elaborate`, `simulate`, `eval`, `force`,
  `release`, `query_wave`, `get_dependencies`, `add_breakpoint`,
  `remove_breakpoint`, `list_breakpoints`, `debug_run`, `debug_step`,
  `get_coverage`

## Workflow

### Step 1: Load and simulate

1. **Compile** the design files:
   ```
   compile({ files: ["top.v", ...] })
   ```
   Expect `success: true`. If compilation fails, report the diagnostics
   and stop.

2. **Elaborate** the compiled design:
   ```
   elaborate({ session_id: "<id>" })
   ```

3. **Simulate** to reach the failing time or sequence:
   ```
   simulate({ session_id: "<id>" })
   ```
   If the assertion failure involves a specific sequence of inputs,
   use `force` to drive input signals then `step` to advance.

### Step 2: Read the failing signal

4. **Eval** the failing signal:
   ```
   eval({ session_id: "<id>", signal: "<failing_signal>" })
   ```
   Note the returned `value`. Compare against the expected value from
   the design specification.

### Step 3: Trace the dependency chain

5. **Get dependencies** for the failing signal:
   ```
   get_dependencies({ session_id: "<id>" })
   ```
   Inspect the `signals` list to understand the design's signal topology.
   (Note: the current implementation returns all signals; cross-reference
   with the failing signal's drivers in the source code.)

6. **Identify driver signals** — read the source around the failing
   signal's assignment. For combinational logic (`always @(*)` or
   `assign`), the drivers are the signals on the right-hand side. For
   sequential logic (`always @(posedge clk)`), check both the clock and
   data inputs.

7. **Eval each driver** in reverse topological order:
   ```
   eval({ session_id: "<id>", signal: "<driver_signal>" })
   ```
   Work backward from the failing signal toward the design inputs. At
   each step, compare the actual value against the expected value.

### Step 4: Isolate the root cause

8. If a **driver signal has an unexpected value**, recurse into Step 3
   using that signal as the new target.

9. If a **driver signal is X (unknown)**:
   - Check if it was forced/released. An unreleased force can leave a
     signal at X.
   - Check if any of its drivers are X.
   - X typically propagates from uninitialized registers, contention,
     or unresolved multiple drivers.

10. If a **driver signal is Z (high-impedance)**:
    - Check for unconnected ports or disabled tri-state buffers.

11. If the failing signal and all drivers have correct values, the
    issue may be in:
    - **Timing**: the signal was evaluated before propagation completed.
      Use `step` to advance one delta and `eval` again.
    - **Sensitivity list**: a combinational `always` block missing a
      signal in its sensitivity list. Compare the source sensitivity
      list against the signals used in the body.

### Step 5: Fix and verify

12. Apply the fix to the source file and recompile:
    ```
    compile({ files: ["<fixed_file>"], ... })
    elaborate({ session_id: "<new_id>" })
    simulate({ session_id: "<new_id>" })
    eval({ session_id: "<new_id>", signal: "<failing_signal>" })
    ```
    Confirm the signal now has the expected value.

13. Optionally use `query_wave` to dump the full waveform for visual
    inspection:
    ```
    query_wave({ session_id: "<id>" })
    ```

## Example

```
Assertion failed: counter.count == 4'd5 at time 40 ns

1. compile({ files: ["counter.v"] })
2. elaborate({ session_id: "s1" })
3. simulate({ session_id: "s1" })
4. eval({ session_id: "s1", signal: "count" }) -> "0000" (unexpected)

5. get_dependencies({ session_id: "s1" })
   -> signals: ["clk", "rst", "count"]

6. Source shows count <= count + 1 on posedge clk.
7. eval({ session_id: "s1", signal: "clk" }) -> "X"
   Root cause: clock not toggling — no stimulus applied.

8. force({ session_id: "s1", signal: "clk", value: "1" })
   step({ session_id: "s1" })
   eval({ session_id: "s1", signal: "count" }) -> "0001"
   Confirmed.
```

## Limitations

- `get_dependencies` returns all signals, not driver-specific data.
  Manual source inspection is required for fine-grained fan-in/fan-out.
- X-propagation tracing is manual — no automatic X-source tracking yet.
