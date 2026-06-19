// MCP server tests — JSON-RPC parsing and handler logic.

use serde_json::json;
use crate::mcp::types::{JsonRpcRequest, JsonRpcResponse};
use crate::session::SessionManager;

#[test]
fn test_json_rpc_parse_request() {
    let json = r#"{"jsonrpc":"2.0","id":1,"method":"compile","params":{"source":"test"}}"#;
    let req: JsonRpcRequest = serde_json::from_str(json).unwrap();
    assert_eq!(req.jsonrpc, "2.0");
    assert_eq!(req.id, 1);
    assert_eq!(req.method, "compile");
    assert!(req.params.is_some());
}

#[test]
fn test_json_rpc_parse_request_no_params() {
    let json = r#"{"jsonrpc":"2.0","id":2,"method":"get_sessions"}"#;
    let req: JsonRpcRequest = serde_json::from_str(json).unwrap();
    assert_eq!(req.method, "get_sessions");
    assert!(req.params.is_none());
}

#[test]
fn test_json_rpc_response_serialize() {
    let resp = JsonRpcResponse::success(1, json!({"success": true}));
    let json = serde_json::to_string(&resp).unwrap();
    assert!(json.contains("\"result\""));
    assert!(json.contains("\"success\""));
    assert!(!json.contains("\"error\""));
}

#[test]
fn test_json_rpc_error_serialize() {
    let resp = JsonRpcResponse::method_not_found(1, "bad_method");
    let json = serde_json::to_string(&resp).unwrap();
    assert!(json.contains("\"error\""));
    assert!(json.contains("-32601") || json.contains("method not found"));
    assert!(!json.contains("\"result\""));
}

#[test]
fn test_json_rpc_parse_error() {
    let resp = JsonRpcResponse::parse_error(0, "bad json");
    assert_eq!(resp.error.as_ref().unwrap().code, -32700);
}

#[test]
fn test_handler_get_sessions() {
    let sessions = SessionManager::new();
    // get_sessions returns empty list for no sessions
    let resp = crate::mcp::handler::handle_request(&sessions, "get_sessions", 1, &None);
    assert!(resp.error.is_none());
    let result = resp.result.unwrap();
    assert_eq!(result["count"].as_i64().unwrap(), 0);
}

#[test]
fn test_handler_compile_and_eval() {
    // This is an integration test that uses the C library.
    let sessions = SessionManager::new();

    // Compile
    let params = json!({"source": "module test(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    assert!(resp.error.is_none());
    let result = resp.result.unwrap();
    assert_eq!(result["success"].as_bool().unwrap_or(false), true);

    let session_id = result["session_id"].as_str().unwrap().to_string();

    // Elaborate
    let params = json!({"session_id": session_id});
    let resp = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));
    assert!(resp.error.is_none());
    assert_eq!(resp.result.unwrap()["success"].as_bool().unwrap_or(false), true);

    // Eval signal
    let params = json!({"session_id": session_id, "signal": "a"});
    let resp = crate::mcp::handler::handle_request(&sessions, "eval", 3, &Some(params));
    assert!(resp.error.is_none());
    let eval_result = resp.result.unwrap();
    assert_eq!(eval_result["signal"].as_str().unwrap(), "a");
    // a should be X initially (uninitialized)
    assert_eq!(eval_result["value"].as_str().unwrap(), "X");
}

#[test]
fn test_handler_get_signals() {
    let sessions = SessionManager::new();
    let params = json!({"source": "module t(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    assert!(resp.error.is_none());
    let sid = resp.result.unwrap()["session_id"].as_str().unwrap().to_string();

    let params = json!({"session_id": sid});
    let resp = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));
    assert!(resp.error.is_none());

    let params = json!({"session_id": sid});
    let resp = crate::mcp::handler::handle_request(&sessions, "get_signals", 3, &Some(params));
    assert!(resp.error.is_none());
    let r = resp.result.unwrap();
    assert!(r["signal_count"].as_i64().unwrap_or(0) > 0);
    let signals = r["signals"].as_array().unwrap();
    assert!(signals.iter().any(|s| s["name"] == "a"));
    assert!(signals.iter().any(|s| s["name"] == "x"));
}

#[test]
fn test_handler_list_designs() {
    let sessions = SessionManager::new();
    let params = json!({"source": "module t(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    assert!(resp.error.is_none());
    let sid = resp.result.unwrap()["session_id"].as_str().unwrap().to_string();

    let params = json!({"session_id": sid});
    let resp = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));
    assert!(resp.error.is_none());

    let params = json!({"session_id": sid});
    let resp = crate::mcp::handler::handle_request(&sessions, "list_designs", 3, &Some(params));
    assert!(resp.error.is_none());
    let r = resp.result.unwrap();
    assert!(r["designs"].is_object());
}

