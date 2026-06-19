# Basic integration test for the Qiming Python MCP client.
# Requires a running MCP TCP server on 127.0.0.1:9876.
# Run with: python tests/test_basic.py

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from qiming import QimingClient


VERILOG_SRC = """\
module test(input a, output reg x);
  always @(*) x = a;
endmodule;
"""


_MCP_PORT = int(os.environ.get("QIMING_MCP_PORT", "9876"))

def test_compile_elaborate_simulate_eval():
    """Full round-trip: compile -> elaborate -> simulate -> eval."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()

        # Compile
        result = client.compile(VERILOG_SRC, "test.v")
        session_id = result.get("session_id")
        assert session_id, f"no session_id in compile response: {result}"
        assert result.get("success") is not False, f"compile failed: {result}"
        print(f"  compile OK: session={session_id[:8]}...")

        # Elaborate
        result = client.elaborate(session_id)
        assert result.get("success") is not False, f"elaborate failed: {result}"
        print("  elaborate OK")

        # Simulate
        result = client.simulate(session_id)
        assert result.get("success") is not False, f"simulate failed: {result}"
        print("  simulate OK")

        # Eval
        result = client.eval(session_id, "a")
        signal = result.get("signal")
        assert signal == "a", f"eval signal mismatch: {result}"
        print(f"  eval OK: a = {result.get('value', '?')}")

    finally:
        client.close()


def test_get_sessions():
    """get_sessions returns the session list."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        result = client.get_sessions()
        assert "count" in result, f"get_sessions missing count: {result}"
        print(f"  get_sessions OK: count={result.get('count')}")
    finally:
        client.close()


def test_compile_failure_reporting():
    """Compilation failure returns success: false, not an error."""
    client = QimingClient("127.0.0.1", _MCP_PORT)
    try:
        client.connect()
        result = client.compile("not valid verilog", "bad.v")
        # Compilation failure is reported as success: false, not a JSON-RPC error
        assert "success" in result, f"compile result missing success: {result}"
        print(f"  compile failure OK: success={result.get('success')}")
    finally:
        client.close()


if __name__ == "__main__":
    print("test_compile_elaborate_simulate_eval...")
    test_compile_elaborate_simulate_eval()
    print("test_get_sessions...")
    test_get_sessions()
    print("test_compile_failure_reporting...")
    test_compile_failure_reporting()
    print("\nAll tests passed.")
