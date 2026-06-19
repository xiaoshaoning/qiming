# coverage_gap_fill — Improve Line Coverage

Given a design and existing testbench, analyze uncovered lines, determine
why they were missed, and generate targeted stimulus to cover them.

## Prerequisites

- Design source files available on disk
- Existing testbench or simulation script
- MCP tools: `compile`, `elaborate`, `simulate`, `eval`, `force`,
  `get_coverage`, `query_wave`

## Workflow

### Step 1: Establish baseline coverage

1. **Compile** the design:
   ```
   compile({ files: ["<design>.v", "<testbench>.v"] })
   ```

2. **Elaborate**:
   ```
   elaborate({ session_id: "<id>" })
   ```

3. **Run simulation** with the existing testbench:
   ```
   simulate({ session_id: "<id>" })
   ```

4. **Get coverage report**:
   ```
   get_coverage({ session_id: "<id>" })
   ```
   Record:
   - `percent`: overall line coverage
   - `entries`: list of (file, line) pairs that were executed
   - Lines NOT in `entries` are uncovered

### Step 2: Identify uncovered lines

5. Cross-reference `entries` against source files. For each source line
   that does NOT appear in the coverage entries, determine the construct:

   | Construct | Likely Reason Missed |
   |-----------|---------------------|
   | `if` branch | Condition never evaluated to that value |
   | `else` branch | `if` condition always true |
   | `case` item | Selector never matches that value |
   | Sequential block | Enable condition never satisfied |
   | Continuous assign | Input combination never exercised |

6. Group uncovered lines by the signal or condition that controls them.

### Step 3: Analyze control signals

7. For each group, identify the **controlling signals** (the signals in
   the `if` condition, `case` expression, or enable logic).

8. **Eval** controlling signals at simulation end:
   ```
   eval({ session_id: "<id>", signal: "<control_signal>" })
   ```

9. If the controlling signal is stuck at a particular value, the
   stimulus never drove it to the needed state.

### Step 4: Generate targeted stimulus

10. For each uncovered branch, determine what input sequence would
    activate it:

    | Gap | Stimulus |
    |-----|----------|
    | `if (a)` — `else` uncovered | Force `a = 0`, step |
    | `case (sel)` — missing item | Force `sel = <missing_value>`, step |
    | Sequential enable | Assert enable signal, step |
    | Edge-sensitive block | Toggle clock while condition true |

11. Apply stimulus using `force`:
    ```
    force({ session_id: "<id>", signal: "<input>", value: "<value>" })
    ```

12. Advance simulation:
    ```
    simulate({ session_id: "<id>" })
    ```
    or step precisely:
    ```
    debug_step({ session_id: "<id>" })
    ```

13. **Check coverage** after each stimulus application:
    ```
    get_coverage({ session_id: "<id>" })
    ```
    If coverage percent increased, the stimulus was effective.

### Step 5: Iterate

14. Repeat Steps 10–13 for each uncovered group until desired coverage
    threshold is reached (typically 90%+ line coverage).

15. If coverage plateaus below target:
    - **Dead code**: some uncovered lines may be unreachable (e.g., a
      `case` item for an impossible input combination). Flag these for
      design review rather than forcing artificial stimulus.
    - **Reset sequence**: some designs require a specific reset sequence
      before certain paths become reachable. Apply reset first, then
      stimulus.

### Step 6: Document results

16. Summarize:
    - Starting coverage percent
    - Final coverage percent
    - List of uncovered lines remaining (if any) with reason (dead code,
      unreachable, requires specific sequence)

## Example

```
Design: arbiter.v with round-robin state machine
Baseline coverage: 62%

Uncovered lines:
  arbiter.v:24 — else branch of if (grant)  (never false)
  arbiter.v:31 — case 2'b10 in state machine (state never = 2'b10)
  arbiter.v:45 — sequential block under posedge grant_valid

Analysis:
- eval(grant) -> 1 (always asserted — never drove grant=0)
- eval(state) -> 2'b01 (never transitions through 2'b10)

Stimulus:
1. force(grant, 0), step -> state advances, else branch covered
2. Drive requests to specific pattern, step 4 clocks ->
   state=2'b10 case item covered
3. Force grant_valid=1, step -> sequential block covered

Final coverage: 94%
Remaining uncovered: arbiter.v:52 (dead code — case 2'b11 impossible)
```

## Limitations

- No toggle coverage or FSM state coverage yet — line coverage only.
- No automatic testbench generation — stimulus patterns are inferred
  manually from source analysis.
- Coverage is per-simulation-session; accumulated coverage across
  multiple testbenches requires manual aggregation.
