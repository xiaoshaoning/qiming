// Session manager — safe Rust wrapper around the C qsim_session_t API.
// Manages a collection of named simulation sessions.

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::sync::Mutex;

use crate::ffi;

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CoverageEntry {
    pub file: String,
    pub line: u32,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CoverageData {
    pub entries: Vec<CoverageEntry>,
    pub percent: f64,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct WaveEntry {
    pub signal: String,
    pub time_fs: u64,
    pub value: String,
}

/// A single simulation session, wrapping the C qsim_session_t pointer.
pub struct Session {
    ptr: *mut std::ffi::c_void,
}

unsafe impl Send for Session {}
unsafe impl Sync for Session {}

impl Session {
    /// Create a new simulation session.
    pub fn create() -> Option<Self> {
        let ptr = unsafe { ffi::qsim_session_create() };
        if ptr.is_null() {
            None
        } else {
            Some(Session { ptr })
        }
    }

    /// Compile a source string into this session.
    /// Serialized via `ffi::with_c_lock` because the C PEG parsers use global static variables.
    pub fn compile_string(&mut self, name: &str, source: &str) -> Result<(), String> {
        ffi::with_c_lock(|| {
            let c_name = CString::new(name).map_err(|_| "name contains null byte".to_string())?;
            let c_source = CString::new(source).map_err(|_| "source contains null byte".to_string())?;
            let ok = unsafe { ffi::qsim_session_compile_string(self.ptr, c_name.as_ptr(), c_source.as_ptr()) };
            if ok == 0 {
                Err("compile failed".to_string())
            } else {
                Ok(())
            }
        })
    }

    /// Elaborate the compiled design.
    /// Serialized via `ffi::with_c_lock` for C global state safety.
    pub fn elaborate(&mut self) -> Result<(), String> {
        ffi::with_c_lock(|| {
            let ok = unsafe { ffi::qsim_session_elaborate(self.ptr) };
            if ok == 0 {
                Err("elaboration failed".to_string())
            } else {
                Ok(())
            }
        })
    }

    /// Run one delta cycle.
    pub fn step_delta(&mut self) -> Result<(), String> {
        let ok = unsafe { ffi::qsim_session_step_delta(self.ptr) };
        if ok == 0 {
            Err("step_delta failed".to_string())
        } else {
            Ok(())
        }
    }

    /// Get the total number of events processed during simulation.
    pub fn event_count(&self) -> usize {
        unsafe { ffi::qsim_session_get_event_count(self.ptr) }
    }

    /// Get the number of signals in the elaborated design.
    pub fn signal_count(&self) -> i32 {
        unsafe { ffi::qsim_session_get_signal_count(self.ptr) }
    }

    /// Get the name of a signal by index.
    pub fn signal_name(&self, idx: i32) -> Option<String> {
        let ptr = unsafe { ffi::qsim_session_get_signal_name(self.ptr, idx) };
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() })
        }
    }

    /// Evaluate a signal by hierarchical path, returning its value as a string (e.g. "1010XZ").
    pub fn eval_str(&self, signal: &str) -> Result<String, String> {
        let c_signal = CString::new(signal).map_err(|_| "signal contains null byte".to_string())?;
        let ptr = unsafe { ffi::qsim_session_eval_str(self.ptr, c_signal.as_ptr()) };
        if ptr.is_null() {
            return Err("eval failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { ffi::qsim_session_free_str(ptr) };
        Ok(result)
    }

    /// Evaluate multiple signals in one call. Returns a map of signal → value string.
    /// Signals that don't exist or fail to evaluate will have empty-string values.
    pub fn eval_multi(&self, signals: &[&str]) -> Result<Vec<(String, String)>, String> {
        let c_signals: Vec<CString> = signals.iter()
            .map(|s| CString::new(*s).map_err(|_| "signal contains null byte".to_string()))
            .collect::<Result<Vec<_>, _>>()?;
        let raw_ptrs: Vec<*const i8> = c_signals.iter().map(|c| c.as_ptr()).collect();

        let result_ptr = unsafe {
            ffi::qsim_session_eval_multi(self.ptr, raw_ptrs.as_ptr(), signals.len())
        };
        if result_ptr.is_null() {
            return Err("eval_multi failed".to_string());
        }

        let result = unsafe { &*result_ptr };
        let mut pairs = Vec::with_capacity(result.count);
        for i in 0..result.count {
            let val = if result.values.is_null() {
                String::new()
            } else {
                let p = unsafe { *result.values.add(i) };
                if p.is_null() {
                    String::new()
                } else {
                    unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() }
                }
            };
            pairs.push((signals[i].to_string(), val));
        }

        unsafe { ffi::qsim_eval_multi_result_free(result_ptr) };
        Ok(pairs)
    }

    /// Run debug trace: toggle clk for N cycles, record signal values at each posedge.
    /// Returns Vec of cycles, each containing Vec of (signal_name, value_string).
    pub fn debug_trace(&self, signals: &[&str], cycles: usize) -> Result<Vec<Vec<(String, String)>>, String> {
        let c_signals: Vec<CString> = signals.iter()
            .map(|s| CString::new(*s).map_err(|_| "signal contains null byte".to_string()))
            .collect::<Result<Vec<_>, _>>()?;
        let raw_ptrs: Vec<*const i8> = c_signals.iter().map(|c| c.as_ptr()).collect();

        let result_ptr = unsafe {
            ffi::qsim_session_debug_trace(self.ptr, raw_ptrs.as_ptr(), signals.len(), cycles)
        };
        if result_ptr.is_null() {
            return Err("debug_trace failed".to_string());
        }

        let result = unsafe { &*result_ptr };
        let signal_count = result.signal_count;
        let cycle_count = result.cycle_count;
        let mut trace = Vec::with_capacity(cycle_count);

        for c in 0..cycle_count {
            let mut cycle_vals = Vec::with_capacity(signal_count);
            for s in 0..signal_count {
                let val = if result.values.is_null() {
                    String::new()
                } else {
                    let p = unsafe { *result.values.add(c * signal_count + s) };
                    if p.is_null() {
                        String::new()
                    } else {
                        unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() }
                    }
                };
                cycle_vals.push((signals[s].to_string(), val));
            }
            trace.push(cycle_vals);
        }

        unsafe { ffi::qsim_debug_trace_result_free(result_ptr) };
        Ok(trace)
    }

    /// Force a signal to a value (string representation, e.g. "1", "1010").
    pub fn force_str(&self, signal: &str, value: &str) -> Result<(), String> {
        let c_signal = CString::new(signal).map_err(|_| "signal contains null byte".to_string())?;
        let c_value = CString::new(value).map_err(|_| "value contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_force_str(self.ptr, c_signal.as_ptr(), c_value.as_ptr()) };
        if ok == 0 {
            Err("force failed".to_string())
        } else {
            Ok(())
        }
    }

    /// Set a signal via the event queue (triggers process sensitivity).
    pub fn set_str(&self, signal: &str, value: &str) -> Result<(), String> {
        let c_signal = CString::new(signal).map_err(|_| "signal contains null byte".to_string())?;
        let c_value = CString::new(value).map_err(|_| "value contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_set_str(self.ptr, c_signal.as_ptr(), c_value.as_ptr()) };
        if ok == 0 {
            Err("set failed".to_string())
        } else {
            Ok(())
        }
    }

    /// Release a forced signal.
    pub fn release(&self, signal: &str) -> Result<(), String> {
        let c_signal = CString::new(signal).map_err(|_| "signal contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_release(self.ptr, c_signal.as_ptr()) };
        if ok == 0 {
            Err("release failed".to_string())
        } else {
            Ok(())
        }
    }

    /// Get the number of wave buffer entries.
    pub fn wave_count(&self) -> usize {
        unsafe { ffi::qsim_session_get_wave_count(self.ptr) }
    }

    /// Get a wave entry by index. Returns (signal_name, time_fs, value_string).
    pub fn get_wave(&self, idx: usize) -> Option<(String, u64, String)> {
        let mut signal: *const i8 = std::ptr::null();
        let mut time_fs: u64 = 0;
        let mut value = ffi::QsimBitVector {
            width: 0,
            bits: std::ptr::null_mut(),
        };
        let ok = unsafe {
            ffi::qsim_session_get_wave(self.ptr, idx, &mut signal, &mut time_fs, &mut value)
        };
        if ok == 0 || signal.is_null() {
            return None;
        }
        let sig_name = unsafe { CStr::from_ptr(signal).to_string_lossy().into_owned() };

        let mut val_str = String::with_capacity(value.width as usize);
        if !value.bits.is_null() {
            for i in 0..value.width {
                let state = unsafe { (*value.bits.add(i as usize)).state };
                let ch = match state {
                    ffi::LogicState::Qsim1 => '1',
                    ffi::LogicState::QsimX => 'X',
                    ffi::LogicState::QsimZ => 'Z',
                    _ => '0',
                };
                val_str.push(ch);
            }
            // Free the bits allocated by C's malloc
            unsafe { libc::free(value.bits as *mut std::ffi::c_void) };
        }
        Some((sig_name, time_fs, val_str))
    }

    /// Clear the wave buffer.
    pub fn clear_wave(&self) {
        unsafe { ffi::qsim_session_clear_wave(self.ptr) };
    }

    /// Query wave buffer with signal filtering and time window.
    /// Returns Vec of (signal, time_fs, value) tuples sorted by time.
    pub fn query_wave_bulk(&self, signals: &[&str], t_start: u64, t_end: u64) -> Result<Vec<(String, u64, String)>, String> {
        let c_signals: Vec<CString> = signals.iter()
            .map(|s| CString::new(*s).map_err(|_| "signal contains null byte".to_string()))
            .collect::<Result<Vec<_>, _>>()?;
        let raw_ptrs: Vec<*const i8> = c_signals.iter().map(|c| c.as_ptr()).collect();

        let result_ptr = unsafe {
            ffi::qsim_session_query_wave_bulk(
                self.ptr,
                raw_ptrs.as_ptr(),
                signals.len(),
                t_start,
                t_end,
            )
        };
        if result_ptr.is_null() {
            return Err("query_wave_bulk failed".to_string());
        }

        let result = unsafe { &*result_ptr };
        let mut entries = Vec::with_capacity(result.count);
        for i in 0..result.count {
            let signal = if result.signals.is_null() {
                String::new()
            } else {
                let p = unsafe { *result.signals.add(i) };
                if p.is_null() { String::new() }
                else { unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() } }
            };
            let time = if result.times.is_null() { 0 } else { unsafe { *result.times.add(i) } };
            let value = if result.values.is_null() {
                String::new()
            } else {
                let p = unsafe { *result.values.add(i) };
                if p.is_null() { String::new() }
                else { unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() } }
            };
            entries.push((signal, time, value));
        }

        unsafe { ffi::qsim_wave_bulk_result_free(result_ptr) };
        Ok(entries)
    }

    /// Get all wave entries as a serializable vector.
    pub fn query_wave(&self) -> Vec<WaveEntry> {
        let count = self.wave_count();
        let mut entries = Vec::with_capacity(count);
        for i in 0..count {
            if let Some((signal, time_fs, value)) = self.get_wave(i) {
                entries.push(WaveEntry { signal, time_fs, value });
            }
        }
        entries
    }

    /// Get the accumulated log text.
    pub fn get_log(&self) -> Option<String> {
        let ptr = unsafe { ffi::qsim_session_get_log(self.ptr) };
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() })
        }
    }

    /// Get structured diagnostics from the last failed compile or elaborate call.
    pub fn get_last_diagnostics(&self) -> Vec<ffi::Diagnostic> {
        let mut count: usize = 0;
        let ptr = unsafe { ffi::qsim_session_get_last_diagnostics(self.ptr, &mut count) };
        if ptr.is_null() || count == 0 {
            return Vec::new();
        }
        let mut diagnostics = Vec::with_capacity(count);
        for i in 0..count {
            let diag = unsafe { &*ptr.add(i) };
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
            diagnostics.push(ffi::Diagnostic {
                severity: severity.to_string(),
                file,
                line: diag.line,
                column: diag.column,
                message,
                recovery: unsafe { ffi::read_diag_recovery(diag) },
            });
        }
        diagnostics
    }

    /// Add a breakpoint at (file, line).
    pub fn add_breakpoint(&self, file: &str, line: u32) -> Result<(), String> {
        let c_file = CString::new(file).map_err(|_| "file contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_add_breakpoint(self.ptr, c_file.as_ptr(), line) };
        if ok == 0 { Err("add_breakpoint failed".to_string()) } else { Ok(()) }
    }

    /// Remove a breakpoint at (file, line).
    pub fn remove_breakpoint(&self, file: &str, line: u32) -> Result<(), String> {
        let c_file = CString::new(file).map_err(|_| "file contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_remove_breakpoint(self.ptr, c_file.as_ptr(), line) };
        if ok == 0 { Err("remove_breakpoint failed".to_string()) } else { Ok(()) }
    }

    /// Clear all breakpoints.
    pub fn clear_breakpoints(&self) {
        unsafe { ffi::qsim_session_clear_breakpoints(self.ptr) };
    }

    /// List all breakpoints as (file, line) pairs.
    pub fn list_breakpoints(&self) -> Result<Vec<(String, u32)>, String> {
        let count = unsafe { ffi::qsim_session_get_breakpoint_count(self.ptr) };
        let mut bps = Vec::with_capacity(count);
        for i in 0..count {
            let mut file: *const i8 = std::ptr::null();
            let mut line: u32 = 0;
            let ok = unsafe { ffi::qsim_session_get_breakpoint(self.ptr, i, &mut file, &mut line) };
            if ok != 0 && !file.is_null() {
                let f = unsafe { CStr::from_ptr(file).to_string_lossy().into_owned() };
                bps.push((f, line));
            }
        }
        Ok(bps)
    }

    /// Run simulation until breakpoint hit or no events remain.
    /// Returns true if breakpoint was hit, false if simulation completed.
    pub fn debug_run(&self) -> Result<bool, String> {
        let ret = unsafe { ffi::qsim_session_debug_run(self.ptr) };
        match ret {
            -1 => Err("debug_run failed".to_string()),
            0 => Ok(false),
            _ => Ok(true),
        }
    }

    /// Export wave buffer to a VCD file.
    pub fn export_vcd(&self, path: &str) -> Result<(), String> {
        let c_path = CString::new(path).map_err(|_| "path contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_export_vcd(self.ptr, c_path.as_ptr()) };
        if ok == 0 { Err("VCD export failed".to_string()) } else { Ok(()) }
    }

    /// Get a JSON summary of the design hierarchy, ports, signals, instances.
    pub fn design_summary(&self) -> Result<String, String> {
        let ptr = unsafe { ffi::qsim_session_get_design_summary(self.ptr) };
        if ptr.is_null() {
            return Err("design_summary failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }

    /// Extract FSM state registers and transitions as JSON.
    pub fn control_flow(&self) -> Result<String, String> {
        let ptr = unsafe { ffi::qsim_session_get_control_flow(self.ptr) };
        if ptr.is_null() {
            return Err("control_flow failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }

    /// Get line coverage data.
    pub fn get_coverage(&self) -> Result<CoverageData, String> {
        let count = unsafe { ffi::qsim_session_get_coverage_count(self.ptr) };
        let mut entries = Vec::with_capacity(count);
        for i in 0..count {
            let mut file: *const i8 = std::ptr::null();
            let mut line: u32 = 0;
            let ok = unsafe { ffi::qsim_session_get_coverage_entry(self.ptr, i, &mut file, &mut line) };
            if ok != 0 && !file.is_null() {
                let f = unsafe { CStr::from_ptr(file).to_string_lossy().into_owned() };
                entries.push(CoverageEntry { file: f, line });
            }
        }
        let percent = unsafe { ffi::qsim_session_get_coverage_percent(self.ptr) };
        Ok(CoverageData { entries, percent })
    }

    /// Save a simulation checkpoint.
    pub fn save_checkpoint(&self, name: &str) -> Result<String, String> {
        let c_name = CString::new(name).map_err(|_| "name contains null byte".to_string())?;
        let ptr = unsafe { ffi::qsim_session_save_checkpoint(self.ptr, c_name.as_ptr()) };
        if ptr.is_null() {
            return Err("save_checkpoint failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }

    /// Restore simulation state from a checkpoint.
    pub fn restore_checkpoint(&self, name: &str) -> Result<(), String> {
        let c_name = CString::new(name).map_err(|_| "name contains null byte".to_string())?;
        let ok = unsafe { ffi::qsim_session_restore_checkpoint(self.ptr, c_name.as_ptr()) };
        if ok == 0 {
            Err("restore_checkpoint failed".to_string())
        } else {
            Ok(())
        }
    }

    /// Diff two checkpoints and return a JSON string with signal-level differences.
    pub fn diff_checkpoint(&self, name_a: &str, name_b: &str) -> Result<String, String> {
        let c_a = CString::new(name_a).map_err(|_| "name_a contains null byte".to_string())?;
        let c_b = CString::new(name_b).map_err(|_| "name_b contains null byte".to_string())?;
        let ptr = unsafe { ffi::qsim_session_diff_checkpoint(self.ptr, c_a.as_ptr(), c_b.as_ptr()) };
        if ptr.is_null() {
            return Err("diff_checkpoint failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }

    /// List all checkpoints. Returns a JSON string.
    pub fn list_checkpoints(&self) -> Result<String, String> {
        let ptr = unsafe { ffi::qsim_session_list_checkpoints(self.ptr) };
        if ptr.is_null() {
            return Err("list_checkpoints failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }

    /// Trace driver signals for a signal. Returns a JSON string describing the driver chain.
    pub fn trace_drivers(&self, signal: &str, max_depth: usize) -> Result<String, String> {
        let c_signal = CString::new(signal).map_err(|_| "signal contains null byte".to_string())?;
        let ptr = unsafe { ffi::qsim_session_trace_drivers(self.ptr, c_signal.as_ptr(), max_depth) };
        if ptr.is_null() {
            return Err("trace_drivers failed".to_string());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { libc::free(ptr as *mut std::ffi::c_void) };
        Ok(result)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        unsafe { ffi::qsim_session_free(self.ptr) };
    }
}

/// Thread-safe session manager — holds multiple named sessions.
pub struct SessionManager {
    sessions: Mutex<HashMap<String, Session>>,
}

impl SessionManager {
    pub fn new() -> Self {
        SessionManager {
            sessions: Mutex::new(HashMap::new()),
        }
    }

    /// Create a new session with a unique ID. Returns the session ID.
    pub fn create(&self) -> Result<String, String> {
        let session = Session::create().ok_or("failed to create session".to_string())?;
        let id = uuid::Uuid::new_v4().to_string();
        let mut map = self.sessions.lock().map_err(|e| e.to_string())?;
        map.insert(id.clone(), session);
        Ok(id)
    }

    /// Destroy a session by ID.
    pub fn destroy(&self, id: &str) -> Result<(), String> {
        let mut map = self.sessions.lock().map_err(|e| e.to_string())?;
        map.remove(id).ok_or_else(|| "session not found".to_string())?;
        Ok(())
    }

    /// Execute a closure with a mutable reference to a session.
    pub fn with_session<F, T>(&self, id: &str, f: F) -> Result<T, String>
    where
        F: FnOnce(&mut Session) -> Result<T, String>,
    {
        let mut map = self.sessions.lock().map_err(|e| e.to_string())?;
        let session = map.get_mut(id).ok_or_else(|| "session not found".to_string())?;
        f(session)
    }

    /// Execute a closure with an immutable reference to a session.
    pub fn with_session_readonly<F, T>(&self, id: &str, f: F) -> Result<T, String>
    where
        F: FnOnce(&Session) -> Result<T, String>,
    {
        let map = self.sessions.lock().map_err(|e| e.to_string())?;
        let session = map.get(id).ok_or_else(|| "session not found".to_string())?;
        f(session)
    }

    /// List all active session IDs.
    pub fn list(&self) -> Result<Vec<String>, String> {
        let map = self.sessions.lock().map_err(|e| e.to_string())?;
        Ok(map.keys().cloned().collect())
    }

    /// Get the number of active sessions.
    pub fn count(&self) -> usize {
        self.sessions.lock().map(|m| m.len()).unwrap_or(0)
    }

    /// Destroy all sessions.
    pub fn clear(&self) {
        if let Ok(mut map) = self.sessions.lock() {
            map.clear();
        }
    }
}
