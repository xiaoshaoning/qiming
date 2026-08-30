// CLI tool — subcommands: compile, simulate, run, bench, mcp.
// Links against qiming_lib for C library access.

use std::fs;
use std::process;
use std::time::Instant;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.len() < 2 {
        print_usage();
        process::exit(1);
    }

    let result = match args[1].as_str() {
        "compile" => cmd_compile(&args[2..]),
        "simulate" => cmd_simulate(&args[2..]),
        "elaborate" => cmd_elaborate(&args[2..]),
        "signals" => cmd_signals(&args[2..]),
        "run" => cmd_run(&args[2..]),
        "bench" => cmd_bench(&args[2..]),
        "mcp" => cmd_mcp(&args[2..]),
        "help" | "--help" | "-h" => {
            print_usage();
            Ok(())
        }
        sub => {
            eprintln!("error: unknown subcommand '{}'", sub);
            print_usage();
            process::exit(1);
        }
    };

    if let Err(e) = result {
        eprintln!("error: {}", e);
        process::exit(1);
    }
}

fn print_usage() {
    eprintln!("Qiming Simulator CLI");
    eprintln!();
    eprintln!("Usage:");
    eprintln!("  qsim compile <files...>            Compile design files (Verilog/VHDL)");
    eprintln!("  qsim elaborate <files...>          Compile and elaborate design files");
    eprintln!("  qsim signals <files...>            Compile, elaborate, list all signals");
    eprintln!("  qsim simulate <files...> [steps] [--clock [signal]]   Compile and simulate (optional step count / clock drive)");
    eprintln!("  qsim run <design_file> <hex_file> [cycles] Load program and run CPU");
    eprintln!("  qsim bench [--save FILE]           Run performance benchmark");
    eprintln!("  qsim mcp                           Start MCP server on stdin/stdout");
    eprintln!("  qsim mcp --tcp <addr>              Start MCP server on TCP address");
    eprintln!("  qsim help                          Print this help");
}

fn cmd_compile(args: &[String]) -> Result<(), String> {
    if args.is_empty() {
        return Err("missing file argument".to_string());
    }

    qiming_lib::ffi::init();

    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;

    for file in args {
        let source = fs::read_to_string(file)
            .map_err(|e| format!("cannot read '{}': {}", file, e))?;
        match session.compile_string(file, &source) {
            Ok(()) => {
                println!("Compile OK: {}", file);
            }
            Err(e) => {
                eprintln!("Compile FAILED: {} ({})", file, e);
            }
        }
    }

    if let Some(log) = session.get_log() {
        if !log.is_empty() {
            println!("--- Diagnostics ---");
            println!("{}", log);
        }
    }

    drop(session);
    qiming_lib::ffi::shutdown();
    Ok(())
}

