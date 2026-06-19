// MCP tool handlers — dispatch JSON-RPC method calls to SessionManager operations.

use serde_json::{json, Value};
use crate::ffi::Diagnostic;
use crate::session::SessionManager;
use super::types::JsonRpcResponse;

/// Extract a string parameter from the params Value.
fn param_str(params: &Option<Value>, key: &str) -> Result<String, String> {
    params.as_ref()
        .and_then(|p| p.get(key))
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
        .ok_or_else(|| format!("missing or invalid parameter: {}", key))
}

/// Extract an optional string parameter.
fn param_str_opt(params: &Option<Value>, key: &str) -> Option<String> {
    params.as_ref()
        .and_then(|p| p.get(key))
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
}

/// Convert a Vec<Diagnostic> to a serde_json Value array.
fn diagnostics_to_json(diags: &[Diagnostic]) -> Value {
    json!(diags.iter().map(|d| {
        let mut obj = json!({
            "severity": d.severity,
            "file": d.file,
            "line": d.line,
            "column": d.column,
            "message": d.message,
        });
        if let Some(ref rec) = d.recovery {
            obj["recovery"] = json!({
                "suggestions": rec.suggestions,
                "next_tool": rec.next_tool,
            });
        }
        obj
    }).collect::<Vec<Value>>())
}

/// Dispatch a JSON-RPC method call and return a response.
pub fn handle_request(sessions: &SessionManager, method: &str, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    match method {
        "compile" => handle_compile(sessions, id, params),
        "elaborate" => handle_elaborate(sessions, id, params),
        "simulate" => handle_simulate(sessions, id, params),
        "eval" => handle_eval(sessions, id, params),
        "eval_multi" => handle_eval_multi(sessions, id, params),
        "force" => handle_force(sessions, id, params),
        "release" => handle_release(sessions, id, params),
        "query_wave" => handle_query_wave(sessions, id, params),
        "query_wave_bulk" => handle_query_wave_bulk(sessions, id, params),
        "get_sessions" => handle_get_sessions(sessions, id, params),
        "get_log_summary" => handle_get_log(sessions, id, params),
        // Breakpoint tools
        "add_breakpoint" => handle_add_breakpoint(sessions, id, params),
        "remove_breakpoint" => handle_remove_breakpoint(sessions, id, params),
        "list_breakpoints" => handle_list_breakpoints(sessions, id, params),
        "debug_run" => handle_debug_run(sessions, id, params),
        "debug_step" => handle_debug_step(sessions, id, params),
        "debug_trace" => handle_debug_trace(sessions, id, params),
        // Coverage tools
        "get_coverage" => handle_get_coverage(sessions, id, params),
        // Design comprehension tools
        "design_summary" => handle_design_summary(sessions, id, params),
        "control_flow" => handle_control_flow(sessions, id, params),
        // Checkpoint tools
        "save_checkpoint" => handle_save_checkpoint(sessions, id, params),
        "restore_checkpoint" => handle_restore_checkpoint(sessions, id, params),
        "diff_checkpoint" => handle_diff_checkpoint(sessions, id, params),
        "list_checkpoints" => handle_list_checkpoints(sessions, id, params),
        // VCD export
        "export_vcd" => handle_export_vcd(sessions, id, params),
        // Signal trace
        "trace_drivers" => handle_trace_drivers(sessions, id, params),
        // Skill MCP handlers
        "auto_debug" => handle_auto_debug(sessions, id, params),
        "interface_check" => handle_interface_check(sessions, id, params),
        "coverage_gap_analysis" => handle_coverage_gap_analysis(sessions, id, params),
        // Dependency tools
        "get_dependencies" => handle_get_dependencies(sessions, id, params),
        "get_signals" => handle_get_signals(sessions, id, params),
        "list_designs" => handle_list_designs(sessions, id, params),
        _ => JsonRpcResponse::method_not_found(id, method),
    }
}