#[test]
fn test_handler_method_not_found() {
    let sessions = SessionManager::new();
    let resp = crate::mcp::handler::handle_request(&sessions, "nonexistent", 1, &None);
    assert!(resp.error.is_some());
    assert_eq!(resp.error.unwrap().code, -32601);
}

#[test]
fn test_handler_invalid_params() {
    let sessions = SessionManager::new();
    // Missing session_id for elaborate
    let params = json!({"wrong_key": "value"});
    let resp = crate::mcp::handler::handle_request(&sessions, "elaborate", 1, &Some(params));
    assert!(resp.error.is_some());
}

#[test]
fn test_handler_add_breakpoint() {
    let sessions = SessionManager::new();

    // Compile
    let params = json!({"source": "module test(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    assert!(resp.error.is_none());
    let result = resp.result.unwrap();
    assert_eq!(result["success"].as_bool().unwrap_or(false), true);
    let session_id = result["session_id"].as_str().unwrap().to_string();

    // Elaborate
    let params = json!({"session_id": session_id});
    let resp = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));
    assert!(resp.error.is_none());
    assert_eq!(resp.result.unwrap()["success"].as_bool().unwrap_or(false), true);

    // Add breakpoint
    let params = json!({"session_id": session_id, "file": "test.v", "line": 1});
    let resp = crate::mcp::handler::handle_request(&sessions, "add_breakpoint", 3, &Some(params));
    assert!(resp.error.is_none());
    assert_eq!(resp.result.unwrap()["success"].as_bool().unwrap_or(false), true);

    // List breakpoints
    let params = json!({"session_id": session_id});
    let resp = crate::mcp::handler::handle_request(&sessions, "list_breakpoints", 4, &Some(params));
    assert!(resp.error.is_none());
    let list_result = resp.result.unwrap();
    assert_eq!(list_result["count"].as_i64().unwrap(), 1);
}

#[test]
fn test_handler_get_coverage() {
    let sessions = SessionManager::new();

    // Compile
    let params = json!({"source": "module test(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    let result = resp.result.unwrap();
    let session_id = result["session_id"].as_str().unwrap().to_string();

    // Elaborate
    let params = json!({"session_id": session_id});
    let _ = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));

    // Get coverage — should respond without error even if no coverage data
    let params = json!({"session_id": session_id});
    let resp = crate::mcp::handler::handle_request(&sessions, "get_coverage", 3, &Some(params));
    assert!(resp.error.is_none());
    let cov_result = resp.result.unwrap();
    // Response must have these fields
    assert!(cov_result.get("count").is_some());
    assert!(cov_result.get("percent").is_some());
}

fn try_compile_and_count(sessions: &SessionManager, label: &str, src: &str) {
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1,
        &Some(json!({"source": src})));
    let result = resp.result.unwrap();
    let success = result["success"].as_bool().unwrap_or(false);
    if !success { println!("  {}: COMPILE FAILED", label); return; }
    let sid = result["session_id"].as_str().unwrap().to_string();
    let resp2 = crate::mcp::handler::handle_request(&sessions, "elaborate", 2,
        &Some(json!({"session_id": sid})));
    let elab_ok = resp2.result.unwrap()["success"].as_bool().unwrap_or(false);
    if !elab_ok { println!("  {}: ELAB FAILED", label); return; }
    let count = sessions.with_session(&sid, |s| Ok(s.signal_count())).unwrap();
    println!("  {}: sigs={}", label, count);
}