fn cmd_simulate(args: &[String]) -> Result<(), String> {
    if args.is_empty() {
        return Err("missing file argument".to_string());
    }

    // Arguments: files... [steps] [--clock [name]] [--until <time>]
    let mut files: Vec<&String> = Vec::new();
    let mut steps: u64 = 10;
    let mut clock: Option<String> = None;
    let mut until: Option<u64> = None;
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--clock" {
            // Optional signal name; defaults to "clk".
            if i + 1 < args.len() && !args[i + 1].starts_with("--") && args[i + 1].parse::<u64>().is_err() {
                clock = Some(args[i + 1].clone());
                i += 2;
            } else {
                clock = Some("clk".to_string());
                i += 1;
            }
        } else if a == "--until" {
            if i + 1 < args.len() {
                until = args[i + 1].parse::<u64>().ok();
                i += 2;
            } else {
                return Err("--until requires a time value".to_string());
            }
        } else if let Ok(n) = a.parse::<u64>() {
            steps = n;
            i += 1;
        } else {
            files.push(a);
            i += 1;
        }
    }

    if files.is_empty() {
        return Err("missing file argument".to_string());
    }

    qiming_lib::ffi::init();

    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;

    for file in &files {
        let source = fs::read_to_string(file)
            .map_err(|e| format!("cannot read '{}': {}", file, e))?;
        session.compile_string(file, &source)
            .map_err(|e| format!("compile failed for '{}': {}", file, e))?;
        println!("Compile OK: {}", file);
    }

    session.elaborate()
        .map_err(|e| format!("elaboration failed: {}", e))?;
    println!("Elaborate OK");

    let sig_count = session.signal_count();
    println!("Signals ({}):", sig_count);
    for i in 0..sig_count {
        if let Some(sig_name) = session.signal_name(i) {
            if let Ok(val) = session.eval_str(&sig_name) {
                println!("  {} = {}", sig_name, val);
            } else {
                println!("  {} = ?", sig_name);
            }
        }
    }

    if let Some(clk) = &clock {
        println!("Auto-driving clock '{}' (toggle each delta step; posedge every 2 steps)", clk);
    } else {
        println!("Note: no stimulus is applied -- signals will stay at their initial values.");
        println!("      Use --clock <signal> to auto-drive a clock during simulation.");
    }

    if let Some(until) = until {
        // Time-based mode: advance through #-delay events up to `until`
        // (the testbench is expected to generate its own clocks).
        println!("Simulating until time {}...", until);
        let elapsed = session.step_time(until);
        println!("Advanced {} time units; final state:", elapsed);
        for i in 0..sig_count {
            if let Some(sig_name) = session.signal_name(i) {
                if let Ok(val) = session.eval_str(&sig_name) {
                    println!("  {} = {}", sig_name, val);
                }
            }
        }
    } else {
        println!("Simulating for {} delta steps...", steps);
        for step in 0..steps {
            // Drive the clock before each step (odd steps high, even steps low),
            // producing a rising edge every two steps.
            if let Some(clk) = &clock {
                session.force_str(clk, if step % 2 == 0 { "1" } else { "0" }).ok();
            }
            match session.step_delta() {
                Ok(()) => {
                    println!("  Step {}: signals updated", step + 1);
                    for i in 0..sig_count {
                        if let Some(sig_name) = session.signal_name(i) {
                            if let Ok(val) = session.eval_str(&sig_name) {
                                println!("    {} = {}", sig_name, val);
                            }
                        }
                    }
                }
                Err(e) => {
                    println!("  Step {}: done ({})", step + 1, e);
                    break;
                }
            }
        }
    }

    // Print any $display/$monitor output accumulated during simulation.
    if let Some(log) = session.get_log() {
        if !log.trim().is_empty() {
            println!("{}", log);
        }
    }

    println!("Wave entries: {}", session.wave_count());

    drop(session);
    qiming_lib::ffi::shutdown();
    Ok(())
}

/// Compile and elaborate without simulation.
fn cmd_elaborate(args: &[String]) -> Result<(), String> {
    if args.is_empty() {
        return Err("missing file argument".to_string());
    }

    qiming_lib::ffi::init();

    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;

    for file in args {
        let source = fs::read_to_string(file)
            .map_err(|e| format!("cannot read '{}': {}", file, e))?;
        session.compile_string(file, &source)
            .map_err(|e| format!("compile failed for '{}': {}", file, e))?;
        println!("Compile OK: {}", file);
    }

    session.elaborate()
        .map_err(|e| format!("elaboration failed: {}", e))?;
    println!("Elaborate OK");

    if let Some(log) = session.get_log() {
        if !log.is_empty() {
            println!("--- Diagnostics ---");
            println!("{}", log);
        }
    }

    drop(session);
    qiming_lib::ffi::shutdown();
    Ok(())
}

/// Compile, elaborate, and list all signals with their values.
fn cmd_signals(args: &[String]) -> Result<(), String> {
    if args.is_empty() {
        return Err("missing file argument".to_string());
    }

    qiming_lib::ffi::init();

    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;

    for file in args {
        let source = fs::read_to_string(file)
            .map_err(|e| format!("cannot read '{}': {}", file, e))?;
        session.compile_string(file, &source)
            .map_err(|e| format!("compile failed for '{}': {}", file, e))?;
        println!("Compile OK: {}", file);
    }

    session.elaborate()
        .map_err(|e| format!("elaboration failed: {}", e))?;

    let sig_count = session.signal_count();
    println!("Signals ({}):", sig_count);
    for i in 0..sig_count {
        if let Some(sig_name) = session.signal_name(i) {
            match session.eval_str(&sig_name) {
                Ok(val) => {
                    // Truncate very long values (e.g., large memories)
                    let display = if val.len() > 80 {
                        format!("{}... ({} bits)", &val[..77], val.len())
                    } else {
                        val
                    };
                    println!("  {} = {}", sig_name, display);
                }
                Err(e) => {
                    println!("  {} = <error: {}>", sig_name, e);
                }
            }
        }
    }

    if let Some(log) = session.get_log() {
        if !log.is_empty() {
            println!("--- Diagnostics ---");
            println!("{}", log);
        }
    }

    drop(session);
    qiming_lib::ffi::shutdown();
    Ok(())
}

