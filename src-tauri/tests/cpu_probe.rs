use std::io::Write;

fn probe(label: &str, src: &str) {
    use qiming_lib::mcp::handler::handle_request;
    use qiming_lib::session::SessionManager;
    use serde_json::json;

    let sessions = SessionManager::new();
    let resp = handle_request(&sessions, "compile", 1, &Some(json!({"source": src})));
    if resp.error.is_some() {
        println!("  {}: COMPILE ERROR {:?}", label, resp.error);
        return;
    }
    let r = resp.result.unwrap();
    if !r["success"].as_bool().unwrap_or(false) {
        if let Some(diag) = r["diagnostics"].as_str() {
            println!("  {}: DIAG {}", label, diag);
        } else {
            println!("  {}: COMPILE FAILED", label);
        }
        return;
    }
    let sid = r["session_id"].as_str().unwrap().to_string();
    let resp2 = handle_request(&sessions, "elaborate", 2, &Some(json!({"session_id": sid})));
    if resp2.error.is_some() || !resp2.result.unwrap()["success"].as_bool().unwrap_or(false) {
        println!("  {}: ELAB FAILED", label);
        return;
    }
    let n = sessions.with_session(&sid, |s| Ok(s.signal_count())).unwrap();
    println!("  {}: sigs={}", label, n);
    std::io::stdout().flush().ok();
}

