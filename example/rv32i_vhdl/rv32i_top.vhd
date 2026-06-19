entity rv32i_top is
  port (
    clk : in bit;
    rst : in bit;
    imem_port : in bit_vector(32767 downto 0);
    halted : out bit;
    pc : out bit_vector(31 downto 0);
    instruction : out bit_vector(31 downto 0);
    reg_x1 : out bit_vector(31 downto 0);
    reg_x10 : out bit_vector(31 downto 0);
    alu_result : out bit_vector(31 downto 0);
    dmem_addr : out bit_vector(31 downto 0);
    dmem_rdata : out bit_vector(31 downto 0);
    dmem_wdata : out bit_vector(31 downto 0);
    dmem_we : out bit;
    uart_tx_valid : out bit;
    uart_tx_data : out bit_vector(7 downto 0);
    debug_ex_branch_taken : out bit;
    debug_idex_branch : out bit;
    debug_idex_funct3 : out bit_vector(2 downto 0);
    debug_forward_rs1_val : out bit_vector(31 downto 0);
    debug_forward_rs2_val : out bit_vector(31 downto 0);
    debug_idex_imm : out bit_vector(31 downto 0);
    debug_branch_taken : out bit;
    debug_flush_ifid : out bit;
    debug_load_use_stall : out bit;
    debug_ex_branch_target : out bit_vector(31 downto 0);
    debug_idex_pc : out bit_vector(31 downto 0);
    debug_idex_rd : out bit_vector(4 downto 0);
    debug_idex_rs1 : out bit_vector(4 downto 0);
    debug_idex_rs2 : out bit_vector(4 downto 0);
    debug_alu_a : out bit_vector(31 downto 0);
    debug_alu_b : out bit_vector(31 downto 0);
    debug_ex_alu_final : out bit_vector(31 downto 0);
    debug_exmem_write_data : out bit_vector(31 downto 0);
    debug_idex_imm_real : out bit_vector(31 downto 0)
  );
end entity;

architecture rtl of rv32i_top is

  -- IMEM: 1024 words x 32 bits = 32768 bits, flat bit_vector

  -- DMEM: 256 words x 32 bits = 8192 bits, flat bit_vector
  signal dmem : bit_vector(8191 downto 0);

  -- Register file: x1-x31 (x0 hardwired to zero)
  signal x1, x2, x3, x4, x5, x6, x7, x8 : bit_vector(31 downto 0);
  signal x9, x10, x11, x12, x13, x14, x15, x16 : bit_vector(31 downto 0);
  signal x17, x18, x19, x20, x21, x22, x23, x24 : bit_vector(31 downto 0);
  signal x25, x26, x27, x28, x29, x30, x31 : bit_vector(31 downto 0);

  -- PC and halt state
  signal pc_reg : bit_vector(31 downto 0);
  signal halted_reg : bit;

  -- UART
  signal uart_tx_valid_reg : bit;
  signal uart_tx_data_reg : bit_vector(7 downto 0);

  -- CSR
  signal csr_mtvec   : bit_vector(31 downto 0);
  signal csr_mepc    : bit_vector(31 downto 0);
  signal csr_mcause  : bit_vector(31 downto 0);
  signal csr_mie     : bit_vector(31 downto 0);
  signal csr_mstatus : bit_vector(31 downto 0);

  -- Timer
  signal timer_compare : bit_vector(31 downto 0);
  signal timer_counter : bit_vector(31 downto 0);

  -- IF stage
  signal if_instr : bit_vector(31 downto 0);
  signal if_pc_plus_4 : bit_vector(31 downto 0);

  -- IF/ID pipeline register
  signal ifid_instr : bit_vector(31 downto 0);
  signal ifid_pc_plus_4 : bit_vector(31 downto 0);

  -- ID stage
  signal id_opcode : bit_vector(6 downto 0);
  signal id_rd, id_rs1, id_rs2 : bit_vector(4 downto 0);
  signal id_funct3 : bit_vector(2 downto 0);
  signal id_funct7 : bit_vector(6 downto 0);
  signal imm_i : bit_vector(31 downto 0);
  signal imm_s : bit_vector(31 downto 0);
  signal imm_b : bit_vector(31 downto 0);
  signal imm_u : bit_vector(31 downto 0);
  signal imm_j : bit_vector(31 downto 0);
  signal imm_id : bit_vector(31 downto 0);
  signal rs1_val_id : bit_vector(31 downto 0);
  signal rs2_val_id : bit_vector(31 downto 0);
  signal timer_match : bit;
  signal irq_pending : bit;
  signal is_mret : bit;
  signal csr_read_data : bit_vector(31 downto 0);
  signal csr_rs1_fwd : bit_vector(31 downto 0);
  signal csr_write_val : bit_vector(31 downto 0);
  signal id_alu_src_a : bit;
  signal id_alu_src_b : bit_vector(1 downto 0);
  signal id_mem_write : bit;
  signal id_mem_to_reg : bit_vector(1 downto 0);
  signal id_mem_read : bit;
  signal id_branch : bit;
  signal id_alu_op : bit_vector(1 downto 0);
  signal id_jal_jalr : bit_vector(1 downto 0);
  signal id_reg_write : bit;
  signal id_alu_control : bit_vector(4 downto 0);

  -- ID/EX pipeline register
  signal idex_alu_src_a : bit;
  signal idex_alu_src_b : bit_vector(1 downto 0);
  signal idex_mem_write : bit;
  signal idex_mem_to_reg : bit_vector(1 downto 0);
  signal idex_mem_read : bit;
  signal idex_reg_write : bit;
  signal idex_alu_control : bit_vector(4 downto 0);
  signal idex_jal_jalr : bit_vector(1 downto 0);
  signal idex_branch : bit;
  signal idex_rs1, idex_rs2, idex_rd : bit_vector(4 downto 0);
  signal idex_rs1_val, idex_rs2_val : bit_vector(31 downto 0);
  signal idex_imm, idex_imm_b, idex_imm_j, idex_imm_u, idex_imm_i : bit_vector(31 downto 0);
  signal idex_pc_plus_4, idex_pc : bit_vector(31 downto 0);
  signal idex_funct3 : bit_vector(2 downto 0);
  signal idex_csr_rdata : bit_vector(31 downto 0);

  -- EX stage
  signal exmem_match_a, exmem_match_b : bit;
  signal memwb_match_a, memwb_match_b : bit;
  signal forward_rs1_val, forward_rs2_val : bit_vector(31 downto 0);
  signal alu_a, alu_b : bit_vector(31 downto 0);
  signal alu_result_ex : bit_vector(31 downto 0);
  signal alu_result_full : bit_vector(31 downto 0);
  signal ex_alu_final : bit_vector(31 downto 0);
  signal branch_taken : bit;
  signal ex_branch_target : bit_vector(31 downto 0);
  signal ex_jal_target : bit_vector(31 downto 0);
  signal ex_jalr_target : bit_vector(31 downto 0);

  -- EX/MEM pipeline register
  signal exmem_mem_write : bit;
  signal exmem_mem_read : bit;
  signal exmem_mem_to_reg : bit_vector(1 downto 0);
  signal exmem_reg_write : bit;
  signal exmem_wb_rd : bit_vector(4 downto 0);
  signal exmem_alu_result : bit_vector(31 downto 0);
  signal exmem_rs2_val : bit_vector(31 downto 0);
  signal exmem_pc_plus_4 : bit_vector(31 downto 0);
  signal exmem_funct3 : bit_vector(2 downto 0);
  signal exmem_imm_u : bit_vector(31 downto 0);

  -- MEM stage
  signal dmem_waddr : bit_vector(7 downto 0);
  signal dmem_cur_word : bit_vector(31 downto 0);
  signal dmem_wdata_merged : bit_vector(31 downto 0);
  signal mem_rdata : bit_vector(31 downto 0);
  signal dmem_rdata_comb : bit_vector(31 downto 0);

  -- MEM/WB pipeline register
  signal memwb_reg_write : bit;
  signal memwb_mem_to_reg : bit_vector(1 downto 0);
  signal memwb_mem_read : bit;
  signal memwb_wb_rd : bit_vector(4 downto 0);
  signal memwb_alu_result, memwb_mem_rdata : bit_vector(31 downto 0);
  signal memwb_pc_plus_4 : bit_vector(31 downto 0);
  signal memwb_imm_u : bit_vector(31 downto 0);

  -- WB stage
  signal memwb_write_data : bit_vector(31 downto 0);
  signal exmem_write_data : bit_vector(31 downto 0);

  -- Hazard
  signal load_use_stall : bit;
  signal ex_is_ebreak : bit;
  signal flush_ifid : bit;
  signal bubble_idex : bit;