// ── CPU runner ──

/// Load a hex file (one 8-digit 32-bit hex value per line) into the 32768-bit
/// (1024 x 32) imem bit string, MSB-first. Unused slots are filled with EBREAK.
fn load_hex(path: &str) -> Result<String, String> {
    const IMEM_WORDS: usize = 1024;
    const IMEM_BITS: usize = IMEM_WORDS * 32;
    const EBREAK: u32 = 0x0010_0073;

    let content = fs::read_to_string(path)
        .map_err(|e| format!("cannot read '{}': {}", path, e))?;
    let mut program: Vec<u32> = Vec::new();
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }
        let val = u32::from_str_radix(line.trim_start_matches("0x"), 16)
            .map_err(|e| format!("bad hex '{}': {}", line, e))?;
        program.push(val);
    }

    // Build MSB-first bit string: word i occupies bits [i*32+31 : i*32],
    // overall bit 0 = LSB of word 0 → last char of the string.
    let mut s = String::with_capacity(IMEM_BITS);
    for i in (0..IMEM_WORDS).rev() {
        let val = program.get(i).copied().unwrap_or(EBREAK);
        // Emit bits 31..0 (MSB-first within each word).
        for b in (0..32).rev() {
            s.push(if (val >> b) & 1 == 1 { '1' } else { '0' });
        }
    }
    Ok(s)
}

/// Run a CPU design with a loaded program.
/// Usage: qsim run <design_file> <hex_file> [max_cycles]
fn cmd_run(args: &[String]) -> Result<(), String> {
    if args.len() < 2 {
        return Err("usage: qsim run <design_file> <hex_file> [max_cycles]".to_string());
    }

    let design_file = &args[0];
    let hex_file = &args[1];
    let max_cycles: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(100);

    // Read and compile the design
    let source = fs::read_to_string(design_file)
        .map_err(|e| format!("cannot read '{}': {}", design_file, e))?;

    qiming_lib::ffi::init();

    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;

    session.compile_string(design_file, &source)
        .map_err(|e| {
            let log = session.get_log().unwrap_or_default();
            format!("compile failed: {} (log: {})", e, log)
        })?;
    println!("Compile OK");

    session.elaborate()
        .map_err(|e| format!("elaboration failed: {}", e))?;
    println!("Elaborate OK");

    // Load program and force onto imem (Verilog: "imem", VHDL: "imem_port")
    let program = load_hex(hex_file)?;
    println!("Program loaded: {}", hex_file);

    let imem_sig = ["imem", "imem_port"]
        .iter()
        .find(|sig| session.force_str(sig, &program).is_ok())
        .copied()
        .ok_or_else(|| "force imem failed: no imem/imem_port signal found".to_string())?;
    println!("IMEM forced ({}), {} bits", imem_sig, program.len());

    // Reset: force rst=1, clk=0, init_en=0 then step 2 deltas
    session.force_str("rst", "1").ok();
    session.force_str("clk", "0").ok();
    session.force_str("init_en", "0").ok();
    session.step_delta().ok();
    session.step_delta().ok();

    // Release reset: rst=0
    session.force_str("rst", "0").ok();
    session.step_delta().ok();
    session.step_delta().ok();

    println!("Running for up to {} cycles...", max_cycles);
    println!();

    // Helper to read a signal and strip leading zeros
    let eval = |sess: &qiming_lib::session::Session, name: &str| -> String {
        sess.eval_str(name).unwrap_or_else(|_| "?".to_string())
    };

    // Header
    println!("{:>4}  {:>5}  {:>16}  {:>10}  {:>10}",
             "cycle", "halted", "pc", "x1", "x10");

    let mut halted = false;
    for cycle in 0..max_cycles {
        // Clock low → step, then clock high → step
        session.force_str("clk", "0").ok();
        session.step_delta().ok();
        session.force_str("clk", "1").ok();
        session.step_delta().ok();

        // Read signals (design exposes reg_x1 and reg_x10)
        let h = eval(&session, "halted");
        let pc = eval(&session, "pc");
        let x1 = eval(&session, "reg_x1");
        let x10 = eval(&session, "reg_x10");

        // Convert binary strings to hex display
        let pc_hex = bin_to_hex(&pc);
        let x1_hex = bin_to_hex(&x1);
        let x10_hex = bin_to_hex(&x10);

        println!("{:>4}  {:>5}  {:>16}  {:>10}  {:>10}",
                 cycle, h, pc_hex, x1_hex, x10_hex);

        if h == "1" {
            halted = true;
            println!();
            println!("HALTED after {} cycles", cycle + 1);
            break;
        }
    }

    if !halted {
        println!();
        println!("Reached cycle limit ({}) without halting", max_cycles);
    }

    // Print any $display output from the design/testbench.
    if let Some(log) = session.get_log() {
        if !log.trim().is_empty() {
            println!("{}", log);
        }
    }

    println!("Wave entries: {}", session.wave_count());

    drop(session);
    qiming_lib::ffi::shutdown();
    Ok(())
}

