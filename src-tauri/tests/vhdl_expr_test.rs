use std::io::Write;
use serde_json::json;
use qiming_lib::mcp::handler::handle_request;
use qiming_lib::session::SessionManager;

fn sigs(sessions: &SessionManager, src: &str) -> i32 {
    let resp = handle_request(sessions, "compile", 1, &Some(json!({"source": src})));
    if resp.error.is_some() { return -1; }
    let r = resp.result.unwrap();
    if !r["success"].as_bool().unwrap_or(false) { return -2; }
    let sid = r["session_id"].as_str().unwrap().to_string();
    let resp2 = handle_request(sessions, "elaborate", 2, &Some(json!({"session_id": sid})));
    if resp2.error.is_some() || !resp2.result.unwrap()["success"].as_bool().unwrap_or(false) { return -3; }
    sessions.with_session(&sid, |s| Ok(s.signal_count())).unwrap()
}

fn t(label: &str, sessions: &SessionManager, src: &str) {
    let n = sigs(sessions, src);
    println!("  {}: sigs={}", label, n);
    std::io::stdout().flush().ok();
}

#[test]
fn test_exact_init() {
    let sessions = SessionManager::new();

    // Reproduce the working test exactly
    t("working 1 port", &sessions, "entity e is port(a: in bit_vector(3 downto 0)); end entity;
architecture behav of e is
  signal s : bit_vector(3 downto 0) := (others => '0');
begin
  process(a) is begin
    for i in 0 to 3 loop
      if a(i) = '1' then s(i) <= '1'; end if;
    end loop;
  end process;
end architecture;");

    // Same entity but WITHOUT process
    t("1 port no process", &sessions, "entity e is port(a: in bit_vector(3 downto 0)); end entity;
architecture behav of e is
  signal s : bit_vector(3 downto 0) := (others => '0');
begin
  s <= a;
end architecture;");

    // Same entity without init
    t("1 port no init", &sessions, "entity e is port(a: in bit_vector(3 downto 0)); end entity;
architecture behav of e is
  signal s : bit_vector(3 downto 0);
begin
  s <= a;
end architecture;");

    // 2-port entity with init, matching body
    t("2 port init match", &sessions, "entity e is port(a: in bit_vector(3 downto 0); b: out bit_vector(3 downto 0)); end entity;
architecture behav of e is
  signal s : bit_vector(3 downto 0) := (others => '0');
begin
  b <= s;
end architecture;");

    // 2-port entity, init, matching body, NO process
    t("2 port init match2", &sessions, "entity e is port(clk: in bit; rst: in bit); end entity;
architecture behav of e is
  signal s : bit := '0';
begin
  s <= clk;
end architecture;");

    // What about stmt body before init?
    t("no init 2 port", &sessions, "entity e is port(clk: in bit; rst: in bit); end entity;
architecture behav of e is
  signal s : bit;
begin
  s <= clk;
end architecture;");

    // 2-port, no init, but with integer signal
    t("int no init 2 port", &sessions, "entity e is port(clk: in bit; rst: in bit); end entity;
architecture behav of e is
  signal s : integer;
begin
  s <= 0;
end architecture;");

    // 2-port, integer init
    t("int init 2 port", &sessions, "entity e is port(clk: in bit; rst: in bit); end entity;
architecture behav of e is
  signal s : integer := 0;
begin
  s <= 0;
end architecture;");
}
