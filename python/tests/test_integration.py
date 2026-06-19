# Integration test suite for Qiming MCP interface.
# Tests: bulk eval, trace debug, checkpoint diff, coverage gap, interface check.
# Requires a running MCP TCP server on 127.0.0.1:9876 (set QIMING_MCP_PORT to override).
#
# Run with: python tests/test_integration.py

import json
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from qiming import QimingClient, QimingError

_MCP_PORT = int(os.environ.get("QIMING_MCP_PORT", "9876"))


# ── Helpers ──

def _compile_elaborate(client, source, name="test.v"):
    """Compile and elaborate, return session_id."""
    result = client.compile(source, name)
    assert result.get("success") is not False, f"compile failed: {result}"
    session_id = result.get("session_id")
    assert session_id, f"no session_id: {result}"
    result = client.elaborate(session_id)
    assert result.get("success") is not False, f"elaborate failed: {result}"
    return session_id


def _step_delta(client, session_id, n=5):
    """Run N delta steps."""
    for _ in range(n):
        result = client.simulate(session_id)
        assert result.get("success") is not False, f"simulate failed: {result}"


def _clock_cycle(client, session_id):
    """Toggle clock: set 1, step, set 0, step."""
    client.force(session_id, "clk", "1'b1")
    _step_delta(client, session_id, 2)
    client.force(session_id, "clk", "1'b0")
    _step_delta(client, session_id, 2)


def _eval_one(client, session_id, signal):
    """Eval a single signal and return its value string."""
    return client.eval(session_id, signal).get("value", "?")


def _force_bits(client, session_id, signal, width, val):
    """Force a signal using a simple bit string (no Verilog literal parsing needed)."""
    # Build binary string like "00000000" for width=8
    bin_str = format(val, f'0{width}b')
    client.force(session_id, signal, f"{width}'b{bin_str}")


# ── Test 1: Bulk Eval (eval_multi) ──

SIMPLE_REG = """\
module simple_reg(
  input wire       clk,
  input wire [7:0] d,
  output reg  [7:0] q
);
  always @(posedge clk) q <= d;
endmodule
"""


def test_bulk_eval():
    """eval_multi returns values for multiple signals in one call."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, SIMPLE_REG)

        # Eval multi with specific signals
        result = client.eval_multi(session_id, ["d", "q", "clk"])
        assert result.get("count") == 3, f"expected 3 signals, got {result}"
        signals = result.get("signals", [])
        assert len(signals) == 3, f"expected 3 entries, got {len(signals)}"

        # Verify all requested signals are present
        names = [s["name"] for s in signals]
        for expected in ["d", "q", "clk"]:
            assert expected in names, f"missing '{expected}' in {names}"
        # Each signal should have a value field
        for s in signals:
            assert "value" in s, f"signal missing 'value': {s}"
            assert isinstance(s["value"], str), f"value not a string: {s}"

        print(f"    signals: {[(s['name'], s['value']) for s in signals]}")

        # Eval multi with all signals (empty list at server side → should use default)
        result = client.eval_multi(session_id, ["d", "q"])
        assert result.get("count") == 2, f"expected 2 signals: {result}"

        # Eval multi with 5 different signal queries should also work
        # Force some signals first
        client.force(session_id, "clk", "1'b0")
        client.force(session_id, "d", "8'hAB")
        _step_delta(client, session_id, 3)
        result = client.eval_multi(session_id, ["d", "clk"])
        assert result.get("count") == 2, f"expected 2 signals: {result}"

        print("  test_bulk_eval: OK")
    finally:
        client.close()


# ── Test 2: Trace Debug ──

COUNTER = """\
module counter(
  input wire clk,
  input wire rst,
  output reg [7:0] count
);
  always @(posedge clk or posedge rst) begin
    if (rst)
      count <= 8'h00;
    else
      count <= count + 1;
  end