/// Convert an LSB-first binary string to a hex display string.
fn bin_to_hex(bin: &str) -> String {
    if bin.contains('X') || bin.contains('Z') {
        return bin.chars().rev().collect::<String>();
    }
    // Parse LSB-first binary to a u64
    let mut val: u64 = 0;
    for (i, ch) in bin.chars().enumerate() {
        if ch == '1' {
            val |= 1 << i;
        }
    }
    let bits = bin.len();
    if bits <= 4 {
        format!("0x{:01X}", val)
    } else if bits <= 8 {
        format!("0x{:02X}", val)
    } else if bits <= 16 {
        format!("0x{:04X}", val)
    } else {
        format!("0x{:08X}", val)
    }
}

/// Performance benchmark: compile a counter, drive clock via force + step_delta,
/// measure throughput in steps/sec and wave entries/sec.
fn cmd_bench(args: &[String]) -> Result<(), String> {
    let save_path = args
        .windows(2)
        .find(|w| w[0] == "--save")
        .and_then(|w| Some(w[1].clone()));

    const SRC: &str = r#"
module counter(input clk, output reg [3:0] count);
  always @(posedge clk) begin
    count <= count + 4'b1;
  end
endmodule
"#;

    const STEPS: u64 = 10000;

    println!("Qiming Performance Benchmark");
    println!("============================");
    println!("Design: 4-bit counter, clock driven via force/release");
    println!("Delta steps: {}", STEPS);
    println!();

    qiming_lib::ffi::init();

    // ── Compile ──
    let t0 = Instant::now();
    let mut session = qiming_lib::session::Session::create()
        .ok_or_else(|| "failed to create session".to_string())?;
    session.compile_string("counter.v", SRC)
        .map_err(|e| {
            let log = session.get_log().unwrap_or_default();
            format!("compile failed: {} (log: {})", e, log)
        })?;
    let t_compile = t0.elapsed();
    println!("  Compile:   {:.1} ms", t_compile.as_secs_f64() * 1000.0);

    // ── Elaborate ──
    let t0 = Instant::now();
    session.elaborate()
        .map_err(|e| format!("elaborate failed: {}", e))?;
    let t_elab = t0.elapsed();
    println!("  Elaborate: {:.1} ms", t_elab.as_secs_f64() * 1000.0);

    // ── Force initial value ──
    let _ = session.force_str("clk", "0");

    // ── Step: toggle clock each delta, measure throughput ──
    let t0 = Instant::now();
    let mut steps_done = 0u64;
    for i in 0..STEPS {
        let clk_val = if i % 2 == 0 { "1" } else { "0" };
        let _ = session.force_str("clk", clk_val);
        match session.step_delta() {
            Ok(()) => steps_done += 1,
            Err(_) => break,
        }
    }
    let t_sim = t0.elapsed();

    let sim_ms = t_sim.as_secs_f64() * 1000.0;
    let sim_secs = t_sim.as_secs_f64();
    let steps_per_sec = if sim_secs > 0.0 { steps_done as f64 / sim_secs } else { 0.0 };

    // Get wave entries as a proxy for events
    let wave_entries = session.wave_count();
    let _ = session.release("clk");
    drop(session);
    qiming_lib::ffi::shutdown();

    println!("  Simulation:    {:.1} ms", sim_ms);
    println!("  Steps done:    {}", steps_done);
    println!("  Wave entries:  {}", wave_entries);
    println!("  Steps/sec:     {:.0}", steps_per_sec);
    println!("  Wave entries/sec: {:.0}", if sim_secs > 0.0 { wave_entries as f64 / sim_secs } else { 0.0 });

    let baseline = serde_json::json!({
        "benchmark": "4-bit counter (force clock + step_delta)",
        "host": "windows",
        "timestamp": chrono_now(),
        "delta_steps": STEPS,
        "steps_done": steps_done,
        "compile_ms": format!("{:.1}", t_compile.as_secs_f64() * 1000.0),
        "elaborate_ms": format!("{:.1}", t_elab.as_secs_f64() * 1000.0),
        "simulation_ms": format!("{:.1}", sim_ms),
        "wave_entries": wave_entries,
        "steps_per_sec": format!("{:.0}", steps_per_sec),
    });

    if let Some(path) = save_path {
        let json = serde_json::to_string_pretty(&baseline)
            .map_err(|e| format!("JSON serialize: {}", e))?;
        fs::write(&path, &json)
            .map_err(|e| format!("write '{}': {}", path, e))?;
        println!("  Baseline saved: {}", path);
    }

    Ok(())
}