#[test]
fn test_probe_cpu_arch() {
    let base = "\
entity rv8_cpu is
  port (
    clk : in bit;
    rst : in bit;
    init_en : in bit;
    init_addr : in bit_vector(7 downto 0);
    init_data : in bit_vector(15 downto 0);
    halted : out bit;
    pc : out bit_vector(15 downto 0);
    instruction : out bit_vector(15 downto 0);
    reg_x1 : out bit_vector(7 downto 0);
    reg_x2 : out bit_vector(7 downto 0);
    reg_x3 : out bit_vector(7 downto 0);
    reg_x4 : out bit_vector(7 downto 0);
    reg_x5 : out bit_vector(7 downto 0);
    reg_x6 : out bit_vector(7 downto 0);
    reg_x7 : out bit_vector(7 downto 0);
    alu_result : out bit_vector(7 downto 0);
    dmem_addr : out bit_vector(7 downto 0);
    dmem_rdata : out bit_vector(7 downto 0);
    dmem_wdata : out bit_vector(7 downto 0);
    dmem_we : out bit
  );
end entity;

architecture rtl of rv8_cpu is
  signal x1_reg : bit_vector(7 downto 0);
  signal x2_reg : bit_vector(7 downto 0);
  signal x3_reg : bit_vector(7 downto 0);
  signal x4_reg : bit_vector(7 downto 0);
  signal x5_reg : bit_vector(7 downto 0);
  signal x6_reg : bit_vector(7 downto 0);
  signal x7_reg : bit_vector(7 downto 0);
  signal pc_reg : bit_vector(15 downto 0);
  signal halted_reg : bit;
  signal instr : bit_vector(15 downto 0);
  signal opcode : bit_vector(15 downto 0);
  signal rf_rd : bit_vector(15 downto 0);
  signal rf_rs1 : bit_vector(15 downto 0);
  signal rf_rs2 : bit_vector(15 downto 0);
  signal rs1_val : bit_vector(7 downto 0);
  signal rs2_val : bit_vector(7 downto 0);
  signal sext_imm : bit_vector(7 downto 0);
  signal sext_imm16 : bit_vector(15 downto 0);
  signal alu_op : bit_vector(2 downto 0);
  signal alu_b : bit_vector(7 downto 0);
  signal alu_result_reg : bit_vector(7 downto 0);
  signal reg_we : bit;
  signal mem_write : bit;
  signal wb_sel : bit_vector(1 downto 0);
  signal branch_taken : bit;
  signal beq_taken : bit;
  signal bne_taken : bit;
  signal blt_taken : bit;
  signal write_data : bit_vector(7 downto 0);
  signal dmem_rdata_reg : bit_vector(7 downto 0);
  signal pc_next : bit_vector(15 downto 0);
  signal imem : bit_vector(4095 downto 0);
begin\n";

    let concat = format!("{}end architecture;", base);
    probe("signals only", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

end architecture;", base);
    probe("+ concurrent assigns", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(pc_reg, init_en, init_data, imem) is
  begin
    if init_en = '1' then
      instr <= init_data;
    else
      instr <= (imem srl (pc_reg * \"10000\")) and \"1111111111111111\";
    end if;
  end process;

end architecture;", base);
    probe("+ ifetch process", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(instr) is
  begin
    case opcode is
      when \"0000000000000101\" | \"0000000000001000\" |
           \"0000000000001001\" | \"0000000000001010\" =>
        rf_rs1 <= (instr sll 4) srl 13;
        rf_rs2 <= (instr sll 7) srl 13;
      when \"0000000000000100\" =>
        rf_rd <= (instr sll 4) srl 13;
        rf_rs1 <= (instr sll 7) srl 13;
      when others =>
        rf_rd <= (instr sll 4) srl 13;
        rf_rs1 <= (instr sll 7) srl 13;
        rf_rs2 <= (instr sll 10) srl 13;
    end case;
  end process;

end architecture;", base);
    probe("+ reg addr decode", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(instr) is
    variable imm6 : bit_vector(15 downto 0);
  begin
    imm6 := (instr sll 10) srl 10;
    if ((imm6 srl 5) and \"0000000000000001\") = \"0000000000000001\" then
      sext_imm <= (imm6 or \"0000000011000000\");
      sext_imm16 <= (imm6 or \"1111111111000000\");
    else
      sext_imm <= imm6;
      sext_imm16 <= imm6;
    end if;
  end process;

end architecture;", base);
    probe("+ sign ext", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(opcode, instr)
  begin
    case opcode is
      when \"0000000000000001\" =>
        case (instr sll 13) srl 13 is
          when \"1000000000000000\" => alu_op <= \"001\";
          when \"0100000000000000\" => alu_op <= \"101\";
          when \"1100000000000000\" => alu_op <= \"110\";
          when others => alu_op <= \"000\";
        end case;
      when \"0000000000000010\" | \"0000000000000100\" |
           \"0000000000000101\" | \"0000000000001100\" |
           \"0000000000001101\" =>
        alu_op <= \"000\";
      when \"0000000000000011\" =>
        alu_op <= \"00\" & (instr sll 13) srl 13(0);
      when \"0000000000000110\" => alu_op <= \"010\";
      when \"0000000000000111\" => alu_op <= \"011\";
      when \"0000000000001110\" => alu_op <= \"100\";
      when others => alu_op <= \"000\";
    end case;
  end process;

end architecture;", base);
    probe("+ ALU op decode", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(opcode, sext_imm, rs2_val)
  begin
    case opcode is
      when \"0000000000000010\" | \"0000000000000011\" |
           \"0000000000000100\" | \"0000000000000110\" |
           \"0000000000000111\" | \"0000000000001110\" =>
        alu_b <= sext_imm;
      when others =>
        alu_b <= rs2_val;
    end case;
  end process;

end architecture;", base);
    probe("+ ALU B mux", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(alu_op, rs1_val, alu_b)
    variable shift_amt : integer;
    variable m : integer;
  begin
    case alu_op is
      when \"000\" => alu_result_reg <= rs1_val + alu_b;
      when \"001\" => alu_result_reg <= rs1_val - alu_b;
      when \"010\" => alu_result_reg <= rs1_val and alu_b;
      when \"011\" => alu_result_reg <= rs1_val or alu_b;
      when \"100\" => alu_result_reg <= rs1_val xor alu_b;
      when others => alu_result_reg <= rs1_val;
    end case;
  end process;

end architecture;", base);
    probe("+ ALU compute", &concat);

    let concat = format!("{}opcode <= instr srl 12;
  instruction <= instr;

  process(opcode, rs1_val, rs2_val)
  begin
    if opcode = \"0000000000001000\" and rs1_val = rs2_val then
      beq_taken <= '1';
    else
      beq_taken <= '0';
    end if;
    if opcode = \"0000000000001001\" and rs1_val /= rs2_val then
      bne_taken <= '1';
    else
      bne_taken <= '0';
    end if;
  end process;

end architecture;", base);
    probe("+ branch cmp", &concat);
}
