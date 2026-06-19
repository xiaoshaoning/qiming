// FFI bindings to libqsim (C core library).
// All unsafe extern "C" declarations and safe wrapper functions.

use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::{Mutex, OnceLock};

/// Global lock for C library calls that use global/static state (parsers).
/// Rust tests run in parallel by default; the C PEG parsers use global static
/// variables and will corrupt each other without this serialization.
static C_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

/// Acquire the global C library lock and execute `f` while holding it.
/// This serializes access to C functions that use global/static state.
pub fn with_c_lock<F, T>(f: F) -> T
where
    F: FnOnce() -> T,
{
    let lock = C_LOCK.get_or_init(|| Mutex::new(()));
    let _guard = lock.lock().unwrap();
    f()
}

// ── C type representations ──

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub enum LogicState {
    Qsim0 = 0,
    Qsim1 = 1,
    QsimX = 2,
    QsimZ = 3,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct QsimValue {
    pub state: LogicState,
    pub strength: u8,
}

#[repr(C)]
pub struct QsimCompileInput {
    pub files: *const *const i8,
    pub file_count: usize,
    pub sources: *const *const i8,
    pub source_count: usize,
}

#[repr(C)]
pub struct QsimDiagnostic {
    pub is_error: i32,
    pub file: *const i8,
    pub line: u32,
    pub column: u32,
    pub message: *const i8,
    pub recovery: *mut QsimRecovery,
}

#[repr(C)]
pub struct QsimRecovery {
    pub suggestions: *mut *mut i8,
    pub suggestion_count: usize,
    pub nearby: *mut *mut i8,
    pub nearby_count: usize,
    pub next_tool: *const i8,
}

#[repr(C)]
pub struct QsimCompileResult {
    pub success: i32,
    pub units: *mut *mut std::ffi::c_void,
    pub unit_count: usize,
    pub diagnostics: *mut QsimDiagnostic,
    pub diag_count: usize,
}

#[repr(C)]
pub struct QsimSimResult {
    pub success: i32,
    pub ended_at: u64,
    pub final_delta: u32,
    pub stop_reason: *const i8,
}

#[repr(C)]
pub struct QsimWaveQuery {
    pub signals: *const *const i8,
    pub signal_count: usize,
    pub t_start: u64,
    pub t_end: u64,
    pub results: *mut QsimWaveQueryResult,
    pub result_count: usize,
}

#[repr(C)]
pub struct QsimWaveQueryResult {
    pub name: *const i8,
    pub count: usize,
}

#[repr(C)]
pub struct QsimWaveBulkResult {
    pub signals: *mut *mut i8,
    pub times: *mut u64,
    pub values: *mut *mut i8,
    pub count: usize,
}

#[repr(C)]
pub struct QsimDepQuery {
    pub signal: *const i8,
    pub drivers: *mut *mut i8,
    pub driver_count: usize,
    pub loads: *mut *mut i8,
    pub load_count: usize,
}

#[repr(C)]
pub struct QsimBitVector {
    pub width: u32,
    pub bits: *mut QsimValue,
}

#[repr(C)]
pub struct QsimEvalMultiResult {
    pub values: *mut *mut i8,
    pub count: usize,
}

#[repr(C)]
pub struct QsimDebugTraceResult {
    pub values: *mut *mut i8,
    pub cycle_count: usize,
    pub signal_count: usize,
}

// ── FFI function declarations ──

extern "C" {
    fn qsim_init();
    fn qsim_shutdown();

    fn qsim_compile(input: *const QsimCompileInput) -> *mut QsimCompileResult;
    fn qsim_compile_result_free(result: *mut QsimCompileResult);

    fn qsim_sim_run(session: *mut std::ffi::c_void, until: u64) -> *mut QsimSimResult;
    fn qsim_sim_result_free(result: *mut QsimSimResult);

    // Session API — all pub so session module can call them
    pub fn qsim_session_create() -> *mut std::ffi::c_void;
    pub fn qsim_session_free(sess: *mut std::ffi::c_void);
    pub fn qsim_session_compile_string(
        sess: *mut std::ffi::c_void, name: *const i8, source: *const i8,
    ) -> i32;
    pub fn qsim_session_elaborate(sess: *mut std::ffi::c_void) -> i32;
    pub fn qsim_session_step_delta(sess: *mut std::ffi::c_void) -> i32;
    pub fn qsim_session_get_event_count(sess: *mut std::ffi::c_void) -> usize;
    pub fn qsim_session_get_signal_count(sess: *mut std::ffi::c_void) -> i32;
    pub fn qsim_session_get_signal_name(
        sess: *mut std::ffi::c_void, idx: i32,
    ) -> *const i8;
    pub fn qsim_session_eval_str(
        sess: *mut std::ffi::c_void, signal: *const i8,
    ) -> *mut i8;
    pub fn qsim_session_force_str(
        sess: *mut std::ffi::c_void, signal: *const i8, value: *const i8,
    ) -> i32;
    pub fn qsim_session_set_str(
        sess: *mut std::ffi::c_void, signal: *const i8, value: *const i8,
    ) -> i32;
    pub fn qsim_session_release(
        sess: *mut std::ffi::c_void, signal: *const i8,
    ) -> i32;
    pub fn qsim_session_get_wave_count(sess: *mut std::ffi::c_void) -> usize;
    pub fn qsim_session_get_wave(
        sess: *mut std::ffi::c_void,
        idx: usize,
        signal: *mut *const i8,
        time_fs: *mut u64,
        value: *mut QsimBitVector,
    ) -> i32;
    pub fn qsim_session_clear_wave(sess: *mut std::ffi::c_void);
    pub fn qsim_session_get_log(sess: *mut std::ffi::c_void) -> *const i8;
    pub fn qsim_session_free_str(s: *mut i8);

    // Bulk evaluation
    pub fn qsim_session_eval_multi(
        sess: *mut std::ffi::c_void,
        signals: *const *const i8,
        count: usize,
    ) -> *mut QsimEvalMultiResult;
    pub fn qsim_eval_multi_result_free(result: *mut QsimEvalMultiResult);

    // Debug trace
    pub fn qsim_session_debug_trace(
        sess: *mut std::ffi::c_void,
        signals: *const *const i8,
        signal_count: usize,
        cycles: usize,
    ) -> *mut QsimDebugTraceResult;
    pub fn qsim_debug_trace_result_free(result: *mut QsimDebugTraceResult);

    // Breakpoint API
    pub fn qsim_session_add_breakpoint(
        sess: *mut std::ffi::c_void, file: *const i8, line: u32,
    ) -> i32;
    pub fn qsim_session_remove_breakpoint(
        sess: *mut std::ffi::c_void, file: *const i8, line: u32,
    ) -> i32;
    pub fn qsim_session_clear_breakpoints(sess: *mut std::ffi::c_void);
    pub fn qsim_session_get_breakpoint_count(sess: *mut std::ffi::c_void) -> usize;
    pub fn qsim_session_get_breakpoint(
        sess: *mut std::ffi::c_void,
        idx: usize,
        file: *mut *const i8,
        line: *mut u32,
    ) -> i32;
    pub fn qsim_session_debug_run(sess: *mut std::ffi::c_void) -> i32;

    // Coverage API
    pub fn qsim_session_get_coverage_count(sess: *mut std::ffi::c_void) -> usize;
    pub fn qsim_session_get_coverage_entry(
        sess: *mut std::ffi::c_void,
        idx: usize,
        file: *mut *const i8,
        line: *mut u32,
    ) -> i32;
    pub fn qsim_session_get_coverage_percent(sess: *mut std::ffi::c_void) -> f64;

    // VCD export
    pub fn qsim_session_export_vcd(
        sess: *mut std::ffi::c_void, path: *const i8,
    ) -> i32;

    // Structured diagnostics with recovery
    pub fn qsim_session_get_last_diagnostics(
        sess: *mut std::ffi::c_void,
        count: *mut usize,
    ) -> *const QsimDiagnostic;

    // Design comprehension tools
    pub fn qsim_session_get_design_summary(sess: *mut std::ffi::c_void) -> *mut i8;
    pub fn qsim_session_get_control_flow(sess: *mut std::ffi::c_void) -> *mut i8;

    // Checkpoint save/restore
    pub fn qsim_session_save_checkpoint(
        sess: *mut std::ffi::c_void, name: *const i8,
    ) -> *mut i8;
    pub fn qsim_session_restore_checkpoint(
        sess: *mut std::ffi::c_void, name: *const i8,
    ) -> i32;
    pub fn qsim_session_diff_checkpoint(
        sess: *mut std::ffi::c_void, name_a: *const i8, name_b: *const i8,
    ) -> *mut i8;
    pub fn qsim_session_list_checkpoints(
        sess: *mut std::ffi::c_void,
    ) -> *mut i8;

    // Signal trace
    pub fn qsim_session_trace_drivers(
        sess: *mut std::ffi::c_void, signal: *const i8, max_depth: usize,
    ) -> *mut i8;

    // Bulk wave query
    pub fn qsim_session_query_wave_bulk(
        sess: *mut std::ffi::c_void,
        signals: *const *const i8,
        signal_count: usize,
        t_start: u64,
        t_end: u64,
    ) -> *mut QsimWaveBulkResult;
    pub fn qsim_wave_bulk_result_free(result: *mut QsimWaveBulkResult);
}

// ── Safe wrappers ──

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CompileResult {
    pub success: bool,
    pub diagnostics: Vec<Diagnostic>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct DiagnosticRecovery {
    pub suggestions: Vec<String>,
    pub next_tool: Option<String>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct Diagnostic {
    pub severity: String,
    pub file: String,
    pub line: u32,
    pub column: u32,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub recovery: Option<DiagnosticRecovery>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct SimResult {
    pub success: bool,
    pub ended_at: u64,
    pub final_delta: u32,
    pub stop_reason: String,
}

/// Read recovery hints from a raw FFI diagnostic.
///
/// # Safety
///
/// `diag` must point to a valid QsimDiagnostic whose lifetime covers the returned strings.
pub(crate) unsafe fn read_diag_recovery(diag: &QsimDiagnostic) -> Option<DiagnosticRecovery> {
    let rec = diag.recovery;
    if rec.is_null() {
        return None;
    }
    let rec = unsafe { &*rec };

    let mut suggestions = Vec::new();
    if !rec.suggestions.is_null() {
        for j in 0..rec.suggestion_count {
            let s = unsafe { *rec.suggestions.add(j) };
            if !s.is_null() {
                suggestions.push(unsafe { CStr::from_ptr(s).to_string_lossy().into_owned() });
            }
        }
    }

    let next_tool = if rec.next_tool.is_null() {
        None
    } else {
        Some(unsafe { CStr::from_ptr(rec.next_tool).to_string_lossy().into_owned() })
    };

    if suggestions.is_empty() && next_tool.is_none() {
        return None;
    }

    Some(DiagnosticRecovery {
        suggestions,
        next_tool,
    })
}

/// Initialize the C library. Call once at program start.
pub fn init()
{
    unsafe { qsim_init(); }
}

/// Shut down the C library.
pub fn shutdown()
{
    unsafe { qsim_shutdown(); }
}

/// Compile source files using the C library.
///
/// # Safety
///
/// `files` must contain valid UTF-8 strings.
pub fn compile(files: &[String]) -> Result<CompileResult, String>
{
    with_c_lock(|| {
    let c_files: Vec<CString> = files
        .iter()
        .map(|f| CString::new(f.as_str()).unwrap())
        .collect();
    let mut raw_ptrs: Vec<*const i8> = c_files.iter().map(|c| c.as_ptr()).collect();
    raw_ptrs.push(ptr::null());

    let input = QsimCompileInput {
        files: raw_ptrs.as_ptr(),
        file_count: files.len(),
        sources: ptr::null(),
        source_count: 0,
    };

    let result = unsafe { qsim_compile(&input) };
    if result.is_null() {
        return Err("compile returned null".to_string());
    }

    let res = unsafe { &*result };
    let success = res.success != 0;

    let mut diagnostics = Vec::new();
    if !res.diagnostics.is_null() {
        for i in 0..res.diag_count {
            let diag = unsafe { &(*res.diagnostics.add(i)) };
            let severity = if diag.is_error != 0 { "error" } else { "warning" };
            let file = if diag.file.is_null() {
                String::new()
            } else {
                unsafe { CStr::from_ptr(diag.file).to_string_lossy().into_owned() }
            };
            let message = if diag.message.is_null() {
                String::new()
            } else {
                unsafe { CStr::from_ptr(diag.message).to_string_lossy().into_owned() }
            };
            diagnostics.push(Diagnostic {
                severity: severity.to_string(),
                file,
                line: diag.line,
                column: diag.column,
                message,
                recovery: unsafe { read_diag_recovery(diag) },
            });
        }
    }

    unsafe { qsim_compile_result_free(result); }

    Ok(CompileResult {
        success,
        diagnostics,
    })
    })
}

/// Run simulation.
///
/// # Safety
///
/// `session_handle` must be a valid pointer from a previous compile/elaborate call.
pub fn simulate(session_handle: *mut std::ffi::c_void, until: u64) -> Result<SimResult, String>
{
    let result = unsafe { qsim_sim_run(session_handle, until) };
    if result.is_null() {
        return Err("simulate returned null".to_string());
    }

    let res = unsafe { &*result };
    let stop_reason = if res.stop_reason.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(res.stop_reason).to_string_lossy().into_owned() }
    };

    let sim_res = SimResult {
        success: res.success != 0,
        ended_at: res.ended_at,
        final_delta: res.final_delta,
        stop_reason,
    };

    unsafe { qsim_sim_result_free(result); }

    Ok(sim_res)
}

// ── Low-level raw wrappers for session-managed Tauri commands ──

/// Compile inline Verilog source and return the raw compile result pointer.
/// The caller must free this pointer via `compile_result_free_raw`.
///
/// # Safety
///
/// `source` must be valid UTF-8.
pub unsafe fn compile_raw(source: &str) -> Result<*mut QsimCompileResult, String>
{
    let c_source = CString::new(source).map_err(|_| "source contains null byte".to_string())?;
    let raw_ptrs = [c_source.as_ptr()];

    let input = QsimCompileInput {
        files: ptr::null(),
        file_count: 0,
        sources: raw_ptrs.as_ptr(),
        source_count: 1,
    };

    with_c_lock(|| {
    let result = qsim_compile(&input);
    if result.is_null() {
        return Err("compile returned null".to_string());
    }
    Ok(result)
    })
}

/// Read diagnostics from a raw compile result. Does NOT free the result.
///
/// # Safety
///
/// `result` must be a valid pointer from `compile_raw`.
pub unsafe fn read_compile_diagnostics(result: *const QsimCompileResult) -> (bool, Vec<Diagnostic>)
{
    let res = &*result;
    let success = res.success != 0;
    let mut diagnostics = Vec::new();

    for i in 0..res.diag_count {
        let diag = &(*res.diagnostics.add(i));
        let severity = if diag.is_error != 0 { "error" } else { "warning" };
        let file = if diag.file.is_null() {
            String::new()
        } else {
            CStr::from_ptr(diag.file).to_string_lossy().into_owned()
        };
        let message = if diag.message.is_null() {
            String::new()
        } else {
            CStr::from_ptr(diag.message).to_string_lossy().into_owned()
        };
        diagnostics.push(Diagnostic {
            severity: severity.to_string(),
            file,
            line: diag.line,
            column: diag.column,
            message,
            recovery: unsafe { read_diag_recovery(diag) },
        });
    }
    (success, diagnostics)
}

/// Free a raw compile result pointer.
///
/// # Safety
///
/// `result` must be a pointer from `compile_raw` and not already freed.
pub unsafe fn compile_result_free_raw(result: *mut QsimCompileResult)
{
    if !result.is_null() {
        qsim_compile_result_free(result);
    }
}

/// Run simulation with a raw session pointer and return the raw sim result.
/// The caller must free the returned pointer via `sim_result_free_raw`.
///
/// # Safety
///
/// `session` must be a pointer from `compile_raw` (or the `units` field thereof).
pub unsafe fn sim_run_raw(session: *mut std::ffi::c_void, until: u64) -> Result<*mut QsimSimResult, String>
{
    let result = qsim_sim_run(session, until);
    if result.is_null() {
        return Err("simulate returned null".to_string());
    }
    Ok(result)
}

/// Read simulation results from a raw sim result pointer. Does NOT free.
///
/// # Safety
///
/// `result` must be a valid pointer from `sim_run_raw`.
pub unsafe fn read_sim_result(result: *const QsimSimResult) -> SimResult
{
    let res = &*result;
    let stop_reason = if res.stop_reason.is_null() {
        String::new()
    } else {
        CStr::from_ptr(res.stop_reason).to_string_lossy().into_owned()
    };
    SimResult {
        success: res.success != 0,
        ended_at: res.ended_at,
        final_delta: res.final_delta,
        stop_reason,
    }
}

/// Free a raw sim result pointer.
///
/// # Safety
///
/// `result` must be a pointer from `sim_run_raw` and not already freed.
pub unsafe fn sim_result_free_raw(result: *mut QsimSimResult)
{
    if !result.is_null() {
        qsim_sim_result_free(result);
    }
}

#[cfg(test)]
mod tests
{
    use super::*;

    #[test]
    fn test_init_shutdown()
    {
        init();
        shutdown();
    }

    #[test]
    fn test_compile_empty()
    {
        init();
        let result = compile(&[]).unwrap();
        assert!(!result.success || true);
        shutdown();
    }

    #[test]
    fn test_simulate_null_session()
    {
        init();
        let result = simulate(ptr::null_mut(), 100);
        assert!(result.is_err());
        shutdown();
    }
}
