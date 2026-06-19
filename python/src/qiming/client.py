# JSON-RPC MCP client for Qiming Simulator.
# Connects to qsim --tcp, all 30+ MCP tools as Python methods.
# Uses only stdlib — no external dependencies.

import json
import socket


class QimingError(Exception):
    """Raised when the MCP server returns a JSON-RPC error response."""
    def __init__(self, code: int, message: str, data=None):
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"[{code}] {message}")


class ReadError(Exception):
    """Raised when reading from the server fails."""


class QimingClient:
    """Low-level JSON-RPC client for the Qiming MCP server.

    Uses Content-Length header framing over a TCP socket.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 9876):
        self._host = host
        self._port = port
        self._sock: socket.socket | None = None
        self._id = 0
        self._buf = b""

    def connect(self):
        """Open the TCP connection."""
        self._sock = socket.create_connection((self._host, self._port), timeout=30)
        self._id = 0
        self._buf = b""

    def close(self):
        """Close the TCP connection."""
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()

    # ── Internal helpers ──

    def _call(self, method: str, params: dict | None = None) -> dict:
        if not self._sock:
            raise ReadError("not connected — call connect() first")

        self._id += 1
        req = json.dumps({
            "jsonrpc": "2.0",
            "id": self._id,
            "method": method,
            "params": params or {},
        })
        header = f"Content-Length: {len(req)}\r\n\r\n"
        try:
            self._sock.sendall((header + req).encode("utf-8"))
        except OSError as e:
            raise ReadError(f"send failed: {e}") from e

        return self._read_response()

    def _read_response(self) -> dict:
        """Read one JSON-RPC response from the server.

        Uses Content-Length header framing:
          Content-Length: N\r\n\r\n{BODY of exactly N bytes}

        Strategy: accumulate buffer until we have a complete message,
        then parse and return.
        """
        # Consume any leftover newlines from previous response
        self._buf = self._buf.lstrip(b"\r\n")

        # Keep reading until we have a complete Content-Length + body
        while True:
            # Try to find Content-Length in buffer
            # Format: Content-Length: N\r\n\r\n<json body>
            header_end_marker = b"\r\n\r\n"
            idx = self._buf.find(header_end_marker)

            if idx < 0:
                # Try just \n\n (some implementations)
                header_end_marker = b"\n\n"
                idx = self._buf.find(header_end_marker)
                if idx >= 0:
                    # Adjust — consume the \n\n
                    idx += 2
                else:
                    # No complete header yet, need more data
                    try:
                        data = self._sock.recv(4096)
                    except OSError as e:
                        raise ReadError(f"recv failed: {e}") from e
                    if not data:
                        raise ReadError("connection closed by server")
                    self._buf += data
                    continue
            else:
                idx += 4  # skip \r\n\r\n

            # Extract Content-Length from header
            header_part = self._buf[:idx].decode("utf-8", errors="replace")
            content_length = None
            for line in header_part.split("\r\n"):
                line = line.strip()
                if line.startswith("Content-Length:"):
                    try:
                        content_length = int(line.split(":", 1)[1].strip())
                    except (ValueError, IndexError):
                        raise ReadError("invalid Content-Length header")
                    break

            if content_length is None:
                # Partial header or no Content-Length — get more data
                try:
                    data = self._sock.recv(4096)
                except OSError as e:
                    raise ReadError(f"recv failed: {e}") from e
                if not data:
                    raise ReadError("connection closed by server")
                self._buf += data
                continue

            # We have Content-Length, check if body is complete
            body_start = idx
            body_end = body_start + content_length
            if len(self._buf) < body_end:
                # Need more body data
                needed = body_end - len(self._buf)
                try:
                    data = self._sock.recv(needed)
                except OSError as e:
                    raise ReadError(f"recv failed: {e}") from e
                if not data:
                    raise ReadError(
                        "connection closed: expected more body data"
                    )
                self._buf += data
                continue

            # Complete message in buffer
            body = self._buf[body_start:body_end]
            self._buf = self._buf[body_end:]

            resp = json.loads(body.decode("utf-8"))
            if "error" in resp:
                err = resp["error"]
                raise QimingError(
                    err.get("code", 0),
                    err.get("message", "unknown error"),
                    err.get("data"),
                )
            return resp.get("result", resp)

    # ── MCP tools ──

    def compile(self, source: str, name: str = "<inline>") -> dict:
        """Compile Verilog or VHDL source string."""
        return self._call("compile", {"source": source, "name": name})

    def elaborate(self, session_id: str) -> dict:
        """Elaborate the compiled design."""
        return self._call("elaborate", {"session_id": session_id})

    def simulate(self, session_id: str) -> dict:
        """Run one delta step of simulation."""
        return self._call("simulate", {"session_id": session_id})

    def eval(self, session_id: str, signal: str) -> dict:
        """Evaluate a signal."""
        return self._call("eval", {"session_id": session_id, "signal": signal})

    def force(self, session_id: str, signal: str, value: str) -> dict:
        """Force a signal to a value."""
        return self._call("force", {
            "session_id": session_id,
            "signal": signal,
            "value": value,
        })

    def release(self, session_id: str, signal: str) -> dict:
        """Release a forced signal."""
        return self._call("release", {
            "session_id": session_id,
            "signal": signal,
        })

    def query_wave(self, session_id: str) -> dict:
        """Get waveform entries."""
        return self._call("query_wave", {"session_id": session_id})

    def get_sessions(self) -> dict:
        """List active session IDs."""
        return self._call("get_sessions")

    def get_log(self, session_id: str) -> dict:
        """Get simulation log."""
        return self._call("get_log_summary", {"session_id": session_id})

    def add_breakpoint(self, session_id: str, file: str, line: int) -> dict:
        """Add a breakpoint at (file, line)."""
        return self._call("add_breakpoint", {
            "session_id": session_id,
            "file": file,
            "line": line,
        })

    def remove_breakpoint(self, session_id: str, file: str, line: int) -> dict:
        """Remove a breakpoint at (file, line)."""
        return self._call("remove_breakpoint", {
            "session_id": session_id,
            "file": file,
            "line": line,
        })

    def list_breakpoints(self, session_id: str) -> dict:
        """List all breakpoints."""
        return self._call("list_breakpoints", {"session_id": session_id})

    def debug_run(self, session_id: str) -> dict:
        """Run simulation until breakpoint."""
        return self._call("debug_run", {"session_id": session_id})

    def debug_step(self, session_id: str) -> dict:
        """Step one delta."""
        return self._call("debug_step", {"session_id": session_id})

    def get_coverage(self, session_id: str) -> dict:
        """Get line coverage data."""
        return self._call("get_coverage", {"session_id": session_id})

    def get_dependencies(self, session_id: str) -> dict:
        """Get signal dependencies."""
        return self._call("get_dependencies", {"session_id": session_id})

    def export_vcd(self, session_id: str, path: str) -> dict:
        """Export wave buffer to VCD file."""
        return self._call("export_vcd", {
            "session_id": session_id,
            "path": path,
        })

    def eval_multi(self, session_id: str, signals: list) -> dict:
        """Evaluate multiple signals in one call."""
        return self._call("eval_multi", {
            "session_id": session_id,
            "signals": signals,
        })

    def query_wave_bulk(self, session_id: str, signals: list = None,
                        t_start: int = 0, t_end: int = 0) -> dict:
        """Query waveform entries filtered by signals and time window."""
        params = {"session_id": session_id, "t_start": t_start, "t_end": t_end}
        if signals is not None:
            params["signals"] = signals
        return self._call("query_wave_bulk", params)

    def debug_trace(self, session_id: str, cycles: int, signals: list) -> dict:
        """Run N clock cycles and return snapshots of requested signals."""
        return self._call("debug_trace", {
            "session_id": session_id,
            "cycles": cycles,
            "signals": signals,
        })

    def design_summary(self, session_id: str) -> dict:
        """Get design hierarchy tree, port list, signal count per module."""
        return self._call("design_summary", {"session_id": session_id})

    def control_flow(self, session_id: str) -> dict:
        """Extract FSM states and transitions via signal analysis."""
        return self._call("control_flow", {"session_id": session_id})

    def save_checkpoint(self, session_id: str, name: str) -> dict:
        """Save a simulation checkpoint."""
        return self._call("save_checkpoint", {
            "session_id": session_id,
            "name": name,
        })

    def restore_checkpoint(self, session_id: str, name: str) -> dict:
        """Restore simulation from a checkpoint."""
        return self._call("restore_checkpoint", {
            "session_id": session_id,
            "name": name,
        })

    def diff_checkpoint(self, session_id: str, checkpoint_a: str,
                        checkpoint_b: str) -> dict:
        """Diff two checkpoints to see signal-level changes."""
        return self._call("diff_checkpoint", {
            "session_id": session_id,
            "checkpoint_a": checkpoint_a,
            "checkpoint_b": checkpoint_b,
        })

    def list_checkpoints(self, session_id: str) -> dict:
        """List all saved checkpoints."""
        return self._call("list_checkpoints", {"session_id": session_id})

    def trace_drivers(self, session_id: str, signal: str,
                      max_depth: int = 3) -> dict:
        """Trace the driver chain of a signal."""
        return self._call("trace_drivers", {
            "session_id": session_id,
            "signal": signal,
            "max_depth": max_depth,
        })

    def auto_debug(self, session_id: str, signal: str,
                   expected: str = None) -> dict:
        """Auto-debug: eval signal + trace driver chain + read driver values."""
        params = {"session_id": session_id, "signal": signal}
        if expected is not None:
            params["expected"] = expected
        return self._call("auto_debug", params)

    def interface_check(self, session_id: str) -> dict:
        """Check ports for width mismatches across the design hierarchy."""
        return self._call("interface_check", {"session_id": session_id})

    def coverage_gap_analysis(self, session_id: str) -> dict:
        """Analyze coverage gaps and suggest stimulus."""
        return self._call("coverage_gap_analysis", {"session_id": session_id})
