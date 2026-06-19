// Qiming simulator — Tauri desktop application entry point

use std::sync::{Arc, Mutex};
use qiming_lib::{ffi, session::SessionManager, python::PythonManager};

// ── Session pointer (Send+Sync wrapper around raw C pointer) ──

#[derive(Clone, Copy)]
struct SessionPtr(*mut std::ffi::c_void);
unsafe impl Send for SessionPtr {}
unsafe impl Sync for SessionPtr {}

impl SessionPtr {
    fn null() -> Self { SessionPtr(std::ptr::null_mut()) }
    fn is_null(&self) -> bool { self.0.is_null() }
    fn as_ptr(&self) -> *mut std::ffi::c_void { self.0 }
    fn set(&mut self, ptr: *mut std::ffi::c_void) { self.0 = ptr; }
}

// ── Application state ──

struct AppState {
    session: Mutex<SessionPtr>,
    sessions: qiming_lib::session::SessionManager,
}

// ── Tauri commands (old compile/simulate API) ──

#[tauri::command]
fn cmd_compile(
    state: tauri::State<'_, AppState>,
    source: String,
) -> Result<ffi::CompileResult, String> {
    // Free previous session
    {
        let mut session = state.session.lock().unwrap();
        if !session.is_null() {
            unsafe { ffi::compile_result_free_raw(session.as_ptr() as *mut _); }
            session.set(std::ptr::null_mut());
        }
    }

    let raw = unsafe { ffi::compile_raw(&source)? };
    let (success, diagnostics) = unsafe { ffi::read_compile_diagnostics(raw) };

    {
        let mut session = state.session.lock().unwrap();
        session.set(raw as *mut std::ffi::c_void);
    }

    Ok(ffi::CompileResult { success, diagnostics })
}

#[tauri::command]
fn cmd_simulate(
    state: tauri::State<'_, AppState>,
    until: u64,
) -> Result<ffi::SimResult, String> {
    let session = {
        let s = state.session.lock().unwrap();
        if s.is_null() {
            return Err("no compiled design — run compile first".to_string());
        }
        s.as_ptr()
    };

    let raw = unsafe { ffi::sim_run_raw(session, until)? };
    let result = unsafe { ffi::read_sim_result(raw) };
    unsafe { ffi::sim_result_free_raw(raw); }
    Ok(result)
}

#[tauri::command]
fn cmd_shutdown(state: tauri::State<'_, AppState>) -> Result<(), String> {
    {
        let mut session = state.session.lock().unwrap();
        if !session.is_null() {
            unsafe { ffi::compile_result_free_raw(session.as_ptr() as *mut _); }
            session.set(std::ptr::null_mut());
        }
    }
    ffi::shutdown();
    Ok(())
}

#[tauri::command]
fn cmd_get_units(state: tauri::State<'_, AppState>) -> Result<Vec<String>, String> {
    let session = {
        let s = state.session.lock().unwrap();
        if s.is_null() {
            return Err("no compiled design".to_string());
        }
        s.as_ptr()
    };

    let res = unsafe { &*(session as *const ffi::QsimCompileResult) };
    let mut units = Vec::new();
    for i in 0..res.unit_count {
        units.push(format!("design_unit_{}", i));
    }
    Ok(units)
}

// ── Session-based Tauri commands (used by waveform viewer) ──

#[tauri::command]
fn cmd_session_compile(
    state: tauri::State<'_, AppState>,
    source: String,
    name: Option<String>,
) -> Result<serde_json::Value, String> {
    let session_id = state.sessions.create()?;
    let name = name.unwrap_or_else(|| "<inline>".to_string());

    state.sessions.with_session(&session_id, |s| {
        s.compile_string(&name, &source)
    })?;

    Ok(serde_json::json!({
        "session_id": session_id,
        "success": true
    }))
}