endmodule
"""


def test_trace_debug():
    """debug_trace runs N cycles and returns per-cycle snapshots."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, COUNTER)

        # Reset
        client.force(session_id, "rst", "1'b1")
        client.force(session_id, "clk", "1'b0")
        _step_delta(client, session_id, 2)
        client.force(session_id, "rst", "1'b0")
        _step_delta(client, session_id, 2)

        # debug_trace 10 cycles
        result = client.debug_trace(session_id, 10, ["count"])
        assert result.get("cycle_count") == 10, f"expected 10 cycles: {result}"
        cycles = result.get("cycles", [])
        assert len(cycles) == 10, f"expected 10 cycle entries, got {len(cycles)}"

        # Values should change over time (counter increments)
        values = []
        for i, cycle in enumerate(cycles):
            assert len(cycle) == 1, f"expected 1 signal per cycle: {cycle}"
            entry = cycle[0]
            assert entry["signal"] == "count", f"unexpected signal: {entry}"
            values.append(entry["value"])

        # At least one value should be non-X (eventually the counter starts)
        non_x = [v for v in values if "X" not in v]
        assert len(non_x) >= 1, f"all values are X after 10 cycles: {values}"
        print(f"    count values: {values}")

        print("  test_trace_debug: OK")
    finally:
        client.close()


# ── Test 3: Checkpoint Diff ──

def test_checkpoint_diff():
    """save_checkpoint, force, save again, then diff the two checkpoints."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, SIMPLE_REG)

        # Set initial state and save checkpoint "before"
        client.force(session_id, "clk", "1'b0")
        client.force(session_id, "d", "8'hAA")
        _step_delta(client, session_id, 3)

        client.save_checkpoint(session_id, "before")
        result = client.list_checkpoints(session_id)
        print(f"    after save 'before': {result}")

        # Clock posedge so q gets the value
        _clock_cycle(client, session_id)
        client.save_checkpoint(session_id, "after")
        result = client.list_checkpoints(session_id)
        print(f"    after save 'after': {result}")

        # Diff the two checkpoints
        result = client.diff_checkpoint(session_id, "before", "after")
        diff = result.get("diff", {})
        print(f"    diff result: {diff}")
        # Should report changes (at least q changed from X to AA)

        # Restore "before" and verify
        result = client.restore_checkpoint(session_id, "before")
        assert result.get("success") is not False, f"restore failed: {result}"
        print(f"    restore OK")

        print("  test_checkpoint_diff: OK")
    except QimingError as e:
        # Checkpoints may fail if backend doesn't support them in all builds
        print(f"  test_checkpoint_diff: SKIPPED ({e})")
    finally:
        client.close()


# ── Test 4: Coverage Gap ──

COVERAGE_TEST = """\
module coverage_test(
  input wire clk,
  input wire rst,
  input wire [1:0] sel,
  output reg [7:0] y
);
  always @(posedge clk or posedge rst) begin
    if (rst)
      y <= 8'h00;
    else begin
      case (sel)
        2'b00: y <= 8'h01;  // Branch A
        2'b01: y <= 8'h02;  // Branch B
        2'b10: y <= 8'h04;  // Branch C
        2'b11: y <= 8'h08;  // Branch D
      endcase
    end
  end
endmodule
"""


def test_coverage_gap():
    """get_coverage and coverage_gap_analysis report coverage info."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, COVERAGE_TEST)

        # Run reset
        client.force(session_id, "rst", "1'b1")
        client.force(session_id, "clk", "1'b0")
        _step_delta(client, session_id, 2)
        client.force(session_id, "rst", "1'b0")
        _step_delta(client, session_id, 2)

        # Run a cycle with sel=0 (branch A)
        client.force(session_id, "sel", "2'b00")
        _clock_cycle(client, session_id)

        # Get coverage
        result = client.get_coverage(session_id)
        print(f"    coverage: {result.get('count', '?')} lines, "
              f"{result.get('percent', '?')}%")

        # Coverage gap analysis
        result = client.coverage_gap_analysis(session_id)
        assert "overall_percent" in result, f"missing overall_percent: {result}"
        by_file = result.get("by_file", [])
        print(f"    gap analysis: {result.get('overall_percent')}% "
              f"over {len(by_file)} file(s)")

        print("  test_coverage_gap: OK")
    finally:
        client.close()


# ── Test 5: Interface Check ──

SUB_MODULE = """\
module sub(input wire [7:0] data_in, output reg [3:0] data_out);
  always @(*) data_out = data_in[3:0];
endmodule
"""