#[test]
fn test_probe_bisect_cpu() {
    let sessions = SessionManager::new();

    // Entity only
    try_compile_and_count(&sessions, "entity only", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
begin
end architecture;");

    // Entity + signal decls only
    try_compile_and_count(&sessions, "+ signals", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit;
begin
end architecture;");

    // + process with clk = '1'
    try_compile_and_count(&sessions, "+ process", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit;
begin
  process(clk) is begin
    s <= '1';
  end process;
end architecture;");

    // + integer range
    try_compile_and_count(&sessions, "+ int range", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal i : integer range 0 to 7;
begin
  process(clk) is begin
    i <= 1;
  end process;
end architecture;");

    // big bit_vector
    try_compile_and_count(&sessions, "+ big bv", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal imem : bit_vector(4095 downto 0);
begin
  process(clk) is begin
    imem(0) <= '1';
  end process;
end architecture;");

    // process with if/elsif
    try_compile_and_count(&sessions, "+ if/elsif", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit;
begin
  process(clk, rst) is begin
    if rst = '1' then s <= '0';
    elsif clk = '1' then s <= '1';
    end if;
  end process;
end architecture;");

    // process with for loop
    try_compile_and_count(&sessions, "+ for loop", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal regs : bit_vector(63 downto 0);
begin
  process(rst) is begin
    if rst = '1' then
      for i in 0 to 63 loop
        regs(i) <= '0';
      end loop;
    end if;
  end process;
end architecture;");

    // concurrent signal assignment
    try_compile_and_count(&sessions, "+ conc assign", "\
entity e is
  port(a: in bit; b: out bit);
end entity;
architecture rtl of e is
begin
  b <= a;
end architecture;");

    // bv(7 downto 0) small
    try_compile_and_count(&sessions, "bv8", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(7 downto 0);
begin
end architecture;");

    // bv(15 downto 0)
    try_compile_and_count(&sessions, "bv16", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(15 downto 0);
begin
end architecture;");

    // bv(63 downto 0)
    try_compile_and_count(&sessions, "bv64", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(63 downto 0);
begin
end architecture;");

    // bv(255 downto 0)
    try_compile_and_count(&sessions, "bv256", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(255 downto 0);
begin
end architecture;");

    // bv(4095 downto 0) - same as imem
    try_compile_and_count(&sessions, "bv4096", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(4095 downto 0);
begin
end architecture;");

    // for loop with bit
    try_compile_and_count(&sessions, "for bit", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
begin
  process(rst) is begin
    for i in 0 to 1 loop
      clk <= '0';
    end loop;
  end process;
end architecture;");

    // for loop with integer variable
    try_compile_and_count(&sessions, "for var", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
begin
  process(rst) is
    variable v : integer;
  begin
    for i in 0 to 1 loop
      v := 1;
    end loop;
  end process;
end architecture;");

    // signal with bit_vector(63 downto 0) and for loop to write it
    try_compile_and_count(&sessions, "bv64+for", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal regs : bit_vector(63 downto 0);
begin
  process(rst) is begin
    for i in 0 to 63 loop
      regs(i) <= '0';
    end loop;
  end process;
end architecture;");

    // for loop with signal assignment to internal signal
    try_compile_and_count(&sessions, "for int sig", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit;
begin
  process(rst) is begin
    for i in 0 to 1 loop
      s <= '0';
    end loop;
  end process;
end architecture;");

    // for loop with signal assignment to internal bv
    try_compile_and_count(&sessions, "for bv sig", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(3 downto 0);
begin
  process(rst) is begin
    for i in 0 to 3 loop
      s(i) <= '0';
    end loop;
  end process;
end architecture;");

    // indexed signal assignment (no for loop)
    try_compile_and_count(&sessions, "idx assign", "\
entity e is
  port(clk: in bit; rst: in bit);
end entity;
architecture rtl of e is
  signal s : bit_vector(3 downto 0);
begin
  process(rst) is begin
    s(0) <= '1';
  end process;
end architecture;");

    // indexed READ on RHS (concurrent)
    try_compile_and_count(&sessions, "idx read conc", "\
entity e is
  port(a: in bit_vector(3 downto 0); b: out bit_vector(3 downto 0));
end entity;
architecture rtl of e is
begin
  b <= a(0);
end architecture;");

    // indexed read in expr
    try_compile_and_count(&sessions, "idx read if", "\
entity e is
  port(a: in bit_vector(3 downto 0); b: out bit);
end entity;
architecture rtl of e is
begin
  process(a) is begin
    if a(0) = '1' then b <= '1'; end if;
  end process;
end architecture;");

    // slice read: b <= a(3 downto 0)
    try_compile_and_count(&sessions, "slice read", "\
entity e is
  port(a: in bit_vector(7 downto 0); b: out bit_vector(3 downto 0));
end entity;
architecture rtl of e is
begin
  b <= a(3 downto 0);
end architecture;");
}

#[test]
fn test_handler_export_vcd() {
    let sessions = SessionManager::new();

    // Compile
    let params = json!({"source": "module test(input a, output reg x); always @(*) x = a; endmodule;"});
    let resp = crate::mcp::handler::handle_request(&sessions, "compile", 1, &Some(params));
    let result = resp.result.unwrap();
    let session_id = result["session_id"].as_str().unwrap().to_string();
    let sid = session_id.clone();

    // Elaborate
    let params = json!({"session_id": session_id});
    let _ = crate::mcp::handler::handle_request(&sessions, "elaborate", 2, &Some(params));

    // Simulate to generate wave data
    let params = json!({"session_id": sid.clone()});
    let _ = crate::mcp::handler::handle_request(&sessions, "simulate", 3, &Some(params));

    // Export VCD
    let vcd_path = std::env::temp_dir().join("test_mcp_export.vcd");
    let vcd_str = vcd_path.to_string_lossy().to_string();
    let params = json!({"session_id": sid, "path": vcd_str});
    let resp = crate::mcp::handler::handle_request(&sessions, "export_vcd", 4, &Some(params));
    assert!(resp.error.is_none());
    let export_result = resp.result.unwrap();
    assert_eq!(export_result["success"].as_bool().unwrap_or(false), true);

    // Clean up
    let _ = std::fs::remove_file(&vcd_path);
}
