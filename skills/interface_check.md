# interface_check — Port Connection and Width Verification

Verify that module instance port connections are correct: all ports
connected, widths match, and no driver conflicts exist.

## Prerequisites

- Top-level design source files on disk
- MCP tools: `compile`, `elaborate`, `eval`, `get_dependencies`
- Knowledge of the expected port list for each instantiated module

## Workflow

### Step 1: Load and elaborate

1. **Compile** all design files:
   ```
   compile({ files: ["top.v", "sub.v", ...] })
   ```
   Fix any compilation errors before proceeding.

2. **Elaborate** with the top-level module:
   ```
   elaborate({ session_id: "<id>" })
   ```
   Elaboration will fail if instances cannot be resolved or ports
   don't match. If elaboration fails, check the error message for
   mismatched port names or widths.

### Step 2: Inspect the signal list

3. **Get all signals** in the elaborated design:
   ```
   get_dependencies({ session_id: "<id>" })
   ```
   The response contains a `signals` array with all hierarchical signal
   names (e.g., `top.uut.data`, `top.clk`).

4. Verify that expected hierarchical signals exist:
   - `top.<instance>.<port>` — every instance port should appear as a
     signal path
   - `top.<net>` — top-level connections should be present

### Step 3: Check port presence

5. For each module instance, confirm every port has a connected signal:
   ```
   eval({ session_id: "<id>", signal: "top.<inst>.<port>" })
   ```
   If the signal doesn't exist, `eval` returns an error. This indicates
   an unconnected port or a name mismatch.

6. Check for **missing ports** by scanning the source. Common issues:
   - Instance port name typo (e.g., `data_out` vs `datao`)
   - Port declared but not connected (left dangling)
   - Named port connection using wrong name (e.g.,
     `.data(data_bus)` where module port is named `dout`)

### Step 4: Check width consistency

7. For each port connection, determine the expected width from the
   module declaration and the actual width from the connected signal.

   Use `eval` to check the signal exists — signal width information
   is not directly returned by the current tools, so cross-reference
   against the source declarations:
   - Module port width: `input [7:0] data` → 8 bits
   - Connected net width: `wire [7:0] data_bus` → 8 bits
   - Match: widths are equal
   - Mismatch: flag for review

8. Common width issues:
   - **Truncation**: wider net connected to narrower port (e.g., 16-bit
     bus connected to 8-bit port) — data loss
   - **Extension**: narrower net connected to wider port — X or Z on
     upper bits
   - **Part select**: `input [3:0]` connected to `wire [7:4]` — check
     that the part select is intentional

### Step 5: Check direction

9. For each instance port, verify connection direction:
   - **Input ports** must be driven by a net or reg in the parent
   - **Output ports** must connect to a net (or reg in Verilog)
   - **Inout ports** must connect to a net (not a reg)

10. Common direction violations:
    - Output port connected to a `reg` in the parent (legal in Verilog,
      but check that it's not accidentally driven)
    - Input port left unconnected (defaults to Z)
    - Inout port connected to a `reg` (illegal)

### Step 6: Verify logical connectivity

11. For critical paths, trace through the hierarchy:
    - Eval a signal at the top level
    - Eval the same signal at the instance boundary
    - Confirm values match (after propagation)

12. If there's a **value mismatch** between hierarchical levels, check:
    - Port reversal: signals connected to wrong ports (e.g., `a` and `b`
      swapped)
    - Width mismatch: partial connectivity
    - Direction error: signal driven from both sides (contention → X)

## Example

```
Design: top.v instantiates counter #(.WIDTH(8)) uut (...)

1. compile({ files: ["top.v", "counter.v"] })
2. elaborate({ session_id: "s1" })

3. get_dependencies({ session_id: "s1" })
   -> signals: ["top.clk", "top.rst", "top.count",
                "top.uut.clk", "top.uut.rst", "top.uut.count",
                "top.uut.internal_count"]

   All expected signals present.

4. eval({ session_id: "s1", signal: "top.uut.internal_count" })
   -> error: signal not found
   Issue: internal_count is not in the port list — likely missing
   declaration in the counter module.

5. Source check: counter.v declares `reg [7:0] internal_count`
   inside the always block but not as a top-level signal.
   Fix: add `output reg [7:0] internal_count` to port list.

Width check:
   counter declares output [7:0] count
   top.v connects: .count(count[3:0]) — width mismatch!
   Flag: 8-bit port connected to 4-bit net — truncation warning.

Direction check:
   All ports properly connected, no inout ports.
```

## Limitations

- Width and direction must be verified against source code — the current
  MCP tools return signal names but not port widths or directions.
- No automatic port-vs-declaration cross-checking — manual source
  inspection is required for detailed verification.