/// Get current timestamp as ISO string for baseline records.
fn chrono_now() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs();
    let days = secs / 86400;
    let time_secs = secs % 86400;
    let hours = time_secs / 3600;
    let mins = (time_secs % 3600) / 60;
    let secs_remain = time_secs % 60;

    let mut y = 1970i64;
    let mut remaining = days as i64;
    loop {
        let days_in_year = if is_leap(y) { 366 } else { 365 };
        if remaining < days_in_year { break; }
        remaining -= days_in_year;
        y += 1;
    }
    let month_days = if is_leap(y) {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut m = 0;
    for days_in_m in month_days {
        if remaining < days_in_m { break; }
        remaining -= days_in_m;
        m += 1;
    }
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
        y, m + 1, remaining + 1, hours, mins, secs_remain)
}

fn is_leap(year: i64) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

fn cmd_mcp(args: &[String]) -> Result<(), String> {
    qiming_lib::ffi::init();

    let sessions = std::sync::Arc::new(qiming_lib::session::SessionManager::new());

    if let Some(pos) = args.iter().position(|a| a == "--tcp") {
        let addr = args.get(pos + 1)
            .ok_or_else(|| "--tcp requires an address argument (e.g. 0.0.0.0:9876)")?
            .clone();
        eprintln!("MCP TCP server starting on {}", addr);
        qiming_lib::mcp::server::run_tcp(&addr, sessions)
            .map_err(|e| format!("MCP TCP error: {}", e))?;
    } else {
        eprintln!("MCP server started on stdin/stdout");
        qiming_lib::mcp::server::run(&sessions);
    }

    qiming_lib::ffi::shutdown();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::load_hex;

    /// Extract word `i` (0-based) from the 32768-bit MSB-first imem string.
    /// Word i bit b lives at string char index 32767 - (i*32 + b).
    fn word_at(s: &str, i: usize) -> u32 {
        let base = 32767 - i * 32;
        let mut val: u32 = 0;
        for b in 0..32 {
            if s.as_bytes()[base - b] == b'1' {
                val |= 1 << b;
            }
        }
        val
    }

    #[test]
    fn load_hex_layout_and_padding() {
        let path = std::env::temp_dir().join("qsim_load_hex_test.hex");
        std::fs::write(&path, "10001137\n# comment\n0x00C000EF\n\n").unwrap();
        let s = load_hex(path.to_str().unwrap()).unwrap();
        assert_eq!(s.len(), 32768, "full 1024x32 imem string");
        assert_eq!(word_at(&s, 0), 0x1000_1137, "word 0");
        assert_eq!(word_at(&s, 1), 0x00C0_00EF, "word 1 (0x prefix + comment skip)");
        // Beyond the program: padded with EBREAK.
        assert_eq!(word_at(&s, 2), 0x0010_0073, "pad word 2 with EBREAK");
        assert_eq!(word_at(&s, 1023), 0x0010_0073, "pad last word with EBREAK");
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn load_hex_rejects_bad_lines() {
        let path = std::env::temp_dir().join("qsim_load_hex_bad.hex");
        std::fs::write(&path, "10001137\nZZZZ\n").unwrap();
        assert!(load_hex(path.to_str().unwrap()).is_err(), "bad hex must error");
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn load_hex_empty_program_pads_fully() {
        let path = std::env::temp_dir().join("qsim_load_hex_empty.hex");
        std::fs::write(&path, "# only a comment\n").unwrap();
        let s = load_hex(path.to_str().unwrap()).unwrap();
        assert_eq!(s.len(), 32768);
        assert_eq!(word_at(&s, 0), 0x0010_0073, "EBREAK everywhere");
        std::fs::remove_file(&path).ok();
    }
}