begin

  -- ================================================================
  -- IF STAGE
  -- ================================================================
  if_pc_plus_4 <= pc_reg + X"00000004";

  -- IMEM read: extract 32-bit word at index (pc_reg >> 2) & 0x3FF
  process(pc_reg, imem_port) is
    variable idx : integer;
    variable shifted : bit_vector(32767 downto 0);
    variable pc_div4_full : bit_vector(31 downto 0);
    variable pc_div4 : bit_vector(9 downto 0);
  begin
    pc_div4_full := pc_reg srl 2;
    pc_div4 := pc_div4_full(9 downto 0);
    idx := 0;
    if pc_div4(0) = '1' then idx := idx + 1; end if;
    if pc_div4(1) = '1' then idx := idx + 2; end if;
    if pc_div4(2) = '1' then idx := idx + 4; end if;
    if pc_div4(3) = '1' then idx := idx + 8; end if;
    if pc_div4(4) = '1' then idx := idx + 16; end if;
    if pc_div4(5) = '1' then idx := idx + 32; end if;
    if pc_div4(6) = '1' then idx := idx + 64; end if;
    if pc_div4(7) = '1' then idx := idx + 128; end if;
    if pc_div4(8) = '1' then idx := idx + 256; end if;
    if pc_div4(9) = '1' then idx := idx + 512; end if;
    shifted := imem_port srl (idx * 32);
    if_instr <= shifted(31 downto 0);
  end process;

  -- ================================================================
  -- ID STAGE
  -- ================================================================
  id_opcode <= ifid_instr(6 downto 0);
  id_rd     <= ifid_instr(11 downto 7);
  id_funct3 <= ifid_instr(14 downto 12);
  id_rs1    <= ifid_instr(19 downto 15);
  id_rs2    <= ifid_instr(24 downto 20);
  id_funct7 <= ifid_instr(31 downto 25);

  -- Immediate generation (process with variables for bit extraction)
  process(ifid_instr) is
    variable raw12 : bit_vector(11 downto 0);
    variable b_raw13 : bit_vector(12 downto 0);
    variable j_raw21 : bit_vector(20 downto 0);
  begin
    -- imm_i: instr[31:20] sign-extended
    raw12 := ifid_instr(31 downto 20);
    if raw12(11) = '1' then
      imm_i <= "11111111111111111111" & raw12;
    else
      imm_i <= "00000000000000000000" & raw12;
    end if;

    -- imm_s: {instr[31:25], instr[11:7]} sign-extended
    raw12 := ifid_instr(31 downto 25) & ifid_instr(11 downto 7);
    if raw12(11) = '1' then
      imm_s <= "11111111111111111111" & raw12;
    else
      imm_s <= "00000000000000000000" & raw12;
    end if;

    -- imm_b: {instr[31], instr[7], instr[30:25], instr[11:8], 0} sign-extended
    b_raw13(12) := ifid_instr(31);
    b_raw13(11) := ifid_instr(7);
    b_raw13(10 downto 5) := ifid_instr(30 downto 25);
    b_raw13(4 downto 1) := ifid_instr(11 downto 8);
    b_raw13(0) := '0';
    if b_raw13(12) = '1' then
      imm_b <= "1111111111111111111" & b_raw13;
    else
      imm_b <= "0000000000000000000" & b_raw13;
    end if;

    -- imm_u: instr[31:12] << 12
    imm_u <= ifid_instr(31 downto 12) & "000000000000";

    -- imm_j: {instr[31], instr[19:12], instr[20], instr[30:21], 0} sign-extended
    j_raw21(20) := ifid_instr(31);
    j_raw21(19 downto 12) := ifid_instr(19 downto 12);
    j_raw21(11) := ifid_instr(20);
    j_raw21(10 downto 1) := ifid_instr(30 downto 21);
    j_raw21(0) := '0';
    if j_raw21(20) = '1' then
      imm_j <= "11111111111" & j_raw21;
    else
      imm_j <= "00000000000" & j_raw21;
    end if;
  end process;

  -- Immediate mux
  process(id_opcode, imm_i, imm_s, imm_b, imm_j, imm_u) is
  begin
    case id_opcode is
      when "0010011" | "0000011" | "1100111" => imm_id <= imm_i;
      when "0100011" => imm_id <= imm_s;
      when "1100011" => imm_id <= imm_b;
      when "1101111" => imm_id <= imm_j;
      when others => imm_id <= imm_u;
    end case;
  end process;

  -- Register file read port 1
  process(id_rs1, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,
          x11, x12, x13, x14, x15, x16, x17, x18, x19, x20,
          x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, x31) is
  begin
    case id_rs1 is
      when "00000" => rs1_val_id <= X"00000000";
      when "00001" => rs1_val_id <= x1;  when "00010" => rs1_val_id <= x2;
      when "00011" => rs1_val_id <= x3;  when "00100" => rs1_val_id <= x4;
      when "00101" => rs1_val_id <= x5;  when "00110" => rs1_val_id <= x6;
      when "00111" => rs1_val_id <= x7;  when "01000" => rs1_val_id <= x8;
      when "01001" => rs1_val_id <= x9;  when "01010" => rs1_val_id <= x10;
      when "01011" => rs1_val_id <= x11; when "01100" => rs1_val_id <= x12;
      when "01101" => rs1_val_id <= x13; when "01110" => rs1_val_id <= x14;
      when "01111" => rs1_val_id <= x15; when "10000" => rs1_val_id <= x16;
      when "10001" => rs1_val_id <= x17; when "10010" => rs1_val_id <= x18;
      when "10011" => rs1_val_id <= x19; when "10100" => rs1_val_id <= x20;
      when "10101" => rs1_val_id <= x21; when "10110" => rs1_val_id <= x22;
      when "10111" => rs1_val_id <= x23; when "11000" => rs1_val_id <= x24;
      when "11001" => rs1_val_id <= x25; when "11010" => rs1_val_id <= x26;
      when "11011" => rs1_val_id <= x27; when "11100" => rs1_val_id <= x28;
      when "11101" => rs1_val_id <= x29; when "11110" => rs1_val_id <= x30;
      when "11111" => rs1_val_id <= x31;
      when others => rs1_val_id <= X"00000000";
    end case;
  end process;

  -- Register file read port 2
  process(id_rs2, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,
          x11, x12, x13, x14, x15, x16, x17, x18, x19, x20,
          x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, x31) is
  begin
    case id_rs2 is
      when "00000" => rs2_val_id <= X"00000000";
      when "00001" => rs2_val_id <= x1;  when "00010" => rs2_val_id <= x2;
      when "00011" => rs2_val_id <= x3;  when "00100" => rs2_val_id <= x4;
      when "00101" => rs2_val_id <= x5;  when "00110" => rs2_val_id <= x6;
      when "00111" => rs2_val_id <= x7;  when "01000" => rs2_val_id <= x8;
      when "01001" => rs2_val_id <= x9;  when "01010" => rs2_val_id <= x10;
      when "01011" => rs2_val_id <= x11; when "01100" => rs2_val_id <= x12;
      when "01101" => rs2_val_id <= x13; when "01110" => rs2_val_id <= x14;
      when "01111" => rs2_val_id <= x15; when "10000" => rs2_val_id <= x16;
      when "10001" => rs2_val_id <= x17; when "10010" => rs2_val_id <= x18;
      when "10011" => rs2_val_id <= x19; when "10100" => rs2_val_id <= x20;
      when "10101" => rs2_val_id <= x21; when "10110" => rs2_val_id <= x22;
      when "10111" => rs2_val_id <= x23; when "11000" => rs2_val_id <= x24;
      when "11001" => rs2_val_id <= x25; when "11010" => rs2_val_id <= x26;
      when "11011" => rs2_val_id <= x27; when "11100" => rs2_val_id <= x28;
      when "11101" => rs2_val_id <= x29; when "11110" => rs2_val_id <= x30;
      when "11111" => rs2_val_id <= x31;
      when others => rs2_val_id <= X"00000000";
    end case;
  end process;

  -- CSR timer_match, irq_pending, is_mret
  process(timer_compare, timer_counter) is
  begin
    if timer_compare /= X"00000000" and timer_counter >= timer_compare then
      timer_match <= '1';
    else
      timer_match <= '0';
    end if;
  end process;
  irq_pending <= ((csr_mstatus(3)) and (csr_mie(7)) and timer_match and not halted_reg);
  process(id_opcode, id_funct3, ifid_instr, halted_reg) is
  begin
    if id_opcode = "1110011" and id_funct3 = "000"
       and ifid_instr(31 downto 20) = "001100000010" and halted_reg = '0' then
      is_mret <= '1';
    else
      is_mret <= '0';
    end if;
  end process;

  -- CSR read
  process(ifid_instr, csr_mstatus, csr_mie, csr_mtvec, csr_mepc, csr_mcause, timer_match) is
  begin
    case ifid_instr(31 downto 20) is
      when X"300" => csr_read_data <= csr_mstatus;
      when X"304" => csr_read_data <= csr_mie;
      when X"305" => csr_read_data <= csr_mtvec;
      when X"341" => csr_read_data <= csr_mepc;
      when X"342" => csr_read_data <= csr_mcause;
      when X"344" =>
        if timer_match = '1' then
          csr_read_data <= X"00000080";
        else
          csr_read_data <= X"00000000";
        end if;
      when others => csr_read_data <= X"00000000";
    end case;
  end process;

  -- CSR forwarding (from EX or MEM to ID for CSR writes)
  process(id_rs1, idex_rd, idex_reg_write, ex_alu_final,
          exmem_wb_rd, exmem_reg_write, exmem_alu_result, rs1_val_id) is
  begin
    if id_rs1 /= "00000" and id_rs1 = idex_rd and idex_reg_write = '1' then
      csr_rs1_fwd <= ex_alu_final;
    elsif id_rs1 /= "00000" and id_rs1 = exmem_wb_rd and exmem_reg_write = '1' then
      csr_rs1_fwd <= exmem_alu_result;
    else
      csr_rs1_fwd <= rs1_val_id;
    end if;
  end process;

  -- CSR write value
  process(id_funct3, csr_rs1_fwd, csr_read_data, id_rs1) is
  begin
    case id_funct3 is
      when "001" => csr_write_val <= csr_rs1_fwd;
      when "010" => csr_write_val <= csr_read_data or csr_rs1_fwd;
      when "011" => csr_write_val <= csr_read_data and not csr_rs1_fwd;
      when "101" => csr_write_val <= ("000000000000000000000000000" & id_rs1);
      when "110" => csr_write_val <= csr_read_data or ("000000000000000000000000000" & id_rs1);
      when "111" => csr_write_val <= csr_read_data and not ("000000000000000000000000000" & id_rs1);
      when others => csr_write_val <= X"00000000";
    end case;
  end process;

  -- Main decoder (process for all RISC-V opcodes)
  -- {alu_src_a, alu_src_b[1:0], mem_write, mem_to_reg[1:0],
  --  mem_read, branch, alu_op[1:0], jal_jalr[1:0]}
  process(id_opcode) is
  begin
    case id_opcode is
      when "0110011" => -- R-type
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "10"; id_jal_jalr <= "00";
      when "0010011" => -- I-type ALU
        id_alu_src_a <= '0'; id_alu_src_b <= "01"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "10"; id_jal_jalr <= "00";
      when "0000011" => -- Load
        id_alu_src_a <= '0'; id_alu_src_b <= "01"; id_mem_write <= '0';
        id_mem_to_reg <= "01"; id_mem_read <= '1'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "00";
      when "0100011" => -- Store
        id_alu_src_a <= '0'; id_alu_src_b <= "11"; id_mem_write <= '1';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "00";
      when "1100011" => -- Branch
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '1';
        id_alu_op <= "01"; id_jal_jalr <= "00";
      when "0110111" => -- LUI
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "11"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "00";
      when "0010111" => -- AUIPC
        id_alu_src_a <= '1'; id_alu_src_b <= "10"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "00";
      when "1101111" => -- JAL
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "10"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "01";
      when "1100111" => -- JALR
        id_alu_src_a <= '0'; id_alu_src_b <= "01"; id_mem_write <= '0';
        id_mem_to_reg <= "10"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "10";
      when "1110011" => -- System
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "11"; id_jal_jalr <= "00";
      when others =>
        id_alu_src_a <= '0'; id_alu_src_b <= "00"; id_mem_write <= '0';
        id_mem_to_reg <= "00"; id_mem_read <= '0'; id_branch <= '0';
        id_alu_op <= "00"; id_jal_jalr <= "00";
    end case;
  end process;

  -- reg_write decode
  process(id_opcode, id_funct3) is
  begin
    if id_opcode = "0110011" or id_opcode = "0010011" or
       id_opcode = "0000011" or id_opcode = "0110111" or
       id_opcode = "0010111" or id_opcode = "1101111" or
       id_opcode = "1100111" or
       (id_opcode = "1110011" and id_funct3 /= "000") then
      id_reg_write <= '1';
    else
      id_reg_write <= '0';
    end if;
  end process;

  -- ALU decoder
  process(id_alu_op, id_funct3, id_opcode, ifid_instr) is
  begin
    case id_alu_op is
      when "00" => -- ADD or default
        id_alu_control <= "00000";
      when "01" => -- SUB (for branches: compare)
        id_alu_control <= "00001";
      when "10" => -- R-type / I-type ALU
        case id_funct3 is
          when "000" =>
            if id_opcode = "0110011" and ifid_instr(30) = '1' then
              id_alu_control <= "00001"; -- SUB
            else
              id_alu_control <= "00000"; -- ADD
            end if;
          when "001" => id_alu_control <= "00010"; -- SLL
          when "010" => id_alu_control <= "00011"; -- SLT
          when "011" => id_alu_control <= "00100"; -- SLTU
          when "100" => id_alu_control <= "00101"; -- XOR
          when "101" =>
            if ifid_instr(30) = '1' then
              id_alu_control <= "00111"; -- SRA
            else
              id_alu_control <= "00110"; -- SRL
            end if;
          when "110" => id_alu_control <= "01000"; -- OR
          when "111" => id_alu_control <= "01001"; -- AND
          when others => id_alu_control <= "00000";
        end case;
      when "11" => -- CSR / System
        if id_funct3 = "000" then
          case ifid_instr(31 downto 20) is
            when X"302" => id_alu_control <= "10101"; -- MRET
            when X"001" => id_alu_control <= "10110"; -- EBREAK
            when X"000" => id_alu_control <= "10111"; -- ECALL
            when others => id_alu_control <= "00000";
          end case;
        elsif id_funct3 = "001" or id_funct3 = "101" then
          id_alu_control <= "10010"; -- CSRRW/I
        elsif id_funct3 = "010" or id_funct3 = "110" then
          id_alu_control <= "10011"; -- CSRRS/I
        else
          id_alu_control <= "10100"; -- CSRRC/I
        end if;
      when others =>
        id_alu_control <= "00000";
    end case;
  end process;

  -- ================================================================
  -- EX STAGE
  -- ================================================================

  -- Forwarding unit
  process(idex_rs1, exmem_wb_rd, exmem_reg_write) is
  begin
    if idex_rs1 /= "00000" and idex_rs1 = exmem_wb_rd and exmem_reg_write = '1' then
      exmem_match_a <= '1';
    else
      exmem_match_a <= '0';
    end if;
  end process;

  process(idex_rs2, exmem_wb_rd, exmem_reg_write) is
  begin
    if idex_rs2 /= "00000" and idex_rs2 = exmem_wb_rd and exmem_reg_write = '1' then
      exmem_match_b <= '1';
    else
      exmem_match_b <= '0';
    end if;
  end process;

  process(idex_rs1, memwb_wb_rd, memwb_reg_write, exmem_match_a) is
  begin
    if idex_rs1 /= "00000" and idex_rs1 = memwb_wb_rd
       and memwb_reg_write = '1' and exmem_match_a = '0' then
      memwb_match_a <= '1';
    else
      memwb_match_a <= '0';
    end if;
  end process;

  process(idex_rs2, memwb_wb_rd, memwb_reg_write, exmem_match_b) is
  begin
    if idex_rs2 /= "00000" and idex_rs2 = memwb_wb_rd
       and memwb_reg_write = '1' and exmem_match_b = '0' then
      memwb_match_b <= '1';
    else
      memwb_match_b <= '0';
    end if;
  end process;

  -- Forward data sources
  process(exmem_match_a, exmem_mem_read, mem_rdata, exmem_alu_result,
          memwb_match_a, memwb_mem_read, memwb_mem_rdata, memwb_alu_result,
          idex_rs1_val) is
  begin
    if exmem_match_a = '1' and exmem_mem_read = '1' then
      forward_rs1_val <= mem_rdata;
    elsif exmem_match_a = '1' then
      forward_rs1_val <= exmem_alu_result;
    elsif memwb_match_a = '1' and memwb_mem_read = '1' then
      forward_rs1_val <= memwb_mem_rdata;
    elsif memwb_match_a = '1' then
      forward_rs1_val <= memwb_alu_result;
    else
      forward_rs1_val <= idex_rs1_val;
    end if;
  end process;

  process(exmem_match_b, exmem_mem_read, mem_rdata, exmem_alu_result,
          memwb_match_b, memwb_mem_read, memwb_mem_rdata, memwb_alu_result,
          idex_rs2_val) is
  begin
    if exmem_match_b = '1' and exmem_mem_read = '1' then
      forward_rs2_val <= mem_rdata;
    elsif exmem_match_b = '1' then
      forward_rs2_val <= exmem_alu_result;
    elsif memwb_match_b = '1' and memwb_mem_read = '1' then
      forward_rs2_val <= memwb_mem_rdata;
    elsif memwb_match_b = '1' then
      forward_rs2_val <= memwb_alu_result;
    else
      forward_rs2_val <= idex_rs2_val;
    end if;
  end process;

  -- ALU input muxes
  process(idex_alu_src_a, idex_pc, forward_rs1_val) is
  begin
    if idex_alu_src_a = '1' then
      alu_a <= idex_pc;
    else
      alu_a <= forward_rs1_val;
    end if;
  end process;

  process(idex_alu_src_b, forward_rs2_val, idex_imm) is
  begin
    case idex_alu_src_b is
      when "00" => alu_b <= forward_rs2_val;
      when "01" => alu_b <= idex_imm;
      when "10" => alu_b <= idex_imm;
      when "11" => alu_b <= idex_imm;
      when others => alu_b <= forward_rs2_val;
    end case;
  end process;

  -- ALU process (handles all operations including shifts, RV32M)
  process(idex_alu_control, alu_a, alu_b, idex_jal_jalr, idex_mem_to_reg,
          idex_pc_plus_4, idex_imm_u, idex_csr_rdata) is
    variable shamt_int : integer;
    variable sra_mask : bit_vector(31 downto 0);
    variable sra_res : bit_vector(31 downto 0);
    variable slt_res : bit_vector(31 downto 0);
    variable mul_a_64 : bit_vector(63 downto 0);
    variable mul_b_64 : bit_vector(63 downto 0);
    variable mul_res : bit_vector(63 downto 0);
    variable div_a_abs : bit_vector(31 downto 0);
    variable div_b_abs : bit_vector(31 downto 0);
    variable div_q_abs : bit_vector(31 downto 0);
    variable div_r_abs : bit_vector(31 downto 0);
    variable a_neg, b_neg : bit;
  begin
    -- Defaults
    alu_result_ex <= X"00000000";
    shamt_int := 0;
    if alu_b(0) = '1' then shamt_int := shamt_int + 1; end if;
    if alu_b(1) = '1' then shamt_int := shamt_int + 2; end if;
    if alu_b(2) = '1' then shamt_int := shamt_int + 4; end if;
    if alu_b(3) = '1' then shamt_int := shamt_int + 8; end if;
    if alu_b(4) = '1' then shamt_int := shamt_int + 16; end if;

    -- Compute basic ALU result
    case idex_alu_control is
      when "00000" => -- ADD
        alu_result_ex <= alu_a + alu_b;
      when "00001" => -- SUB
        alu_result_ex <= alu_a - alu_b;
      when "00010" => -- SLL
        alu_result_ex <= alu_a sll shamt_int;
      when "00011" => -- SLT (signed)
        if (alu_a(31) = '1' and alu_b(31) = '0') or
           (alu_a(31) = alu_b(31) and alu_a < alu_b) then
          alu_result_ex <= X"00000001";
        else
          alu_result_ex <= X"00000000";
        end if;
      when "00100" => -- SLTU
        if alu_a < alu_b then
          alu_result_ex <= X"00000001";
        else
          alu_result_ex <= X"00000000";
        end if;
      when "00101" => -- XOR
        alu_result_ex <= alu_a xor alu_b;
      when "00110" => -- SRL
        alu_result_ex <= alu_a srl shamt_int;
      when "00111" => -- SRA
        if shamt_int > 0 and alu_a(31) = '1' then
          sra_mask := X"FFFFFFFF";
          sra_mask := sra_mask sll (32 - shamt_int);
          sra_res := (alu_a srl shamt_int) or sra_mask;
        else
          sra_res := alu_a srl shamt_int;
        end if;
        alu_result_ex <= sra_res;
      when "01000" => -- OR
        alu_result_ex <= alu_a or alu_b;
      when "01001" => -- AND
        alu_result_ex <= alu_a and alu_b;
      when "01010" => -- MUL
        if alu_a(31) = '1' then
          mul_a_64 := X"FFFFFFFF" & alu_a;
        else
          mul_a_64 := X"00000000" & alu_a;
        end if;
        if alu_b(31) = '1' then
          mul_b_64 := X"FFFFFFFF" & alu_b;
        else
          mul_b_64 := X"00000000" & alu_b;
        end if;
        mul_res := mul_a_64 * mul_b_64;
        alu_result_ex <= mul_res(31 downto 0);
      when "01011" => -- MULH
        if alu_a(31) = '1' then
          mul_a_64 := X"FFFFFFFF" & alu_a;
        else
          mul_a_64 := X"00000000" & alu_a;
        end if;
        if alu_b(31) = '1' then
          mul_b_64 := X"FFFFFFFF" & alu_b;
        else
          mul_b_64 := X"00000000" & alu_b;
        end if;
        mul_res := mul_a_64 * mul_b_64;
        alu_result_ex <= mul_res(63 downto 32);
      when "01100" => -- MULHSU
        if alu_a(31) = '1' then
          mul_a_64 := X"FFFFFFFF" & alu_a;
        else
          mul_a_64 := X"00000000" & alu_a;
        end if;
        mul_b_64 := X"00000000" & alu_b;
        mul_res := mul_a_64 * mul_b_64;
        alu_result_ex <= mul_res(63 downto 32);
      when "01101" => -- MULHU
        mul_a_64 := X"00000000" & alu_a;
        mul_b_64 := X"00000000" & alu_b;
        mul_res := mul_a_64 * mul_b_64;
        alu_result_ex <= mul_res(63 downto 32);
      when "01110" => -- DIV
        a_neg := alu_a(31); b_neg := alu_b(31);
        if a_neg = '1' then
          div_a_abs := (not alu_a) + X"00000001";
        else
          div_a_abs := alu_a;
        end if;
        if b_neg = '1' then
          div_b_abs := (not alu_b) + X"00000001";
        else
          div_b_abs := alu_b;
        end if;
        if alu_a = X"80000000" and alu_b = X"FFFFFFFF" then
          alu_result_ex <= X"80000000"; -- overflow
        elsif div_b_abs = X"00000000" then
          alu_result_ex <= X"FFFFFFFF"; -- div by 0
        else
          div_q_abs := div_a_abs / div_b_abs;
          if a_neg /= b_neg then
            alu_result_ex <= (not div_q_abs) + X"00000001";
          else
            alu_result_ex <= div_q_abs;
          end if;
        end if;
      when "01111" => -- DIVU
        if alu_b = X"00000000" then
          alu_result_ex <= X"FFFFFFFF";
        else
          alu_result_ex <= alu_a / alu_b;
        end if;
      when "10000" => -- REM
        a_neg := alu_a(31); b_neg := alu_b(31);
        if a_neg = '1' then
          div_a_abs := (not alu_a) + X"00000001";
        else
          div_a_abs := alu_a;
        end if;
        if alu_b = X"00000000" then
          alu_result_ex <= alu_a;
        elsif alu_a = X"80000000" and alu_b = X"FFFFFFFF" then
          alu_result_ex <= X"00000000";
        else
          if b_neg = '1' then
            div_b_abs := (not alu_b) + X"00000001";
          else
            div_b_abs := alu_b;
          end if;
          div_r_abs := div_a_abs rem div_b_abs;
          if a_neg = '1' then
            alu_result_ex <= (not div_r_abs) + X"00000001";
          else
            alu_result_ex <= div_r_abs;
          end if;
        end if;
      when "10001" => -- REMU
        if alu_b = X"00000000" then
          alu_result_ex <= alu_a;
        else
          alu_result_ex <= alu_a rem alu_b;
        end if;
      when "10010" | "10011" | "10100" => -- CSR read
        alu_result_ex <= idex_csr_rdata;
      when "10101" => -- MRET (returns PC+4 for forwarding)
        alu_result_ex <= idex_pc_plus_4;
      when "10110" | "10111" => -- EBREAK / ECALL
        alu_result_ex <= X"00000000";
      when others =>
        alu_result_ex <= X"00000000";
    end case;
  end process;

  -- ALU result full: for JAL/JALR override (PC+4) and LUI (imm_u)
  process(idex_jal_jalr, idex_pc_plus_4, idex_mem_to_reg, idex_imm_u, alu_result_ex) is
  begin
    if idex_jal_jalr /= "00" then
      ex_alu_final <= idex_pc_plus_4;
    elsif idex_mem_to_reg = "11" then
      ex_alu_final <= idex_imm_u;
    else
      ex_alu_final <= alu_result_ex;
    end if;
  end process;

  -- Branch comparison
  process(idex_funct3, forward_rs1_val, forward_rs2_val) is
    variable b_taken : bit;
  begin
    b_taken := '0';
    case idex_funct3 is
      when "000" =>
        if forward_rs1_val = forward_rs2_val then b_taken := '1'; end if;
      when "001" =>
        if forward_rs1_val /= forward_rs2_val then b_taken := '1'; end if;
      when "100" =>
        if (forward_rs1_val(31) = '1' and forward_rs2_val(31) = '0') or
           (forward_rs1_val(31) = forward_rs2_val(31) and forward_rs1_val < forward_rs2_val) then
          b_taken := '1';
        end if;
      when "101" =>
        if (forward_rs1_val(31) = '0' and forward_rs2_val(31) = '1') or
           (forward_rs1_val(31) = forward_rs2_val(31) and forward_rs1_val >= forward_rs2_val) then
          b_taken := '1';
        end if;
      when "110" =>
        if forward_rs1_val < forward_rs2_val then b_taken := '1'; end if;
      when "111" =>
        if forward_rs1_val >= forward_rs2_val then b_taken := '1'; end if;
      when others => null;
    end case;
    branch_taken <= b_taken;
  end process;

  -- Branch/JAL/JALR target computation
  ex_branch_target <= idex_pc + idex_imm_b;
  ex_jal_target   <= idex_pc + idex_imm_j;
  ex_jalr_target  <= (forward_rs1_val + idex_imm_i) and X"FFFFFFFE";

  -- ================================================================
  -- MEM STAGE
  -- ================================================================
  process(exmem_alu_result) is
    variable tmp : bit_vector(31 downto 0);
  begin
    tmp := exmem_alu_result srl 2;
    dmem_waddr <= tmp(7 downto 0);
  end process;

  -- DMEM read current word at dmem_waddr
  process(dmem, dmem_waddr) is
    variable dmem_rd_idx : integer;
    variable dmem_rd_shifted : bit_vector(8191 downto 0);
  begin
    dmem_rd_idx := 0;
    if dmem_waddr(0) = '1' then dmem_rd_idx := dmem_rd_idx + 1; end if;
    if dmem_waddr(1) = '1' then dmem_rd_idx := dmem_rd_idx + 2; end if;
    if dmem_waddr(2) = '1' then dmem_rd_idx := dmem_rd_idx + 4; end if;
    if dmem_waddr(3) = '1' then dmem_rd_idx := dmem_rd_idx + 8; end if;
    if dmem_waddr(4) = '1' then dmem_rd_idx := dmem_rd_idx + 16; end if;
    if dmem_waddr(5) = '1' then dmem_rd_idx := dmem_rd_idx + 32; end if;
    if dmem_waddr(6) = '1' then dmem_rd_idx := dmem_rd_idx + 64; end if;
    if dmem_waddr(7) = '1' then dmem_rd_idx := dmem_rd_idx + 128; end if;
    dmem_rd_shifted := dmem srl (dmem_rd_idx * 32);
    dmem_cur_word <= dmem_rd_shifted(31 downto 0);
  end process;

  -- Byte-merged write data for SB/SH
  process(exmem_funct3, exmem_alu_result, dmem_cur_word, exmem_rs2_val) is
  begin
    case exmem_funct3 is
      when "000" => -- SB
        case exmem_alu_result(1 downto 0) is
          when "00" => dmem_wdata_merged <= dmem_cur_word(31 downto 8) & exmem_rs2_val(7 downto 0);
          when "01" => dmem_wdata_merged <= dmem_cur_word(31 downto 16) & exmem_rs2_val(7 downto 0) & dmem_cur_word(7 downto 0);
          when "10" => dmem_wdata_merged <= dmem_cur_word(31 downto 24) & exmem_rs2_val(7 downto 0) & dmem_cur_word(15 downto 0);
          when others => dmem_wdata_merged <= exmem_rs2_val(7 downto 0) & dmem_cur_word(23 downto 0);
        end case;
      when "001" => -- SH
        if exmem_alu_result(1) = '0' then
          dmem_wdata_merged <= dmem_cur_word(31 downto 16) & exmem_rs2_val(15 downto 0);
        else
          dmem_wdata_merged <= exmem_rs2_val(15 downto 0) & dmem_cur_word(15 downto 0);
        end if;
      when others => -- SW
        dmem_wdata_merged <= exmem_rs2_val;
    end case;
  end process;

  -- Unified read (IMEM for low addresses, DMEM for high)
  process(exmem_alu_result, dmem, imem_port) is
    variable imem_idx : integer;
    variable dmem_idx : integer;
    variable shifted_imem : bit_vector(32767 downto 0);
    variable shifted_dmem : bit_vector(8191 downto 0);
    variable addr_div4 : bit_vector(31 downto 0);
  begin
    addr_div4 := exmem_alu_result srl 2;
    if exmem_alu_result >= X"00010000" then
      dmem_idx := 0;
      if addr_div4(0) = '1' then dmem_idx := dmem_idx + 1; end if;
      if addr_div4(1) = '1' then dmem_idx := dmem_idx + 2; end if;
      if addr_div4(2) = '1' then dmem_idx := dmem_idx + 4; end if;
      if addr_div4(3) = '1' then dmem_idx := dmem_idx + 8; end if;
      if addr_div4(4) = '1' then dmem_idx := dmem_idx + 16; end if;
      if addr_div4(5) = '1' then dmem_idx := dmem_idx + 32; end if;
      if addr_div4(6) = '1' then dmem_idx := dmem_idx + 64; end if;
      if addr_div4(7) = '1' then dmem_idx := dmem_idx + 128; end if;
      shifted_dmem := dmem srl (dmem_idx * 32);
      mem_rdata <= shifted_dmem(31 downto 0);
    else
      imem_idx := 0;
      if addr_div4(0) = '1' then imem_idx := imem_idx + 1; end if;
      if addr_div4(1) = '1' then imem_idx := imem_idx + 2; end if;
      if addr_div4(2) = '1' then imem_idx := imem_idx + 4; end if;
      if addr_div4(3) = '1' then imem_idx := imem_idx + 8; end if;
      if addr_div4(4) = '1' then imem_idx := imem_idx + 16; end if;
      if addr_div4(5) = '1' then imem_idx := imem_idx + 32; end if;
      if addr_div4(6) = '1' then imem_idx := imem_idx + 64; end if;
      if addr_div4(7) = '1' then imem_idx := imem_idx + 128; end if;
      if addr_div4(8) = '1' then imem_idx := imem_idx + 256; end if;
      if addr_div4(9) = '1' then imem_idx := imem_idx + 512; end if;
      shifted_imem := imem_port srl (imem_idx * 32);
      mem_rdata <= shifted_imem(31 downto 0);
    end if;
  end process;

  -- Load byte/halfword extraction and sign extension (DMEM read data comb)
  process(exmem_funct3, mem_rdata, exmem_alu_result) is
    variable load_offset : bit_vector(1 downto 0);
    variable load_byte : bit_vector(7 downto 0);
    variable load_hword : bit_vector(15 downto 0);
  begin
    load_offset := exmem_alu_result(1 downto 0);
    case load_offset is
      when "00" => load_byte := mem_rdata(7 downto 0);
      when "01" => load_byte := mem_rdata(15 downto 8);
      when "10" => load_byte := mem_rdata(23 downto 16);
      when others => load_byte := mem_rdata(31 downto 24);
    end case;
    case load_offset is
      when "00" => load_hword := mem_rdata(15 downto 0);
      when others => load_hword := mem_rdata(31 downto 16);
    end case;

    case exmem_funct3 is
      when "000" => -- LB
        if load_byte(7) = '1' then
          dmem_rdata_comb <= X"FFFFFF" & load_byte;
        else
          dmem_rdata_comb <= X"000000" & load_byte;
        end if;
      when "001" => -- LH
        if load_hword(15) = '1' then
          dmem_rdata_comb <= X"FFFF" & load_hword;
        else
          dmem_rdata_comb <= X"0000" & load_hword;
        end if;
      when "010" => -- LW
        dmem_rdata_comb <= mem_rdata;
      when "100" => -- LBU
        dmem_rdata_comb <= X"000000" & load_byte;
      when "101" => -- LHU
        dmem_rdata_comb <= X"0000" & load_hword;
      when others =>
        dmem_rdata_comb <= mem_rdata;
    end case;
  end process;

  -- ================================================================
  -- WB STAGE
  -- ================================================================
  process(memwb_mem_to_reg, memwb_mem_rdata, memwb_pc_plus_4, memwb_imm_u, memwb_alu_result) is
  begin
    case memwb_mem_to_reg is
      when "01" => memwb_write_data <= memwb_mem_rdata;
      when "10" => memwb_write_data <= memwb_pc_plus_4;
      when "11" => memwb_write_data <= memwb_imm_u;
      when others => memwb_write_data <= memwb_alu_result;
    end case;
  end process;

  process(exmem_mem_to_reg, dmem_rdata_comb, exmem_pc_plus_4, exmem_imm_u, exmem_alu_result) is
  begin
    case exmem_mem_to_reg is
      when "01" => exmem_write_data <= dmem_rdata_comb;
      when "10" => exmem_write_data <= exmem_pc_plus_4;
      when "11" => exmem_write_data <= exmem_imm_u;
      when others => exmem_write_data <= exmem_alu_result;
    end case;
  end process;

  -- ================================================================
  -- Hazard detection
  -- ================================================================
  process(idex_mem_read, idex_rd, id_rs1, id_rs2) is
  begin
    if idex_mem_read = '1' and idex_rd /= "00000"
       and (idex_rd = id_rs1 or idex_rd = id_rs2) then
      load_use_stall <= '1';
    else
      load_use_stall <= '0';
    end if;
  end process;

  process(idex_alu_control) is
  begin
    if idex_alu_control = "10110" then
      ex_is_ebreak <= '1';
    else
      ex_is_ebreak <= '0';
    end if;
  end process;

  process(idex_branch, branch_taken, idex_jal_jalr, ex_is_ebreak) is
  begin
    if (idex_branch = '1' and branch_taken = '1')
       or idex_jal_jalr /= "00" or ex_is_ebreak = '1' then
      flush_ifid <= '1';
    else
      flush_ifid <= '0';
    end if;
  end process;
  bubble_idex <= load_use_stall or flush_ifid;

  -- ================================================================
  -- Debug output assignments
  -- ================================================================
  halted <= halted_reg;
  pc <= pc_reg;
  reg_x1 <= x1;
  reg_x10 <= x10;
  instruction <= ifid_instr;
  alu_result <= exmem_alu_result;
  dmem_addr <= exmem_alu_result;
  dmem_rdata <= dmem_rdata_comb;
  dmem_wdata <= exmem_rs2_val;
  dmem_we <= exmem_mem_write;
  uart_tx_valid <= uart_tx_valid_reg;
  uart_tx_data <= uart_tx_data_reg;
  debug_ex_branch_taken <= idex_branch and branch_taken;
  debug_idex_branch <= idex_branch;
  debug_idex_funct3 <= idex_funct3;
  debug_forward_rs1_val <= forward_rs1_val;
  debug_forward_rs2_val <= forward_rs2_val;
  debug_idex_imm <= X"000" & ifid_instr(30 downto 21) & ifid_instr(20) & ifid_instr(19 downto 12) & ifid_instr(31);
  debug_branch_taken <= branch_taken;
  debug_flush_ifid <= flush_ifid;
  debug_load_use_stall <= load_use_stall;
  debug_ex_branch_target <= ex_branch_target;
  debug_idex_pc <= idex_pc;
  debug_idex_rd <= idex_rd;
  debug_idex_rs1 <= idex_rs1;
  debug_idex_rs2 <= idex_rs2;
  debug_alu_a <= alu_a;
  debug_alu_b <= alu_b;
  debug_ex_alu_final <= ex_alu_final;
  debug_exmem_write_data <= exmem_write_data;
  debug_idex_imm_real <= idex_imm;

  -- ================================================================
  -- Main sequential process
  -- ================================================================
  process(clk, rst) is
    variable dmem_tmp : bit_vector(8191 downto 0);
    variable dmem_base : integer;
  begin
    if rst = '1' then
      pc_reg <= X"00000000";
      halted_reg <= '0';
      ifid_instr <= X"00000000";
      ifid_pc_plus_4 <= X"00000000";
      idex_alu_src_a <= '0'; idex_alu_src_b <= "00";
      idex_mem_write <= '0'; idex_mem_to_reg <= "00";
      idex_mem_read <= '0'; idex_reg_write <= '0';
      idex_alu_control <= "00000"; idex_jal_jalr <= "00";
      idex_branch <= '0';
      idex_rs1 <= "00000"; idex_rs2 <= "00000"; idex_rd <= "00000";
      idex_rs1_val <= X"00000000"; idex_rs2_val <= X"00000000";
      idex_imm <= X"00000000"; idex_imm_b <= X"00000000";
      idex_imm_j <= X"00000000"; idex_imm_u <= X"00000000";
      idex_imm_i <= X"00000000";
      idex_pc_plus_4 <= X"00000000"; idex_pc <= X"00000000";
      idex_funct3 <= "000";
      idex_csr_rdata <= X"00000000";
      exmem_mem_write <= '0'; exmem_mem_read <= '0';
      exmem_mem_to_reg <= "00"; exmem_reg_write <= '0';
      exmem_wb_rd <= "00000";
      exmem_alu_result <= X"00000000"; exmem_rs2_val <= X"00000000";
      exmem_pc_plus_4 <= X"00000000"; exmem_funct3 <= "000";
      exmem_imm_u <= X"00000000";
      memwb_reg_write <= '0'; memwb_mem_to_reg <= "00"; memwb_mem_read <= '0';
      memwb_wb_rd <= "00000";
      memwb_alu_result <= X"00000000"; memwb_mem_rdata <= X"00000000";
      memwb_pc_plus_4 <= X"00000000"; memwb_imm_u <= X"00000000";
      x1 <= X"00000000"; x2 <= X"00000000"; x3 <= X"00000000"; x4 <= X"00000000";
      x5 <= X"00000000"; x6 <= X"00000000"; x7 <= X"00000000"; x8 <= X"00000000";
      x9 <= X"00000000"; x10 <= X"00000000"; x11 <= X"00000000"; x12 <= X"00000000";
      x13 <= X"00000000"; x14 <= X"00000000"; x15 <= X"00000000"; x16 <= X"00000000";
      x17 <= X"00000000"; x18 <= X"00000000"; x19 <= X"00000000"; x20 <= X"00000000";
      x21 <= X"00000000"; x22 <= X"00000000"; x23 <= X"00000000"; x24 <= X"00000000";
      x25 <= X"00000000"; x26 <= X"00000000"; x27 <= X"00000000"; x28 <= X"00000000";
      x29 <= X"00000000"; x30 <= X"00000000"; x31 <= X"00000000";
      uart_tx_valid_reg <= '0';
      uart_tx_data_reg <= X"00";
      csr_mtvec <= X"00000000"; csr_mepc <= X"00000000";
      csr_mcause <= X"00000000"; csr_mie <= X"00000000";
      csr_mstatus <= X"00000000";
      timer_compare <= X"00000000"; timer_counter <= X"00000000";
      dmem <= dmem xor dmem;
    elsif clk = '1' then
      -- PC update
      if halted_reg = '0' then
        if irq_pending = '1' then
          pc_reg <= csr_mtvec;
        elsif is_mret = '1' then
          pc_reg <= csr_mepc;
        elsif load_use_stall = '1' then
          pc_reg <= pc_reg;
        elsif idex_branch = '1' and branch_taken = '1' then
          pc_reg <= ex_branch_target;
        elsif idex_jal_jalr = "01" then
          pc_reg <= ex_jal_target;
        elsif idex_jal_jalr = "10" then
          pc_reg <= ex_jalr_target;
        else
          pc_reg <= if_pc_plus_4;
        end if;
      end if;

      -- IF/ID
      if halted_reg = '1' or irq_pending = '1' or flush_ifid = '1' or is_mret = '1' then
        ifid_instr <= X"00000000";
        ifid_pc_plus_4 <= X"00000000";
      elsif load_use_stall = '0' then
        ifid_instr <= if_instr;
        ifid_pc_plus_4 <= if_pc_plus_4;
      end if;

      -- ID/EX
      if halted_reg = '1' or irq_pending = '1' or bubble_idex = '1' or is_mret = '1' then
        idex_alu_src_a <= '0'; idex_alu_src_b <= "00";
        idex_mem_write <= '0'; idex_mem_to_reg <= "00";
        idex_mem_read <= '0'; idex_reg_write <= '0';
        idex_alu_control <= "00000"; idex_jal_jalr <= "00";
        idex_branch <= '0';
        idex_rs1 <= "00000"; idex_rs2 <= "00000"; idex_rd <= "00000";
        idex_rs1_val <= X"00000000"; idex_rs2_val <= X"00000000";
        idex_imm <= X"00000000"; idex_imm_b <= X"00000000";
        idex_imm_j <= X"00000000"; idex_imm_u <= X"00000000";
        idex_imm_i <= X"00000000";
        idex_pc_plus_4 <= X"00000000"; idex_pc <= X"00000000";
        idex_funct3 <= "000";
        idex_csr_rdata <= X"00000000";
      elsif load_use_stall = '0' then
        idex_alu_src_a <= id_alu_src_a;
        idex_alu_src_b <= id_alu_src_b;
        idex_mem_write <= id_mem_write;
        idex_mem_to_reg <= id_mem_to_reg;
        idex_mem_read <= id_mem_read;
        idex_reg_write <= id_reg_write;
        idex_alu_control <= id_alu_control;
        idex_jal_jalr <= id_jal_jalr;
        idex_branch <= id_branch;
        idex_rs1 <= id_rs1; idex_rs2 <= id_rs2; idex_rd <= id_rd;
        idex_rs1_val <= rs1_val_id; idex_rs2_val <= rs2_val_id;
        idex_imm <= imm_id; idex_imm_b <= imm_b;
        idex_imm_j <= imm_j; idex_imm_u <= imm_u;
        idex_imm_i <= imm_i;
        idex_pc_plus_4 <= ifid_pc_plus_4;
        idex_pc <= ifid_pc_plus_4 - X"00000004";
        idex_funct3 <= id_funct3;
        idex_csr_rdata <= csr_read_data;
      end if;

      -- EX/MEM
      exmem_mem_write <= idex_mem_write;
      exmem_mem_read <= idex_mem_read;
      exmem_mem_to_reg <= idex_mem_to_reg;
      exmem_reg_write <= idex_reg_write;
      exmem_wb_rd <= idex_rd;
      exmem_alu_result <= ex_alu_final;
      exmem_rs2_val <= forward_rs2_val;
      exmem_pc_plus_4 <= idex_pc_plus_4;
      exmem_funct3 <= idex_funct3;
      exmem_imm_u <= idex_imm_u;

      -- MEM/WB
      memwb_reg_write <= exmem_reg_write;
      memwb_mem_to_reg <= exmem_mem_to_reg;
      memwb_mem_read <= exmem_mem_read;
      memwb_wb_rd <= exmem_wb_rd;
      memwb_alu_result <= exmem_alu_result;
      memwb_mem_rdata <= dmem_rdata_comb;
      memwb_pc_plus_4 <= exmem_pc_plus_4;
      memwb_imm_u <= exmem_imm_u;

      -- Interrupt entry
      if halted_reg = '0' and irq_pending = '1' then
        csr_mepc <= pc_reg;
        csr_mcause <= X"80000007";
        csr_mstatus <= csr_mstatus and X"FFFFFFF7";
      end if;

      -- MRET
      if halted_reg = '0' and is_mret = '1' then
        csr_mstatus <= csr_mstatus or X"00000008";
      end if;

      -- CSR writes
      if halted_reg = '0' and id_opcode = "1110011" and id_funct3 /= "000" then
        case ifid_instr(31 downto 20) is
          when X"300" => csr_mstatus <= csr_write_val;
          when X"304" => csr_mie <= csr_write_val;
          when X"305" => csr_mtvec <= csr_write_val;
          when X"341" => csr_mepc <= csr_write_val;
          when X"342" => csr_mcause <= csr_write_val;
          when others => null;
        end case;
      end if;

      -- EBREAK
      if ex_is_ebreak = '1' then
        halted_reg <= '1';
      end if;

      -- Timer counter
      if halted_reg = '0' then
        timer_counter <= timer_counter + X"00000001";
      end if;

      -- DMEM write
      if exmem_mem_write = '1' and exmem_alu_result /= X"10000000"
                             and exmem_alu_result /= X"20000000" then
        dmem_tmp := dmem;
        dmem_base := 0;
        if dmem_waddr(0) = '1' then dmem_base := dmem_base + 1; end if;
        if dmem_waddr(1) = '1' then dmem_base := dmem_base + 2; end if;
        if dmem_waddr(2) = '1' then dmem_base := dmem_base + 4; end if;
        if dmem_waddr(3) = '1' then dmem_base := dmem_base + 8; end if;
        if dmem_waddr(4) = '1' then dmem_base := dmem_base + 16; end if;
        if dmem_waddr(5) = '1' then dmem_base := dmem_base + 32; end if;
        if dmem_waddr(6) = '1' then dmem_base := dmem_base + 64; end if;
        if dmem_waddr(7) = '1' then dmem_base := dmem_base + 128; end if;
        dmem_base := dmem_base * 32;
        dmem_tmp(dmem_base + 31 downto dmem_base) := dmem_wdata_merged;
        dmem <= dmem_tmp;
      end if;

      -- UART write at 0x10000000
      if exmem_mem_write = '1' and exmem_alu_result = X"10000000" then
        uart_tx_data_reg <= exmem_rs2_val(7 downto 0);
        uart_tx_valid_reg <= '1';
      else
        uart_tx_valid_reg <= '0';
      end if;

      -- Timer compare write at 0x20000000
      if exmem_mem_write = '1' and exmem_alu_result = X"20000000" then
        timer_compare <= exmem_rs2_val;
      end if;

      -- Register file write
      if exmem_reg_write = '1' and exmem_wb_rd /= "00000" then
        if exmem_wb_rd = "00001" then x1 <= exmem_write_data; end if;
        if exmem_wb_rd = "00010" then x2 <= exmem_write_data; end if;
        if exmem_wb_rd = "00011" then x3 <= exmem_write_data; end if;
        if exmem_wb_rd = "00100" then x4 <= exmem_write_data; end if;
        if exmem_wb_rd = "00101" then x5 <= exmem_write_data; end if;
        if exmem_wb_rd = "00110" then x6 <= exmem_write_data; end if;
        if exmem_wb_rd = "00111" then x7 <= exmem_write_data; end if;
        if exmem_wb_rd = "01000" then x8 <= exmem_write_data; end if;
        if exmem_wb_rd = "01001" then x9 <= exmem_write_data; end if;
        if exmem_wb_rd = "01010" then x10 <= exmem_write_data; end if;
        if exmem_wb_rd = "01011" then x11 <= exmem_write_data; end if;
        if exmem_wb_rd = "01100" then x12 <= exmem_write_data; end if;
        if exmem_wb_rd = "01101" then x13 <= exmem_write_data; end if;
        if exmem_wb_rd = "01110" then x14 <= exmem_write_data; end if;
        if exmem_wb_rd = "01111" then x15 <= exmem_write_data; end if;
        if exmem_wb_rd = "10000" then x16 <= exmem_write_data; end if;
        if exmem_wb_rd = "10001" then x17 <= exmem_write_data; end if;
        if exmem_wb_rd = "10010" then x18 <= exmem_write_data; end if;
        if exmem_wb_rd = "10011" then x19 <= exmem_write_data; end if;
        if exmem_wb_rd = "10100" then x20 <= exmem_write_data; end if;
        if exmem_wb_rd = "10101" then x21 <= exmem_write_data; end if;
        if exmem_wb_rd = "10110" then x22 <= exmem_write_data; end if;
        if exmem_wb_rd = "10111" then x23 <= exmem_write_data; end if;
        if exmem_wb_rd = "11000" then x24 <= exmem_write_data; end if;
        if exmem_wb_rd = "11001" then x25 <= exmem_write_data; end if;
        if exmem_wb_rd = "11010" then x26 <= exmem_write_data; end if;
        if exmem_wb_rd = "11011" then x27 <= exmem_write_data; end if;
        if exmem_wb_rd = "11100" then x28 <= exmem_write_data; end if;
        if exmem_wb_rd = "11101" then x29 <= exmem_write_data; end if;
        if exmem_wb_rd = "11110" then x30 <= exmem_write_data; end if;
        if exmem_wb_rd = "11111" then x31 <= exmem_write_data; end if;
      end if;
    end if;
  end process;

end architecture;