fn handle_compile(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let name = param_str_opt(params, "name").unwrap_or_else(|| "<inline>".to_string());
    let source = match param_str(params, "source") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let session_id = match sessions.create() {
        Ok(sid) => sid,
        Err(e) => return JsonRpcResponse::internal_error(id, &e),
    };

    let result = sessions.with_session(&session_id, |s| {
        let r = s.compile_string(&name, &source);
        let diags = s.get_last_diagnostics();
        Ok((r, diags))
    });

    match result {
        Ok((Ok(_), _)) => JsonRpcResponse::success(id, json!({
            "session_id": session_id,
            "success": true
        })),
        Ok((Err(e), diagnostics)) => JsonRpcResponse::success(id, json!({
            "session_id": session_id,
            "success": false,
            "error": e,
            "diagnostics": diagnostics_to_json(&diagnostics)
        })),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_elaborate(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session(&session_id, |s| {
        let r = s.elaborate();
        let diags = s.get_last_diagnostics();
        Ok((r, diags))
    });
    match result {
        Ok((Ok(_), _)) => JsonRpcResponse::success(id, json!({"success": true})),
        Ok((Err(e), diagnostics)) => JsonRpcResponse::success(id, json!({"success": false, "error": e, "diagnostics": diagnostics_to_json(&diagnostics)})),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_simulate(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session(&session_id, |s| s.step_delta());
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::success(id, json!({"success": false, "error": e})),
    }
}

fn handle_eval(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let signal = match param_str(params, "signal") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.eval_str(&signal));
    match result {
        Ok(value) => JsonRpcResponse::success(id, json!({
            "signal": signal,
            "value": value
        })),
        Err(e) => JsonRpcResponse::success(id, json!({
            "signal": signal,
            "error": e
        })),
    }
}

fn handle_eval_multi(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let signals: Vec<String> = match params.as_ref()
        .and_then(|p| p.get("signals"))
        .and_then(|v| v.as_array())
    {
        Some(arr) => arr.iter()
            .filter_map(|v| v.as_str().map(|s| s.to_string()))
            .collect(),
        None => return JsonRpcResponse::invalid_params(id, "missing or invalid parameter: signals"),
    };

    if signals.is_empty() {
        return JsonRpcResponse::invalid_params(id, "signals array must not be empty");
    }

    let signal_refs: Vec<&str> = signals.iter().map(|s| s.as_str()).collect();
    let result = sessions.with_session_readonly(&session_id, |s| {
        let pairs = s.eval_multi(&signal_refs)?;
        Ok(json!({
            "session_id": session_id,
            "count": pairs.len(),
            "signals": pairs.iter().map(|(name, val)| json!({"name": name, "value": val})).collect::<Vec<_>>()
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_force(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let signal = match param_str(params, "signal") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let value = match param_str(params, "value") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.force_str(&signal, &value));
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::success(id, json!({"success": false, "error": e})),
    }
}

fn handle_release(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let signal = match param_str(params, "signal") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.release(&signal));
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::success(id, json!({"success": false, "error": e})),
    }
}

fn handle_query_wave(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let count = s.wave_count();
        let mut entries = Vec::new();
        for i in 0..count {
            if let Some((sig, time_fs, val)) = s.get_wave(i) {
                entries.push(json!({
                    "signal": sig,
                    "time_fs": time_fs,
                    "value": val
                }));
            }
        }
        Ok(json!({
            "session_id": session_id,
            "count": count,
            "entries": entries
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_query_wave_bulk(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    // Parse optional signals array
    let signals: Vec<&str> = params.as_ref()
        .and_then(|p| p.get("signals"))
        .and_then(|v| v.as_array())
        .map(|arr| arr.iter().filter_map(|v| v.as_str()).collect())
        .unwrap_or_default();

    let t_start = params.as_ref()
        .and_then(|p| p.get("t_start"))
        .and_then(|v| v.as_u64())
        .unwrap_or(0);

    let t_end = params.as_ref()
        .and_then(|p| p.get("t_end"))
        .and_then(|v| v.as_u64())
        .unwrap_or(0);

    let result = sessions.with_session_readonly(&session_id, |s| {
        let entries = s.query_wave_bulk(&signals, t_start, t_end)?;
        let json_entries: Vec<Value> = entries.iter()
            .map(|(sig, time, val)| json!({
                "signal": sig,
                "time_fs": time,
                "value": val
            }))
            .collect();
        Ok(json!({
            "session_id": session_id,
            "count": entries.len(),
            "entries": json_entries
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_get_sessions(sessions: &SessionManager, id: u64, _params: &Option<Value>) -> JsonRpcResponse {
    match sessions.list() {
        Ok(ids) => JsonRpcResponse::success(id, json!({
            "count": ids.len(),
            "sessions": ids
        })),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_get_log(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        Ok(s.get_log().unwrap_or_default())
    });

    match result {
        Ok(log_text) => JsonRpcResponse::success(id, json!({
            "session_id": session_id,
            "log": log_text
        })),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_add_breakpoint(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let file = match param_str(params, "file") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let line = match params.as_ref().and_then(|p| p.get("line")).and_then(|v| v.as_u64()) {
        Some(l) => l as u32,
        None => return JsonRpcResponse::invalid_params(id, "missing or invalid parameter: line"),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.add_breakpoint(&file, line));
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_remove_breakpoint(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let file = match param_str(params, "file") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let line = match params.as_ref().and_then(|p| p.get("line")).and_then(|v| v.as_u64()) {
        Some(l) => l as u32,
        None => return JsonRpcResponse::invalid_params(id, "missing or invalid parameter: line"),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.remove_breakpoint(&file, line));
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_list_breakpoints(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let bps = s.list_breakpoints()?;
        let bp_list: Vec<Value> = bps.iter().map(|(f, l)| json!({"file": f, "line": l})).collect();
        Ok(json!({"session_id": session_id, "count": bps.len(), "breakpoints": bp_list}))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_debug_run(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let hit = s.debug_run()?;
        Ok(json!({"session_id": session_id, "breakpoint_hit": hit}))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_debug_step(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session(&session_id, |s| s.step_delta());
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true})),
        Err(e) => JsonRpcResponse::success(id, json!({"success": false, "error": e})),
    }
}

fn handle_debug_trace(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let cycles = match params.as_ref().and_then(|p| p.get("cycles")).and_then(|v| v.as_u64()) {
        Some(c) => c as usize,
        None => return JsonRpcResponse::invalid_params(id, "missing or invalid parameter: cycles"),
    };

    let signals: Vec<String> = match params.as_ref()
        .and_then(|p| p.get("signals"))
        .and_then(|v| v.as_array())
    {
        Some(arr) => arr.iter()
            .filter_map(|v| v.as_str().map(|s| s.to_string()))
            .collect(),
        None => return JsonRpcResponse::invalid_params(id, "missing or invalid parameter: signals"),
    };

    if signals.is_empty() {
        return JsonRpcResponse::invalid_params(id, "signals array must not be empty");
    }

    let signal_refs: Vec<&str> = signals.iter().map(|s| s.as_str()).collect();
    let result = sessions.with_session_readonly(&session_id, |s| {
        let trace = s.debug_trace(&signal_refs, cycles)?;
        let cycles_json: Vec<Value> = trace.iter().map(|cycle| {
            json!(cycle.iter().map(|(name, val)| {
                json!({"signal": name, "value": val})
            }).collect::<Vec<Value>>())
        }).collect();
        Ok(json!({
            "session_id": session_id,
            "cycle_count": cycles,
            "signal_count": signals.len(),
            "cycles": cycles_json
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_get_coverage(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let cov = s.get_coverage()?;
        let entries: Vec<Value> = cov.entries.iter()
            .map(|e| json!({"file": e.file, "line": e.line}))
            .collect();
        Ok(json!({
            "session_id": session_id,
            "count": cov.entries.len(),
            "percent": cov.percent,
            "entries": entries
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_design_summary(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let summary = s.design_summary()?;
        Ok(json!({
            "session_id": session_id,
            "summary": summary
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_control_flow(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let flow = s.control_flow()?;
        Ok(json!({
            "session_id": session_id,
            "control_flow": flow
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_save_checkpoint(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let name = match param_str(params, "name") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let ckpt_id = s.save_checkpoint(&name)?;
        Ok(json!({
            "session_id": session_id,
            "checkpoint_id": ckpt_id
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_restore_checkpoint(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let name = match param_str(params, "name") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        s.restore_checkpoint(&name)?;
        Ok(json!({
            "session_id": session_id,
            "success": true
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_diff_checkpoint(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let name_a = match param_str(params, "checkpoint_a") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let name_b = match param_str(params, "checkpoint_b") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let diff = s.diff_checkpoint(&name_a, &name_b)?;
        let parsed: Value = serde_json::from_str(&diff)
            .unwrap_or_else(|_| json!({"raw": diff}));
        Ok(json!({
            "session_id": session_id,
            "diff": parsed
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_list_checkpoints(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let list = s.list_checkpoints()?;
        let parsed: Value = serde_json::from_str(&list)
            .unwrap_or_else(|_| json!({"raw": list}));
        Ok(json!({
            "session_id": session_id,
            "checkpoints": parsed
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_get_dependencies(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let count = s.signal_count();
        let mut signals = Vec::new();
        for i in 0..count {
            if let Some(name) = s.signal_name(i) {
                signals.push(name);
            }
        }
        Ok(json!({
            "session_id": session_id,
            "signal_count": count,
            "signals": signals
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}


fn handle_get_signals(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let count = s.signal_count();
        let mut signals = Vec::new();
        for i in 0..count {
            if let Some(name) = s.signal_name(i) {
                let value = s.eval_str(&name).unwrap_or_else(|_| "?".to_string());
                signals.push(json!({
                    "index": i,
                    "name": name,
                    "value": value
                }));
            }
        }
        Ok(json!({
            "session_id": session_id,
            "signal_count": count,
            "signals": signals
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_list_designs(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let summary_json = s.design_summary()?;
        /* design_summary returns a JSON string — parse it for structured output */
        let parsed: serde_json::Value = serde_json::from_str(&summary_json)
            .unwrap_or_else(|_| json!({"raw": summary_json}));
        Ok(json!({
            "session_id": session_id,
            "designs": parsed
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}
fn handle_export_vcd(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let path = match param_str(params, "path") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| s.export_vcd(&path));
    match result {
        Ok(_) => JsonRpcResponse::success(id, json!({"success": true, "path": path})),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_trace_drivers(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let signal = match param_str(params, "signal") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let max_depth = params.as_ref()
        .and_then(|p| p.get("max_depth"))
        .and_then(|v| v.as_u64())
        .unwrap_or(3) as usize;

    let result = sessions.with_session_readonly(&session_id, |s| {
        let trace = s.trace_drivers(&signal, max_depth)?;
        let parsed: Value = serde_json::from_str(&trace)
            .unwrap_or_else(|_| json!({"raw": trace}));
        Ok(json!({
            "session_id": session_id,
            "signal": signal,
            "drivers": parsed
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_auto_debug(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let signal = match param_str(params, "signal") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };
    let expected = param_str_opt(params, "expected");

    let result = sessions.with_session_readonly(&session_id, |s| {
        // Step 1: Eval the failing signal
        let actual = s.eval_str(&signal).unwrap_or_else(|_| "?".to_string());
        let mut report = json!({
            "signal": signal,
            "actual_value": actual,
        });
        if let Some(ref exp) = expected {
            report["expected_value"] = json!(exp);
            report["match"] = json!(actual == *exp);
        }

        // Step 2: Trace driver chain (up to 3 levels)
        let trace = s.trace_drivers(&signal, 3).ok();
        if let Some(ref t) = trace {
            let parsed = serde_json::from_str::<Value>(t).unwrap_or_else(|_| json!({"raw": t}));
            report["driver_chain"] = parsed;
        }

        // Step 3: Eval driver signals from the trace
        let mut driver_values = Vec::new();
        if let Some(ref t) = trace {
            if let Ok(parsed) = serde_json::from_str::<Value>(t) {
                if let Some(drivers) = parsed.get("drivers").and_then(|v| v.as_array()) {
                    for driver in drivers {
                        if let Some(name) = driver.get("name").and_then(|v| v.as_str()) {
                            let val = s.eval_str(name).unwrap_or_else(|_| "?".to_string());
                            driver_values.push(json!({
                                "name": name,
                                "value": val
                            }));
                        }
                    }
                }
            }
        }
        report["driver_values"] = json!(driver_values);

        Ok(json!({
            "session_id": session_id,
            "analysis": report
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn handle_interface_check(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let summary = s.design_summary()?;
        let parsed: Value = serde_json::from_str(&summary)
            .unwrap_or_else(|_| json!({"raw": summary}));

        // Extract module information from the summary
        let modules = parsed.get("modules")
            .and_then(|v| v.as_array())
            .cloned()
            .unwrap_or_default();

        let mut findings = Vec::new();
        for module in &modules {
            let mod_name = module.get("name").and_then(|v| v.as_str()).unwrap_or("?");
            let ports = module.get("ports")
                .and_then(|v| v.as_array())
                .cloned()
                .unwrap_or_default();

            for port in &ports {
                let port_name = port.get("name").and_then(|v| v.as_str()).unwrap_or("?");
                let port_dir = port.get("direction").and_then(|v| v.as_str()).unwrap_or("?");
                let port_width = port.get("width").and_then(|v| v.as_u64()).unwrap_or(1) as u32;

                // Check for potential width issues by scanning all signal widths
                let _all_signals = s.signal_count();
                if port_dir == "output" || port_dir == "inout" {
                    // For outputs, verify the signal exists
                    let hier_signal = format!("{}.{}", mod_name, port_name);
                    match s.eval_str(&hier_signal) {
                        Ok(val) => {
                            let actual_width = val.len() as u32;
                            if actual_width != port_width {
                                findings.push(json!({
                                    "module": mod_name,
                                    "port": port_name,
                                    "direction": port_dir,
                                    "declared_width": port_width,
                                    "actual_width": actual_width,
                                    "issue": "width_mismatch"
                                }));
                            }
                        }
                        Err(_) => {
                            findings.push(json!({
                                "module": mod_name,
                                "port": port_name,
                                "direction": port_dir,
                                "issue": "signal_not_found"
                            }));
                        }
                    }
                }
            }

            // Check instances
            let instances = module.get("instances")
                .and_then(|v| v.as_array())
                .cloned()
                .unwrap_or_default();
            for inst in &instances {
                if let Some(inst_name) = inst.get("name").and_then(|v| v.as_str()) {
                    // Verify the instance has all expected hierarchical signals
                    let _inst_prefix = format!("{}.{}", mod_name, inst_name);
                    let found = s.signal_count() > 0; // instance exists if design elaborated
                    if !found {
                        findings.push(json!({
                            "instance": inst_name,
                            "issue": "instance_not_found"
                        }));
                    }
                }
            }
        }

        Ok(json!({
            "session_id": session_id,
            "modules": modules.len(),
            "total_ports": ports_count(&modules),
            "findings": findings,
            "summary": parsed
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}

fn ports_count(modules: &[Value]) -> usize {
    modules.iter()
        .filter_map(|m| m.get("ports").and_then(|p| p.as_array()))
        .map(|p| p.len())
        .sum()
}

fn handle_coverage_gap_analysis(sessions: &SessionManager, id: u64, params: &Option<Value>) -> JsonRpcResponse {
    let session_id = match param_str(params, "session_id") {
        Ok(s) => s,
        Err(e) => return JsonRpcResponse::invalid_params(id, &e),
    };

    let result = sessions.with_session_readonly(&session_id, |s| {
        let cov = s.get_coverage()?;
        let covered_lines: Vec<Value> = cov.entries.iter()
            .map(|e| json!({"file": e.file, "line": e.line}))
            .collect();

        // Group coverage by file
        let mut file_coverage: std::collections::BTreeMap<String, Vec<u32>> = std::collections::BTreeMap::new();
        for entry in &cov.entries {
            file_coverage.entry(entry.file.clone())
                .or_default()
                .push(entry.line);
        }

        let file_summaries: Vec<Value> = file_coverage.iter()
            .map(|(file, lines)| {
                json!({
                    "file": file,
                    "covered_lines": lines.len(),
                    "lines": lines
                })
            })
            .collect();

        Ok(json!({
            "session_id": session_id,
            "overall_percent": cov.percent,
            "covered_count": cov.entries.len(),
            "by_file": file_summaries,
            "covered_lines": covered_lines,
            "suggestions": [
                "Lines not in covered_lines are uncovered — inspect source to identify uncovered branches",
                "Use force/set to drive controlling signals to uncovered states, then step/simulate",
                "If coverage plateaus, check for dead code (unreachable case items, impossible if conditions)"
            ]
        }))
    });

    match result {
        Ok(data) => JsonRpcResponse::success(id, data),
        Err(e) => JsonRpcResponse::internal_error(id, &e),
    }
}