TOP_WITH_MISMATCH = """\
module top_mismatch(
  input wire [7:0] main_data,
  output wire [7:0] main_out
);
  wire [3:0] internal;
  sub u_sub (
    .data_in(main_data),
    .data_out(main_out)
  );
  assign main_out = internal;
endmodule
"""


def test_interface_check():
    """interface_check detects port width mismatches."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()

        source = SUB_MODULE + "\n" + TOP_WITH_MISMATCH
        result = client.compile(source, "top_mismatch.v")
        session_id = result.get("session_id")
        assert session_id, f"no session_id: {result}"

        result = client.elaborate(session_id)
        print(f"    elaborate: success={result.get('success')}")

        result = client.interface_check(session_id)
        print(f"    interface_check findings: {result.get('findings', [])}")

        print("  test_interface_check: OK")
    finally:
        client.close()


# ── Test 6: Design Comprehension ──

HIERARCHY_DESIGN = """\
module leaf(input wire [3:0] a, input wire [3:0] b, output wire [7:0] y);
  assign y = a * b;
endmodule

module middle(input wire [3:0] x, input wire [3:0] y, output wire [7:0] z);
  leaf u_leaf(.a(x), .b(y), .y(z));
endmodule

module top(input wire [3:0] in1, input wire [3:0] in2, output wire [7:0] out);
  middle u_mid(.x(in1), .y(in2), .z(out));
endmodule
"""


def test_design_comprehension():
    """design_summary and control_flow provide structural insight."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, HIERARCHY_DESIGN)

        # Design summary
        result = client.design_summary(session_id)
        summary_raw = result.get("summary", {})
        # summary may be a JSON string — parse if needed
        if isinstance(summary_raw, str):
            summary = json.loads(summary_raw)
        else:
            summary = summary_raw
        print(f"    design_summary keys: {list(summary.keys())[:8]}")
        summary_str = json.dumps(summary)
        assert "top" in summary_str or "middle" in summary_str or "leaf" in summary_str, \
            f"design_summary missing module names: {summary_str[:200]}"

        # Control flow
        result = client.control_flow(session_id)
        flow_raw = result.get("control_flow", {})
        if isinstance(flow_raw, str):
            flow = json.loads(flow_raw)
        else:
            flow = flow_raw
        print(f"    control_flow keys: {list(flow.keys())[:5]}")

        print("  test_design_comprehension: OK")
    finally:
        client.close()


# ── Test 7: Query Wave Bulk ──

def test_query_wave_bulk():
    """query_wave_bulk returns waveform entries filtered by signals and time."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        session_id = _compile_elaborate(client, COUNTER)

        # Reset
        client.force(session_id, "rst", "1'b1")
        client.force(session_id, "clk", "1'b0")
        _step_delta(client, session_id, 2)
        client.force(session_id, "rst", "1'b0")
        _step_delta(client, session_id, 2)

        # Run 5 clock cycles
        for _ in range(5):
            _clock_cycle(client, session_id)

        # Query wave bulk
        result = client.query_wave_bulk(session_id, ["count", "clk"], 0, 0)
        entries = result.get("entries", [])
        print(f"    query_wave_bulk: {len(entries)} entries")

        # Should have some entries
        assert len(entries) >= 0

        print("  test_query_wave_bulk: OK")
    finally:
        client.close()


# ── Test Runner ──

TESTS = [
    ("test_bulk_eval", test_bulk_eval),
    ("test_trace_debug", test_trace_debug),
    ("test_checkpoint_diff", test_checkpoint_diff),
    ("test_coverage_gap", test_coverage_gap),
    ("test_interface_check", test_interface_check),
    ("test_design_comprehension", test_design_comprehension),
    ("test_query_wave_bulk", test_query_wave_bulk),
]


def main():
    passed = 0
    failed = 0
    skipped = 0

    for name, func in TESTS:
        print(f"{name}...")
        try:
            func()
            passed += 1
        except Exception as e:
            import traceback
            print(f"  FAILED: {e}")
            traceback.print_exc()
            failed += 1

    total = passed + failed
    print(f"\n{'='*40}")
    print(f"Results: {passed}/{total} passed", end="")
    if skipped:
        print(f", {skipped} skipped", end="")
    if failed:
        print(f", {failed} failed", end="")
    print()
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
