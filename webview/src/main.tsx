import React, { useState, useCallback } from "react";
import { invoke } from "@tauri-apps/api/core";
import ReactDOM from "react-dom/client";
import { WaveformViewer, type WaveEntry } from "./waveform/mod";
import { CodeEditor } from "./editor/mod";

interface Diagnostic {
  severity: string;
  file: string;
  line: number;
  column: number;
  message: string;
}

interface CompileResult {
  success: boolean;
  diagnostics: Diagnostic[];
}

interface SimResult {
  success: boolean;
  ended_at: number;
  final_delta: number;
  stop_reason: string;
}

const DEFAULT_SOURCE = `module counter(input clk, output reg [3:0] count);
  always @(posedge clk) begin
    count <= count + 4'b1;
  end
endmodule
`;

function App(): React.ReactElement {
  const [source, setSource] = useState(DEFAULT_SOURCE);
  const [compileResult, setCompileResult] = useState<CompileResult | null>(null);
  const [simResult, setSimResult] = useState<SimResult | null>(null);
  const [simTime, setSimTime] = useState("100");
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  // Session-based flow state (for waveform viewer)
  const [sessionId, setSessionId] = useState<string | null>(null);
  const [waveEntries, setWaveEntries] = useState<WaveEntry[]>([]);
  const [sessionStatus, setSessionStatus] = useState<string>("");

  // Debug panel state
  const [watchedSignals, setWatchedSignals] = useState<string[]>([]);
  const [signalValues, setSignalValues] = useState<Record<string, string>>({});
  const [bpFile, setBpFile] = useState("");
  const [bpLine, setBpLine] = useState("");
  const [breakpoints, setBreakpoints] = useState<{ file: string; line: number }[]>([]);
  const [logText, setLogText] = useState("");
  const [watchInput, setWatchInput] = useState("");
  const [debugStatus, setDebugStatus] = useState("");

  const handleCompile = useCallback(async () => {
    setLoading(true);
    setError(null);
    setCompileResult(null);
    setSimResult(null);
    try {
      const result = await invoke<CompileResult>("cmd_compile", { source });
      setCompileResult(result);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }, [source]);

  const handleSimulate = useCallback(async () => {
    setLoading(true);
    setError(null);
    setSimResult(null);
    try {
      const until = Number(simTime || "100");
      const result = await invoke<SimResult>("cmd_simulate", { until });
      setSimResult(result);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }, [simTime]);

  // ── Session-based handlers (for waveform viewer) ──

  const handleSessionCompile = useCallback(async () => {
    setLoading(true);
    setError(null);
    setWaveEntries([]);
    setSessionId(null);
    setSessionStatus("compiling...");
    try {
      const result = await invoke<{ session_id: string; success: boolean }>(
        "cmd_session_compile", { source }
      );
      if (result.success) {
        setSessionId(result.session_id);
        // Auto-elaborate after successful compile
        await invoke("cmd_session_elaborate", { sessionId: result.session_id });
        setSessionStatus("compiled & elaborated. Step to generate waveform data.");
      } else {
        setSessionStatus("compile failed");
      }
    } catch (e) {
      setError(String(e));
      setSessionStatus("error");
    } finally {
      setLoading(false);
    }
  }, [source]);

  const handleStepDelta = useCallback(async () => {
    if (!sessionId) return;
    setLoading(true);
    setError(null);
    try {
      await invoke("cmd_session_simulate", { sessionId });
      // Auto-query wave data after step
      const waveResult = await invoke<{ count: number; entries: WaveEntry[] }>(
        "cmd_query_wave", { sessionId }
      );
      setWaveEntries(waveResult.entries);
      setSessionStatus('stepped. ' + waveResult.count + ' wave entries');
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }, [sessionId]);

  const handleQueryWave = useCallback(async () => {
    if (!sessionId) return;
    setLoading(true);
    setError(null);
    try {
      const result = await invoke<{ count: number; entries: WaveEntry[] }>(
        "cmd_query_wave", { sessionId }
      );
      setWaveEntries(result.entries);
      setSessionStatus(`wave data: ${result.count} entries`);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }, [sessionId]);

  // ── Debug panel handlers ──

  const handleAddWatchSignal = useCallback(async () => {
    if (!sessionId || !watchInput.trim()) return;
    const name = watchInput.trim();
    if (watchedSignals.includes(name)) return;
    setWatchedSignals(prev => [...prev, name]);
    setWatchInput("");
    try {
      const result = await invoke<{ signal: string; value: string }>(
        "cmd_eval_signal", { sessionId, signal: name }
      );
      setSignalValues(prev => ({ ...prev, [name]: result.value }));
    } catch (_e) {
      setSignalValues(prev => ({ ...prev, [name]: "?" }));
    }
  }, [sessionId, watchInput, watchedSignals]);

  const handleRemoveWatchSignal = useCallback((name: string) => {
    setWatchedSignals(prev => prev.filter(s => s !== name));
    setSignalValues(prev => {
      const next = { ...prev };
      delete next[name];
      return next;
    });
  }, []);

  const handleRefreshValues = useCallback(async () => {
    if (!sessionId) return;
    const values: Record<string, string> = {};
    for (const name of watchedSignals) {
      try {
        const result = await invoke<{ signal: string; value: string }>(
          "cmd_eval_signal", { sessionId, signal: name }
        );
        values[name] = result.value;
      } catch (_e) {
        values[name] = "?";
      }
    }
    setSignalValues(values);
    setDebugStatus("values refreshed");
  }, [sessionId, watchedSignals]);

  const handleAddBreakpoint = useCallback(async () => {
    if (!sessionId || !bpFile.trim() || !bpLine.trim()) return;
    const line = parseInt(bpLine, 10);
    if (isNaN(line)) return;
    try {
      await invoke("cmd_session_add_breakpoint", {
        sessionId, file: bpFile.trim(), line,
      });
      setBreakpoints(prev => [...prev, { file: bpFile.trim(), line }]);
      setBpFile("");
      setBpLine("");
      setDebugStatus(`breakpoint added at ${bpFile.trim()}:${line}`);
    } catch (e) {
      setDebugStatus(`failed to add breakpoint: ${e}`);
    }
  }, [sessionId, bpFile, bpLine]);

  const handleRemoveBreakpoint = useCallback(async (file: string, line: number) => {
    if (!sessionId) return;
    try {
      await invoke("cmd_session_remove_breakpoint", { sessionId, file, line });
      setBreakpoints(prev => prev.filter(b => b.file !== file || b.line !== line));
      setDebugStatus(`breakpoint removed at ${file}:${line}`);
    } catch (e) {
      setDebugStatus(`failed to remove breakpoint: ${e}`);
    }
  }, [sessionId]);

  const handleContinue = useCallback(async () => {
    if (!sessionId) return;
    setLoading(true);
    setDebugStatus("running...");
    try {
      const result = await invoke<{ breakpoint_hit: boolean }>(
        "cmd_session_debug_run", { sessionId }
      );
      if (result.breakpoint_hit) {
        setDebugStatus("hit breakpoint");
      } else {
        setDebugStatus("simulation completed (no more events)");
      }
      // Refresh watched signal values
      if (watchedSignals.length > 0) {
        const values: Record<string, string> = {};
        for (const name of watchedSignals) {
          try {
            const r = await invoke<{ signal: string; value: string }>(
              "cmd_eval_signal", { sessionId, signal: name }
            );
            values[name] = r.value;
          } catch (_e) {
            values[name] = "?";
          }
        }
        setSignalValues(values);
      }
    } catch (e) {
      setDebugStatus(`error: ${e}`);
    } finally {
      setLoading(false);
    }
  }, [sessionId, watchedSignals]);

  const handleFetchLog = useCallback(async () => {
    if (!sessionId) return;
    try {
      const result = await invoke<{ log: string | null }>(
        "cmd_session_get_log", { sessionId }
      );
      setLogText(result.log ?? "(no log)");
      setDebugStatus("log fetched");
    } catch (e) {
      setDebugStatus(`failed to fetch log: ${e}`);
    }
  }, [sessionId]);

  const handleLoadSignals = useCallback(async () => {
    if (!sessionId) return;
    try {
      const result = await invoke<{ signals: string[] }>(
        "cmd_session_get_signals", { sessionId }
      );
      setDebugStatus(`loaded ${result.signals.length} signals`);
      // Add all signals to watch list
      for (const name of result.signals) {
        if (!watchedSignals.includes(name)) {
          setWatchedSignals(prev => [...prev, name]);
          try {
            const r = await invoke<{ signal: string; value: string }>(
              "cmd_eval_signal", { sessionId, signal: name }
            );
            setSignalValues(prev => ({ ...prev, [name]: r.value }));
          } catch (_e) {
            setSignalValues(prev => ({ ...prev, [name]: "?" }));
          }
        }
      }
    } catch (e) {
      setDebugStatus(`failed to load signals: ${e}`);
    }
  }, [sessionId, watchedSignals]);

  return (
    <div style={{ padding: "16px", fontFamily: "system-ui, sans-serif" }}>
      <h1 style={{ margin: "0 0 12px 0" }}>Qiming Simulator</h1>

      <div style={{ display: "flex", gap: "16px", flexWrap: "wrap" }}>
        {/* ── Source panel ── */}
        <div style={{ flex: "1 1 500px", minWidth: "300px" }}>
          <h3 style={{ margin: "0 0 6px 0" }}>Verilog Source</h3>
          <CodeEditor
            value={source}
            onChange={setSource}
            language="verilog"
            height="300px"
          />
          <div style={{ marginTop: "8px" }}>
            <button onClick={handleCompile} disabled={loading}
              style={{ padding: "8px 24px", fontSize: "14px", cursor: "pointer" }}>
              {loading ? "Working..." : "Compile"}
            </button>
          </div>

          {compileResult && (
            <div style={{ marginTop: "12px" }}>
              <h4 style={{ margin: "0 0 4px 0" }}>
                Compile {compileResult.success ? "OK" : "FAILED"}
              </h4>
              {compileResult.diagnostics.length > 0 ? (
                <table style={{
                  width: "100%", borderCollapse: "collapse",
                  fontFamily: "monospace", fontSize: "12px",
                }}>
                  <thead>
                    <tr style={{ background: "#f5f5f5" }}>
                      <th style={thStyle}>Severity</th>
                      <th style={thStyle}>File</th>
                      <th style={thStyle}>Line</th>
                      <th style={thStyle}>Message</th>
                    </tr>
                  </thead>
                  <tbody>
                    {compileResult.diagnostics.map((d, i) => (
                      <tr key={i} style={{
                        background: d.severity === "error" ? "#fff0f0" : "#fffff0",
                      }}>
                        <td style={tdStyle}>{d.severity}</td>
                        <td style={tdStyle}>{d.file}</td>
                        <td style={tdStyle}>{d.line}</td>
                        <td style={tdStyle}>{d.message}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              ) : (
                <p style={{ margin: 0, fontSize: "13px", color: "#666" }}>No diagnostics</p>
              )}
            </div>
          )}
        </div>

        {/* ── Simulation panel ── */}
        <div style={{ flex: "0 0 320px" }}>
          <h3 style={{ margin: "0 0 6px 0" }}>Simulation</h3>
          <div style={{ display: "flex", alignItems: "center", gap: "8px", marginBottom: "8px" }}>
            <label style={{ fontSize: "13px" }}>Run for:</label>
            <input
              type="number"
              value={simTime}
              onChange={(e) => setSimTime(e.target.value)}
              style={{
                width: "80px", padding: "4px 8px", fontFamily: "monospace",
                border: "1px solid #ccc", borderRadius: "4px",
              }}
            />
            <span style={{ fontSize: "13px", color: "#666" }}>time units</span>
          </div>
          <button onClick={handleSimulate} disabled={loading}
            style={{ padding: "8px 24px", fontSize: "14px", cursor: "pointer" }}>
            {loading ? "Working..." : "Simulate"}
          </button>

          {simResult && (
            <div style={{
              marginTop: "12px", padding: "12px", borderRadius: "4px",
              background: simResult.success ? "#f0fff0" : "#fff0f0",
              border: "1px solid #ccc", fontFamily: "monospace", fontSize: "13px",
            }}>
              <div><strong>Status:</strong> {simResult.success ? "OK" : "FAILED"}</div>
              <div><strong>Ended at:</strong> {simResult.ended_at} time units</div>
              <div><strong>Final delta:</strong> {simResult.final_delta}</div>
              <div><strong>Stop reason:</strong> {simResult.stop_reason}</div>
            </div>
          )}
        </div>
      </div>

      {error && (
        <div style={{
          marginTop: "12px", padding: "10px", borderRadius: "4px",
          background: "#fff0f0", border: "1px solid #fcc", fontFamily: "monospace", fontSize: "13px",
        }}>
          <strong>Error:</strong> {error}
        </div>
      )}

      {/* ── Waveform Viewer section ── */}
      <div style={{ marginTop: "20px", borderTop: "1px solid #ddd", paddingTop: "16px" }}>
        <h3 style={{ margin: "0 0 8px 0" }}>Waveform Viewer (Session-based)</h3>
        <div style={{ display: "flex", gap: "8px", alignItems: "center", marginBottom: "8px" }}>
          <button onClick={handleSessionCompile} disabled={loading}
            style={wfBtnStyle}>
            Compile & Elaborate
          </button>
          <button onClick={handleStepDelta} disabled={loading || !sessionId}
            style={wfBtnStyle}>
            Step Delta
          </button>
          <button onClick={handleQueryWave} disabled={loading || !sessionId}
            style={wfBtnStyle}>
            Query Wave
          </button>
          {sessionStatus && (
            <span style={{ fontSize: "12px", color: "#666" }}>{sessionStatus}</span>
          )}
        </div>

        {waveEntries.length > 0 && (
          <WaveformViewer entries={waveEntries} />
        )}
      </div>

      {/* ── Debug Panel section ── */}
      <div style={{ marginTop: "20px", borderTop: "1px solid #ddd", paddingTop: "16px" }}>
        <h3 style={{ margin: "0 0 8px 0" }}>Debug Panel</h3>

        {/* Controls */}
        <div style={{ display: "flex", gap: "8px", alignItems: "center", marginBottom: "12px", flexWrap: "wrap" }}>
          <button onClick={async () => {
            if (!sessionId) return;
            setLoading(true);
            try {
              await invoke("cmd_session_simulate", { sessionId });
              setDebugStatus("stepped");
              // Refresh watched values
              if (watchedSignals.length > 0) {
                const values: Record<string, string> = {};
                for (const name of watchedSignals) {
                  try {
                    const r = await invoke<{ signal: string; value: string }>(
                      "cmd_eval_signal", { sessionId, signal: name }
                    );
                    values[name] = r.value;
                  } catch { values[name] = "?"; }
                }
                setSignalValues(values);
              }
            } catch (e) { setDebugStatus(`error: ${e}`); }
            finally { setLoading(false); }
          }} disabled={loading || !sessionId} style={dbBtnStyle}>
            Step Delta
          </button>
          <button onClick={handleContinue} disabled={loading || !sessionId} style={dbBtnStyle}>
            Continue
          </button>
          <button onClick={handleRefreshValues} disabled={!sessionId} style={dbBtnStyle}>
            Refresh Values
          </button>
          <button onClick={handleFetchLog} disabled={!sessionId} style={dbBtnStyle}>
            Fetch Log
          </button>
          <button onClick={handleLoadSignals} disabled={loading || !sessionId} style={dbBtnStyle}>
            Load Signals
          </button>
          {debugStatus && (
            <span style={{ fontSize: "12px", color: "#666" }}>{debugStatus}</span>
          )}
        </div>

        <div style={{ display: "flex", gap: "16px", flexWrap: "wrap" }}>
          {/* Watch Signals */}
          <div style={{ flex: "1 1 300px", minWidth: "200px" }}>
            <h4 style={{ margin: "0 0 6px 0", fontSize: "13px" }}>Signal Watch</h4>
            <div style={{ display: "flex", gap: "4px", marginBottom: "6px" }}>
              <input
                value={watchInput}
                onChange={e => setWatchInput(e.target.value)}
                placeholder="signal name"
                style={dbInputStyle}
                onKeyDown={e => e.key === "Enter" && handleAddWatchSignal()}
              />
              <button onClick={handleAddWatchSignal} disabled={!sessionId} style={dbBtnStyle}>
                Watch
              </button>
            </div>
            <div style={{ maxHeight: "200px", overflowY: "auto", fontFamily: "monospace", fontSize: "12px" }}>
              {watchedSignals.length === 0 && (
                <div style={{ color: "#999", padding: "4px 0" }}>No signals watched</div>
              )}
              {watchedSignals.map(name => (
                <div key={name} style={{
                  display: "flex", justifyContent: "space-between", alignItems: "center",
                  padding: "2px 4px", borderBottom: "1px solid #eee",
                }}>
                  <span>{name}</span>
                  <span style={{ fontWeight: "bold", color: "#069" }}>
                    {signalValues[name] ?? "?"}
                  </span>
                  <button onClick={() => handleRemoveWatchSignal(name)}
                    style={{ ...dbBtnStyle, padding: "1px 6px", fontSize: "11px" }}>
                    rm
                  </button>
                </div>
              ))}
            </div>
          </div>

          {/* Breakpoints */}
          <div style={{ flex: "1 1 300px", minWidth: "200px" }}>
            <h4 style={{ margin: "0 0 6px 0", fontSize: "13px" }}>Breakpoints</h4>
            <div style={{ display: "flex", gap: "4px", marginBottom: "6px" }}>
              <input
                value={bpFile}
                onChange={e => setBpFile(e.target.value)}
                placeholder="file name"
                style={{ ...dbInputStyle, width: "120px" }}
              />
              <input
                value={bpLine}
                onChange={e => setBpLine(e.target.value)}
                placeholder="line"
                type="number"
                style={{ ...dbInputStyle, width: "60px" }}
                onKeyDown={e => e.key === "Enter" && handleAddBreakpoint()}
              />
              <button onClick={handleAddBreakpoint} disabled={!sessionId} style={dbBtnStyle}>
                Add
              </button>
            </div>
            <div style={{ maxHeight: "200px", overflowY: "auto", fontFamily: "monospace", fontSize: "12px" }}>
              {breakpoints.length === 0 && (
                <div style={{ color: "#999", padding: "4px 0" }}>No breakpoints set</div>
              )}
              {breakpoints.map((bp, i) => (
                <div key={i} style={{
                  display: "flex", justifyContent: "space-between", alignItems: "center",
                  padding: "2px 4px", borderBottom: "1px solid #eee",
                }}>
                  <span>{bp.file}:{bp.line}</span>
                  <button onClick={() => handleRemoveBreakpoint(bp.file, bp.line)}
                    style={{ ...dbBtnStyle, padding: "1px 6px", fontSize: "11px" }}>
                    rm
                  </button>
                </div>
              ))}
            </div>
          </div>
        </div>

        {/* Log output */}
        <div style={{ marginTop: "12px" }}>
          <h4 style={{ margin: "0 0 6px 0", fontSize: "13px" }}>Log Output</h4>
          <pre style={{
            margin: 0, padding: "8px", fontSize: "11px", fontFamily: "monospace",
            background: "#fafafa", border: "1px solid #ccc", borderRadius: "4px",
            maxHeight: "150px", overflowY: "auto", whiteSpace: "pre-wrap",
          }}>
            {logText || "(no log)"}
          </pre>
        </div>
      </div>
    </div>
  );
}

const thStyle: React.CSSProperties = {
  border: "1px solid #ccc", padding: "4px 8px", textAlign: "left",
};
const tdStyle: React.CSSProperties = {
  border: "1px solid #ccc", padding: "4px 8px",
};
const wfBtnStyle: React.CSSProperties = {
  padding: "6px 16px", fontSize: "12px", cursor: "pointer",
  border: "1px solid #999", borderRadius: "3px", background: "#fff",
};
const dbBtnStyle: React.CSSProperties = {
  padding: "4px 12px", fontSize: "12px", cursor: "pointer",
  border: "1px solid #999", borderRadius: "3px", background: "#fff",
};
const dbInputStyle: React.CSSProperties = {
  padding: "4px 8px", fontSize: "12px", fontFamily: "monospace",
  border: "1px solid #ccc", borderRadius: "3px",
};

const root = document.getElementById("root");
if (root) {
  ReactDOM.createRoot(root).render(<App />);
}