#[tauri::command]
fn cmd_session_elaborate(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<serde_json::Value, String> {
    state.sessions.with_session(&session_id, |s| s.elaborate())?;
    Ok(serde_json::json!({"success": true}))
}

#[tauri::command]
fn cmd_session_simulate(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<serde_json::Value, String> {
    state.sessions.with_session(&session_id, |s| s.step_delta())?;
    Ok(serde_json::json!({"success": true}))
}

#[derive(serde::Serialize)]
struct QueryWaveResponse {
    count: usize,
    entries: Vec<qiming_lib::session::WaveEntry>,
}

#[tauri::command]
fn cmd_query_wave(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<QueryWaveResponse, String> {
    let entries = state.sessions.with_session_readonly(&session_id, |s| {
        Ok::<_, String>(s.query_wave())
    })?;
    let count = entries.len();
    Ok(QueryWaveResponse { count, entries })
}

#[tauri::command]
fn cmd_eval_signal(
    state: tauri::State<'_, AppState>,
    session_id: String,
    signal: String,
) -> Result<serde_json::Value, String> {
    let value = state.sessions.with_session_readonly(&session_id, |s| {
        s.eval_str(&signal)
    })?;
    Ok(serde_json::json!({"signal": signal, "value": value}))
}

// ── Debug panel commands ──

#[tauri::command]
fn cmd_session_add_breakpoint(
    state: tauri::State<'_, AppState>,
    session_id: String,
    file: String,
    line: u32,
) -> Result<serde_json::Value, String> {
    state.sessions.with_session_readonly(&session_id, |s| {
        s.add_breakpoint(&file, line)
    })?;
    Ok(serde_json::json!({"success": true}))
}

#[tauri::command]
fn cmd_session_remove_breakpoint(
    state: tauri::State<'_, AppState>,
    session_id: String,
    file: String,
    line: u32,
) -> Result<serde_json::Value, String> {
    state.sessions.with_session_readonly(&session_id, |s| {
        s.remove_breakpoint(&file, line)
    })?;
    Ok(serde_json::json!({"success": true}))
}

#[derive(serde::Serialize)]
struct ListBreakpointsResponse {
    breakpoints: Vec<(String, u32)>,
}

#[tauri::command]
fn cmd_session_list_breakpoints(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<ListBreakpointsResponse, String> {
    let bps = state.sessions.with_session_readonly(&session_id, |s| {
        s.list_breakpoints()
    })?;
    Ok(ListBreakpointsResponse { breakpoints: bps })
}

#[tauri::command]
fn cmd_session_debug_run(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<serde_json::Value, String> {
    let hit = state.sessions.with_session_readonly(&session_id, |s| {
        s.debug_run()
    })?;
    Ok(serde_json::json!({"breakpoint_hit": hit}))
}

#[derive(serde::Serialize)]
struct GetLogResponse {
    log: Option<String>,
}

#[tauri::command]
fn cmd_session_get_log(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<GetLogResponse, String> {
    let log = state.sessions.with_session_readonly(&session_id, |s| {
        Ok::<_, String>(s.get_log())
    })?;
    Ok(GetLogResponse { log })
}

#[derive(serde::Serialize)]
struct GetSignalsResponse {
    signals: Vec<String>,
}

#[tauri::command]
fn cmd_session_get_signals(
    state: tauri::State<'_, AppState>,
    session_id: String,
) -> Result<GetSignalsResponse, String> {
    let signals = state.sessions.with_session_readonly(&session_id, |s| {
        let count = s.signal_count();
        let mut names = Vec::with_capacity(count as usize);
        for i in 0..count {
            if let Some(name) = s.signal_name(i) {
                names.push(name);
            }
        }
        Ok::<_, String>(names)
    })?;
    Ok(GetSignalsResponse { signals })
}

// ── Application entry ──

/// Run the MCP server on stdin/stdout (non-GUI mode).
fn run_mcp()
{
    ffi::init();
    let sessions = Arc::new(SessionManager::new());
    eprintln!("MCP server started on stdin/stdout");
    qiming_lib::mcp::server::run(&sessions);
    ffi::shutdown();
}

/// Run the MCP server on a TCP socket (non-GUI mode).
fn run_tcp(addr: &str)
{
    ffi::init();
    let sessions = Arc::new(SessionManager::new());
    eprintln!("MCP TCP server starting on {}", addr);
    if let Err(e) = qiming_lib::mcp::server::run_tcp(addr, sessions) {
        eprintln!("MCP TCP error: {}", e);
    }
    ffi::shutdown();
}

/// Run a Python script via the TCP MCP server.
/// Starts the server on port 9876, spawns the Python script with
/// QIMING_MCP_ADDR=127.0.0.1:9876, and exits when the script finishes.
fn run_tcp_with_python(script: &str)
{
    let addr = "127.0.0.1:9876";

    ffi::init();
    let sessions = Arc::new(SessionManager::new());

    // Spawn Python script first (will connect once server is ready)
    let mgr = PythonManager::new();
    let sessions_clone = Arc::clone(&sessions);

    // Start TCP server in a thread
    std::thread::spawn(move || {
        if let Err(e) = qiming_lib::mcp::server::run_tcp(addr, sessions_clone) {
            eprintln!("MCP TCP error: {}", e);
        }
    });

    // Brief pause to let server start
    std::thread::sleep(std::time::Duration::from_millis(200));

    eprintln!("Running Python script: {} (MCP at {})", script, addr);
    match mgr.run_script(script, addr) {
        Ok(result) => {
            // Print script stdout
            print!("{}", result.stdout);
            if !result.stderr.is_empty() {
                eprint!("{}", result.stderr);
            }
            if result.success {
                eprintln!("Python script completed successfully.");
            } else {
                eprintln!("Python script failed (exit code non-zero).");
            }
        }
        Err(e) => {
            eprintln!("Failed to run Python script: {}", e);
        }
    }

    // Wait for server thread
    drop(sessions);
    // The TCP server runs until killed, so we exit the process
    ffi::shutdown();
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run()
{
    let args: Vec<String> = std::env::args().collect();

    // Check for --tcp (with space-separated addr or = addr)
    if let Some(pos) = args.iter().position(|a| a == "--tcp") {
        if let Some(addr) = args.get(pos + 1) {
            run_tcp(addr);
            return;
        }
    }
    // Also support --tcp=0.0.0.0:9876 format
    for arg in &args {
        if let Some(addr) = arg.strip_prefix("--tcp=") {
            run_tcp(addr);
            return;
        }
    }

    // Check for --run-python <script> — starts TCP server, runs script, exits
    if let Some(pos) = args.iter().position(|a| a == "--run-python") {
        if let Some(script) = args.get(pos + 1) {
            run_tcp_with_python(script);
            return;
        }
    }

    if args.iter().any(|a| a == "--mcp") {
        run_mcp();
        return;
    }

    ffi::init();

    tauri::Builder::default()
        .manage(AppState {
            session: Mutex::new(SessionPtr::null()),
            sessions: qiming_lib::session::SessionManager::new(),
        })
        .invoke_handler(tauri::generate_handler![
            cmd_compile,
            cmd_simulate,
            cmd_shutdown,
            cmd_get_units,
            cmd_session_compile,
            cmd_session_elaborate,
            cmd_session_simulate,
            cmd_query_wave,
            cmd_eval_signal,
            cmd_session_add_breakpoint,
            cmd_session_remove_breakpoint,
            cmd_session_list_breakpoints,
            cmd_session_debug_run,
            cmd_session_get_log,
            cmd_session_get_signals,
        ])
        .on_window_event(|_window, event| {
            if let tauri::WindowEvent::Destroyed = event {
                ffi::shutdown();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn main()
{
    run();
}
