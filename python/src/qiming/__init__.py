# Qiming Simulator Python Client
# High-level API for regression automation.

from .client import QimingClient, QimingError, ReadError


class Qiming:
    """High-level interface to the Qiming MCP server.

    Automatically manages session_id across compile/elaborate/simulate calls.
    Usage::

        qm = Qiming("127.0.0.1", 9876)
        qm.connect()
        qm.compile("module counter(...)...")
        qm.elaborate()
        qm.simulate()
        value = qm.eval("count")
        print(value)
        qm.close()
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 9876):
        self._client = QimingClient(host, port)
        self._session_id: str | None = None

    def connect(self):
        self._client.connect()

    def close(self):
        self._client.close()

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()

    # ── Session-aware methods ──

    def compile(self, source: str, name: str = "<inline>") -> dict:
        result = self._client.compile(source, name)
        sid = result.get("session_id")
        if sid:
            self._session_id = sid
        return result

    def elaborate(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.elaborate(self._session_id)

    def simulate(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.simulate(self._session_id)

    def eval(self, signal: str) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.eval(self._session_id, signal)

    def force(self, signal: str, value: str) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.force(self._session_id, signal, value)

    def release(self, signal: str) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.release(self._session_id, signal)

    def query_wave(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.query_wave(self._session_id)

    def get_log(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.get_log(self._session_id)

    def add_breakpoint(self, file: str, line: int) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.add_breakpoint(self._session_id, file, line)

    def remove_breakpoint(self, file: str, line: int) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.remove_breakpoint(self._session_id, file, line)

    def list_breakpoints(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.list_breakpoints(self._session_id)

    def debug_run(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.debug_run(self._session_id)

    def debug_step(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.debug_step(self._session_id)

    def get_coverage(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.get_coverage(self._session_id)

    def get_dependencies(self) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.get_dependencies(self._session_id)

    def export_vcd(self, path: str) -> dict:
        if not self._session_id:
            raise RuntimeError("no active session — call compile() first")
        return self._client.export_vcd(self._session_id, path)

    def get_sessions(self) -> dict:
        return self._client.get_sessions()
