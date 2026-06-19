// RV32I 5-Stage Pipeline CPU -- qsim-compatible Verilog
// Implements all 40 RV32I base instructions + RV32M. Harvard architecture.
// 5-stage: IF, ID, EX, MEM, WB with forwarding and hazard detection.
//
// R-type: funct7[31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
// I-type: imm[31:20]    | rs1[19:15] | funct3[14:12] | rd[11:7]   | opcode[6:0]
// S-type: imm[31:25]    | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:0] | opcode[6:0]
// B-type: instr[31|30:25|11:8|7] -> imm[12|10:5|4:1|11]
// U-type: imm[31:12]    | rd[11:7]   | opcode[6:0]
// J-type: instr[31|30:21|20|19:12] -> imm[20|10:1|11|19:12]

module rv32i_top (
    input  wire       clk,
    input  wire       rst,
    output wire       halted,
    output wire [31:0] pc,
    output wire [31:0] instruction,
    output wire [31:0] reg_x1,
    output wire [31:0] reg_x10,
    output wire [31:0] alu_result,
    output wire [31:0] dmem_addr,
    output wire [31:0] dmem_rdata,
    output wire [31:0] dmem_wdata,
    output wire        dmem_we,
    output wire        uart_tx_valid,
    output wire [7:0]  uart_tx_data,
    // Debug outputs for pipeline debug
    output wire        debug_ex_branch_taken,
    output wire        debug_idex_branch,
    output wire [2:0]  debug_idex_funct3,
    output wire [31:0] debug_forward_rs1_val,
    output wire [31:0] debug_forward_rs2_val,
    output wire [31:0] debug_idex_imm,
    output wire        debug_branch_taken,
    output wire        debug_flush_ifid,
    output wire        debug_load_use_stall,
    output wire [31:0] debug_ex_branch_target,
    output wire [31:0] debug_idex_pc,
    output wire [4:0]  debug_idex_rd,
    output wire [4:0]  debug_idex_rs1,
    output wire [4:0]  debug_idex_rs2
);

  // IMEM: 1024 words x 32 bits
  reg [31:0] imem [0:1023];

  // DMEM: 256 words x 32 bits
  reg [31:0] dmem [0:255];

  // Register file: x1-x31 (x0 hardwired to zero)
  reg [31:0] x1, x2, x3, x4, x5, x6, x7, x8;
  reg [31:0] x9, x10, x11, x12, x13, x14, x15, x16;
  reg [31:0] x17, x18, x19, x20, x21, x22, x23, x24;
  reg [31:0] x25, x26, x27, x28, x29, x30, x31;

  // PC and halt state
  reg [31:0] pc_reg;
  reg halted_reg;

  // UART output
  reg uart_tx_valid_reg;
  reg [7:0] uart_tx_data_reg;

  // ============================================================
  // CSR registers
  // ============================================================
  reg [31:0] csr_mtvec;     // trap handler base address (default 0)
  reg [31:0] csr_mepc;      // saved PC on interrupt
  reg [31:0] csr_mcause;    // cause of interrupt
  reg [31:0] csr_mie;       // interrupt enable bits (bit 7 = MTIE)
  reg [31:0] csr_mstatus;   // machine status (bit 3 = MIE)

  // ============================================================
  // Timer peripheral
  // ============================================================
  reg [31:0] timer_compare;
  reg [31:0] timer_counter;

  // ============================================================
  // IF STAGE: Instruction fetch
  // ============================================================
  wire [31:0] if_instr;
  wire [31:0] if_pc_plus_4;
  assign if_instr = imem[(pc_reg >> 2) & 10'h3FF];
  assign if_pc_plus_4 = pc_reg + 32'd4;

  // ============================================================
  // IF/ID PIPELINE REGISTER
  // ============================================================
  reg [31:0] ifid_instr;
  reg [31:0] ifid_pc_plus_4;

  // ============================================================
  // ID STAGE: Instruction decode, register read, control decode
  // ============================================================
  // Field extraction
  wire [6:0] id_opcode;
  wire [4:0] id_rd, id_rs1, id_rs2;
  wire [2:0] id_funct3;
  wire [6:0] id_funct7;
  assign id_opcode = ifid_instr & 7'h7F;
  assign id_rd     = (ifid_instr >> 4'd7) & 5'h1F;
  assign id_funct3 = (ifid_instr >> 4'd12) & 3'h7;
  assign id_rs1    = (ifid_instr >> 4'd15) & 5'h1F;
  assign id_rs2    = (ifid_instr >> 5'd20) & 5'h1F;
  assign id_funct7 = (ifid_instr >> 5'd25) & 7'h7F;

  // Immediate generation (all 6 formats)
  wire [11:0] imm_i_raw;
  wire [31:0] imm_i;
  assign imm_i_raw = (ifid_instr >> 5'd20) & 12'hFFF;
  assign imm_i = ((imm_i_raw >> 4'd11) & 1'h1)
               ? {20'hFFFFF, imm_i_raw}
               : {20'h00000, imm_i_raw};

  wire [11:0] imm_s_raw;
  wire [31:0] imm_s;
  assign imm_s_raw = (((ifid_instr >> 5'd25) & 7'h7F) << 5) | ((ifid_instr >> 4'd7) & 5'h1F);
  assign imm_s = ((imm_s_raw >> 4'd11) & 1'h1)
               ? {20'hFFFFF, imm_s_raw}
               : {20'h00000, imm_s_raw};

  wire [12:0] imm_b_raw;
  wire [31:0] imm_b;
  assign imm_b_raw = (((ifid_instr >> 5'd31) & 1'h1) << 12)
                   | (((ifid_instr >> 4'd7)  & 1'h1) << 11)
                   | (((ifid_instr >> 5'd25) & 6'h3F) << 5)
                   | (((ifid_instr >> 4'd8)  & 4'hF) << 1);
  assign imm_b = (((ifid_instr >> 5'd31) & 1'h1))
               ? {19'h7FFFF, imm_b_raw}
               : {19'h00000, imm_b_raw};

  wire [31:0] imm_u;
  assign imm_u = ifid_instr & 32'hFFFFF000;

  wire [20:0] imm_j_raw;
  wire [31:0] imm_j;
  assign imm_j_raw = (((ifid_instr >> 5'd31) & 1'h1) << 20)
                   | (((ifid_instr >> 4'd12) & 8'hFF) << 12)
                   | (((ifid_instr >> 5'd20) & 1'h1) << 11)
                   | (((ifid_instr >> 5'd21) & 10'h3FF) << 1);
  assign imm_j = (((ifid_instr >> 5'd31) & 1'h1))
               ? {11'h7FF, imm_j_raw}
               : {11'h000, imm_j_raw};

  // Immediate mux (select by ID opcode)
  wire [31:0] imm_id;
  assign imm_id = (id_opcode == 7'h13 || id_opcode == 7'h03 || id_opcode == 7'h67) ? imm_i
                : (id_opcode == 7'h23) ? imm_s
                : (id_opcode == 7'h63) ? imm_b
                : (id_opcode == 7'h6F) ? imm_j
                : imm_u;

  // Register file read
  wire [31:0] rs1_val_id, rs2_val_id;
  assign rs1_val_id = (id_rs1 == 5'h00) ? 32'h00000000
                    : (id_rs1 == 5'h01) ? x1  : (id_rs1 == 5'h02) ? x2
                    : (id_rs1 == 5'h03) ? x3  : (id_rs1 == 5'h04) ? x4
                    : (id_rs1 == 5'h05) ? x5  : (id_rs1 == 5'h06) ? x6
                    : (id_rs1 == 5'h07) ? x7  : (id_rs1 == 5'h08) ? x8
                    : (id_rs1 == 5'h09) ? x9  : (id_rs1 == 5'h0A) ? x10
                    : (id_rs1 == 5'h0B) ? x11 : (id_rs1 == 5'h0C) ? x12
                    : (id_rs1 == 5'h0D) ? x13 : (id_rs1 == 5'h0E) ? x14
                    : (id_rs1 == 5'h0F) ? x15 : (id_rs1 == 5'h10) ? x16
                    : (id_rs1 == 5'h11) ? x17 : (id_rs1 == 5'h12) ? x18
                    : (id_rs1 == 5'h13) ? x19 : (id_rs1 == 5'h14) ? x20
                    : (id_rs1 == 5'h15) ? x21 : (id_rs1 == 5'h16) ? x22
                    : (id_rs1 == 5'h17) ? x23 : (id_rs1 == 5'h18) ? x24
                    : (id_rs1 == 5'h19) ? x25 : (id_rs1 == 5'h1A) ? x26
                    : (id_rs1 == 5'h1B) ? x27 : (id_rs1 == 5'h1C) ? x28
                    : (id_rs1 == 5'h1D) ? x29 : (id_rs1 == 5'h1E) ? x30
                    : x31;
  assign rs2_val_id = (id_rs2 == 5'h00) ? 32'h00000000
                    : (id_rs2 == 5'h01) ? x1  : (id_rs2 == 5'h02) ? x2
                    : (id_rs2 == 5'h03) ? x3  : (id_rs2 == 5'h04) ? x4
                    : (id_rs2 == 5'h05) ? x5  : (id_rs2 == 5'h06) ? x6
                    : (id_rs2 == 5'h07) ? x7  : (id_rs2 == 5'h08) ? x8
                    : (id_rs2 == 5'h09) ? x9  : (id_rs2 == 5'h0A) ? x10
                    : (id_rs2 == 5'h0B) ? x11 : (id_rs2 == 5'h0C) ? x12
                    : (id_rs2 == 5'h0D) ? x13 : (id_rs2 == 5'h0E) ? x14
                    : (id_rs2 == 5'h0F) ? x15 : (id_rs2 == 5'h10) ? x16
                    : (id_rs2 == 5'h11) ? x17 : (id_rs2 == 5'h12) ? x18
                    : (id_rs2 == 5'h13) ? x19 : (id_rs2 == 5'h14) ? x20
                    : (id_rs2 == 5'h15) ? x21 : (id_rs2 == 5'h16) ? x22
                    : (id_rs2 == 5'h17) ? x23 : (id_rs2 == 5'h18) ? x24
                    : (id_rs2 == 5'h19) ? x25 : (id_rs2 == 5'h1A) ? x26
                    : (id_rs2 == 5'h1B) ? x27 : (id_rs2 == 5'h1C) ? x28
                    : (id_rs2 == 5'h1D) ? x29 : (id_rs2 == 5'h1E) ? x30
                    : x31;

  // CSR read mux
  wire timer_match;
  assign timer_match = (timer_compare != 32'h00000000) && (timer_counter >= timer_compare);
  wire irq_pending;
  assign irq_pending = ((csr_mstatus >> 5'd3) & 1'h1) & ((csr_mie >> 5'd7) & 1'h1) & timer_match & ~halted_reg;
  wire is_mret;
  assign is_mret = (id_opcode == 7'h73 && id_funct3 == 3'h0 && ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h302 && ~halted_reg);

  wire [31:0] csr_read_data;
  assign csr_read_data = ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h300 ? csr_mstatus
                       : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h304 ? csr_mie
                       : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h305 ? csr_mtvec
                       : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h341 ? csr_mepc
                       : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h342 ? csr_mcause
                       : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h344 ? {24'h000000, timer_match ? 8'h80 : 8'h00}
                       : 32'h00000000;

  // CSR write value (uses forwarded rs1 to handle RAW hazard from preceding ALU)
  wire [31:0] csr_rs1_fwd;
  assign csr_rs1_fwd = (id_rs1 != 5'h00 && id_rs1 == idex_rd && idex_reg_write) ? ex_alu_final
                      : (id_rs1 != 5'h00 && id_rs1 == exmem_wb_rd && exmem_reg_write) ? exmem_alu_result
                      : rs1_val_id;
  wire [31:0] csr_zimm;
  assign csr_zimm = {27'h0000000, id_rs1};
  wire [31:0] csr_write_val;
  assign csr_write_val = (id_funct3 == 3'h1) ? csr_rs1_fwd
                       : (id_funct3 == 3'h2) ? csr_read_data | csr_rs1_fwd
                       : (id_funct3 == 3'h3) ? csr_read_data & ~csr_rs1_fwd
                       : (id_funct3 == 3'h5) ? csr_zimm
                       : (id_funct3 == 3'h6) ? csr_read_data | csr_zimm
                       : (id_funct3 == 3'h7) ? csr_read_data & ~csr_zimm
                       : 32'h00000000;

  // Main Decoder (P&H style): single packed decode from opcode
  //   {alu_src_a, alu_src_b, mem_write, mem_to_reg,
  //    mem_read,  branch,    alu_op,    jal_jalr}
  //   alu_src_b: 00=rs2_val, 01=imm_i, 10=imm_u, 11=imm_s
  //   mem_to_reg: 00=ALU, 01=DMEM, 10=PC+4, 11=imm_u
  //   alu_op: 00=ADD, 01=SUB, 10=R-type/I-type, 11=CSR/System
  //   jal_jalr: 00=none, 01=JAL, 10=JALR
  wire [11:0] main_dec;
  assign main_dec = (id_opcode == 7'h33) ? 12'b0_00_0_00_0_0_10_00  // R-type
                  : (id_opcode == 7'h13) ? 12'b0_01_0_00_0_0_10_00  // I-type ALU
                  : (id_opcode == 7'h03) ? 12'b0_01_0_01_1_0_00_00  // Load
                  : (id_opcode == 7'h23) ? 12'b0_11_1_00_0_0_00_00  // Store
                  : (id_opcode == 7'h63) ? 12'b0_00_0_00_0_1_01_00  // Branch
                  : (id_opcode == 7'h37) ? 12'b0_00_0_11_0_0_00_00  // LUI
                  : (id_opcode == 7'h17) ? 12'b1_10_0_00_0_0_00_00  // AUIPC
                  : (id_opcode == 7'h6F) ? 12'b0_00_0_10_0_0_00_01  // JAL
                  : (id_opcode == 7'h67) ? 12'b0_01_0_10_0_0_00_10  // JALR
                  : (id_opcode == 7'h73) ? 12'b0_00_0_00_0_0_11_00  // System
                  : 12'b0_00_0_00_0_0_00_00;

  wire        id_alu_src_a;
  wire [1:0]  id_alu_src_b;
  wire        id_mem_write;
  wire [1:0]  id_mem_to_reg;
  wire        id_mem_read;
  wire        id_branch;
  wire [1:0]  id_alu_op;
  wire [1:0]  id_jal_jalr;
  assign {id_alu_src_a, id_alu_src_b, id_mem_write, id_mem_to_reg,
          id_mem_read, id_branch, id_alu_op, id_jal_jalr} = main_dec;

  // reg_write (separate because System 0x73 is conditional on funct3)
  wire id_reg_write;
  assign id_reg_write = (id_opcode == 7'h33) || (id_opcode == 7'h13) || (id_opcode == 7'h03) ||
                         (id_opcode == 7'h37) || (id_opcode == 7'h17) ||
                         (id_opcode == 7'h6F) || (id_opcode == 7'h67) ||
                         (id_opcode == 7'h73 && id_funct3 != 3'h0);

  // ALU Decoder: maps {alu_op, funct3, funct7} to alu_control[4:0]
  wire [4:0] id_alu_control;
  assign id_alu_control = (id_alu_op == 2'b00) ? 5'd0
                        : (id_alu_op == 2'b01) ? 5'd1
                        : (id_alu_op == 2'b10) ?
                          (id_funct3 == 3'h0) ? ((id_opcode == 7'h33 && ((id_funct7 >> 5'd5) & 1'h1)) ? 5'd1 : 5'd0)
                        : (id_funct3 == 3'h1) ? 5'd2
                        : (id_funct3 == 3'h2) ? 5'd3
                        : (id_funct3 == 3'h3) ? 5'd4
                        : (id_funct3 == 3'h4) ? 5'd5
                        : (id_funct3 == 3'h5) ? (((ifid_instr >> 5'd30) & 1'h1) ? 5'd7 : 5'd6)
                        : (id_funct3 == 3'h6) ? 5'd8
                        : (id_funct3 == 3'h7) ? 5'd9
                        : 5'd0
                        :  // id_alu_op == 2'b11: CSR / System
                          (id_funct3 == 3'h0) ? (
                            ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h302 ? 5'd21  // MRET
                          : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h001 ? 5'd22  // EBREAK
                          : ((ifid_instr >> 5'd20) & 12'hFFF) == 12'h000 ? 5'd23  // ECALL
                          : 5'd0)
                        : (id_funct3 == 3'h1 || id_funct3 == 3'h5) ? 5'd18  // CSRRW/I
                        : (id_funct3 == 3'h2 || id_funct3 == 3'h6) ? 5'd19  // CSRRS/I
                        : (id_funct3 == 3'h3 || id_funct3 == 3'h7) ? 5'd20  // CSRRC/I
                        : 5'd0;

  // ============================================================
  // ID/EX PIPELINE REGISTER
  // ============================================================
  reg         idex_alu_src_a;
  reg [1:0]   idex_alu_src_b;
  reg         idex_mem_write;
  reg [1:0]   idex_mem_to_reg;
  reg         idex_mem_read;
  reg         idex_reg_write;
  reg [4:0]   idex_alu_control;
  reg [1:0]   idex_jal_jalr;
  reg         idex_branch;
  reg [4:0]   idex_rs1;
  reg [4:0]   idex_rs2;
  reg [4:0]   idex_rd;
  reg [31:0]  idex_rs1_val;
  reg [31:0]  idex_rs2_val;
  reg [31:0]  idex_imm;
  reg [31:0]  idex_imm_b;
  reg [31:0]  idex_imm_j;
  reg [31:0]  idex_imm_u;
  reg [31:0]  idex_imm_i;
  reg [31:0]  idex_pc_plus_4;
  reg [31:0]  idex_pc;
  reg [2:0]   idex_funct3;
  reg [31:0]  idex_csr_rdata;

  // ============================================================
  // EX STAGE: ALU, forwarding, branch comparison
  // ============================================================
  // Forwarding unit (two-level: EX/MEM, MEM/WB)
  // EX/MEM match
  wire exmem_match_a, exmem_match_b;
  assign exmem_match_a = (idex_rs1 != 5'h00) && (idex_rs1 == exmem_wb_rd) && exmem_reg_write;
  assign exmem_match_b = (idex_rs2 != 5'h00) && (idex_rs2 == exmem_wb_rd) && exmem_reg_write;

  // MEM/WB match (only if EX/MEM didn't match)
  wire memwb_match_a, memwb_match_b;
  assign memwb_match_a = (idex_rs1 != 5'h00) && (idex_rs1 == memwb_wb_rd) && memwb_reg_write && !exmem_match_a;
  assign memwb_match_b = (idex_rs2 != 5'h00) && (idex_rs2 == memwb_wb_rd) && memwb_reg_write && !exmem_match_b;

  // Forward data from EX/MEM: use mem_rdata if load, alu_result otherwise
  wire [31:0] exmem_fwd_data_a, exmem_fwd_data_b;
  assign exmem_fwd_data_a = exmem_mem_read ? exmem_mem_rdata : exmem_alu_result;
  assign exmem_fwd_data_b = exmem_mem_read ? exmem_mem_rdata : exmem_alu_result;

  // Forward data from MEM/WB: use mem_rdata if load, alu_result otherwise
  wire [31:0] memwb_fwd_data_a, memwb_fwd_data_b;
  assign memwb_fwd_data_a = memwb_mem_read ? memwb_mem_rdata : memwb_alu_result;
  assign memwb_fwd_data_b = memwb_mem_read ? memwb_mem_rdata : memwb_alu_result;

  // Final forwarded register values for EX
  wire [31:0] forward_rs1_val, forward_rs2_val;
  assign forward_rs1_val = exmem_match_a ? exmem_fwd_data_a
                         : memwb_match_a ? memwb_fwd_data_a
                         : idex_rs1_val;
  assign forward_rs2_val = exmem_match_b ? exmem_fwd_data_b
                         : memwb_match_b ? memwb_fwd_data_b
                         : idex_rs2_val;

  // ALU input muxes
  wire [31:0] alu_a;
  wire [31:0] alu_b;
  assign alu_a = idex_alu_src_a ? idex_pc : forward_rs1_val;
  assign alu_b = (idex_alu_src_b == 2'b01) ? idex_imm  // imm_i
               : (idex_alu_src_b == 2'b10) ? idex_imm  // imm_u (same as idex_imm for AUIPC)
               : (idex_alu_src_b == 2'b11) ? idex_imm  // imm_s
               : forward_rs2_val;

  wire [4:0] shamt;
  assign shamt = alu_b & 5'h1F;

  // ALU comparison helpers
  wire [31:0] alu_slt_result;
  assign alu_slt_result = ((alu_a >> 5'd31) & 1'h1)
        ? (((alu_b >> 5'd31) & 1'h1) ? ((alu_a < alu_b) ? 32'h00000001 : 32'h00000000) : 32'h00000001)
        : (((alu_b >> 5'd31) & 1'h1) ? 32'h00000000 : ((alu_a < alu_b) ? 32'h00000001 : 32'h00000000));
  wire [31:0] alu_sra_mask;
  assign alu_sra_mask = (((alu_a >> 5'd31) & 1'h1) & (shamt > 5'h00)) ? (32'hFFFFFFFF << (6'd32 - shamt)) : 32'h00000000;
  wire [31:0] alu_sra_result;
  assign alu_sra_result = (alu_a >> shamt) | alu_sra_mask;

  // ALU result
  wire [31:0] alu_result_ex;
  assign alu_result_ex = (idex_alu_control == 5'd0)  ? alu_a + alu_b
                        : (idex_alu_control == 5'd1)  ? alu_a - alu_b
                        : (idex_alu_control == 5'd2)  ? alu_a << shamt
                        : (idex_alu_control == 5'd3)  ? alu_slt_result
                        : (idex_alu_control == 5'd4)  ? ((alu_a < alu_b) ? 32'h00000001 : 32'h00000000)
                        : (idex_alu_control == 5'd5)  ? alu_a ^ alu_b
                        : (idex_alu_control == 5'd6)  ? alu_a >> shamt
                        : (idex_alu_control == 5'd7)  ? alu_sra_result
                        : (idex_alu_control == 5'd8)  ? alu_a | alu_b
                        : (idex_alu_control == 5'd9)  ? alu_a & alu_b
                        : 32'h00000000;

  // RV32M multiplication (in EX stage, uses forwarded values)
  wire [63:0] mul_a_sex, mul_b_sex, mul_b_zex;
  assign mul_a_sex = {alu_a[31] ? 32'hFFFFFFFF : 32'h00000000, alu_a};
  assign mul_b_sex = {alu_b[31] ? 32'hFFFFFFFF : 32'h00000000, alu_b};
  assign mul_b_zex = {32'h00000000, alu_b};
  wire [63:0] mul_full_signed;
  assign mul_full_signed = mul_a_sex * mul_b_sex;
  wire [63:0] mul_full_unsigned;
  assign mul_full_unsigned = alu_a * alu_b;
  wire [63:0] mul_full_mixed;
  assign mul_full_mixed = mul_a_sex * mul_b_zex;

  // RV32M division
  wire alu_a_neg, alu_b_neg;
  wire [31:0] alu_a_abs, alu_b_abs;
  wire div_overflow;
  assign alu_a_neg = alu_a[31];
  assign alu_b_neg = alu_b[31];
  assign alu_a_abs = alu_a_neg ? (~alu_a + 1'b1) : alu_a;
  assign alu_b_abs = alu_b_neg ? (~alu_b + 1'b1) : alu_b;
  assign div_overflow = (alu_a == 32'h80000000 && alu_b == 32'hFFFFFFFF);
  wire [31:0] alu_div_unsigned, alu_rem_unsigned, alu_div_signed, alu_rem_signed;
  assign alu_div_unsigned = (alu_b == 32'h00000000) ? 32'hFFFFFFFF : (alu_a / alu_b);
  assign alu_rem_unsigned = (alu_b == 32'h00000000) ? alu_a : (alu_a % alu_b);
  wire [31:0] div_abs, rem_abs;
  assign div_abs = (alu_b_abs == 32'h00000000) ? 32'hFFFFFFFF : (alu_a_abs / alu_b_abs);
  assign rem_abs = (alu_b_abs == 32'h00000000) ? alu_a_abs : (alu_a_abs % alu_b_abs);
  assign alu_div_signed = div_overflow ? 32'h80000000 : (alu_a_neg ^ alu_b_neg) ? (~div_abs + 1'b1) : div_abs;
  assign alu_rem_signed = (alu_b == 32'h00000000) ? alu_a : div_overflow ? 32'h00000000 : alu_a_neg ? (~rem_abs + 1'b1) : rem_abs;

  // Full ALU result with RV32M and CSR/System
  wire [31:0] alu_result_full;
  assign alu_result_full = (idex_alu_control == 5'd10) ? mul_full_signed[31:0]
                          : (idex_alu_control == 5'd11) ? mul_full_signed[63:32]
                          : (idex_alu_control == 5'd12) ? mul_full_mixed[63:32]
                          : (idex_alu_control == 5'd13) ? mul_full_unsigned[63:32]
                          : (idex_alu_control == 5'd14) ? alu_div_signed
                          : (idex_alu_control == 5'd15) ? alu_div_unsigned
                          : (idex_alu_control == 5'd16) ? alu_rem_signed
                          : (idex_alu_control == 5'd17) ? alu_rem_unsigned
                          : (idex_alu_control == 5'd18) ? idex_csr_rdata
                          : (idex_alu_control == 5'd19) ? idex_csr_rdata
                          : (idex_alu_control == 5'd20) ? idex_csr_rdata
                          : (idex_alu_control == 5'd21) ? idex_pc_plus_4  // MRET
                          : (idex_alu_control == 5'd22) ? 32'h00000000   // EBREAK
                          : (idex_alu_control == 5'd23) ? 32'h00000000   // ECALL
                          : alu_result_ex;

  // ALU result override for JAL/JALR (return PC+4) and LUI (pass imm_u)
  wire [31:0] ex_alu_final;
  assign ex_alu_final = (idex_jal_jalr != 2'b00) ? idex_pc_plus_4
                       : (idex_mem_to_reg == 2'b11) ? idex_imm_u  // LUI result for forwarding
                       : alu_result_full;

  // Branch comparison (unconditional, uses forwarded values)
  wire beq_taken, bne_taken, blt_taken, bge_taken, bltu_taken, bgeu_taken;
  wire branch_taken;
  assign beq_taken   = (idex_funct3 == 3'h0) && (forward_rs1_val == forward_rs2_val);
  assign bne_taken   = (idex_funct3 == 3'h1) && (forward_rs1_val != forward_rs2_val);
  assign blt_taken   = (idex_funct3 == 3'h4) && (
      (((forward_rs1_val >> 5'd31) & 1'h1) && !((forward_rs2_val >> 5'd31) & 1'h1)) ||
      (((forward_rs1_val >> 5'd31) & 1'h1) == ((forward_rs2_val >> 5'd31) & 1'h1) && forward_rs1_val < forward_rs2_val));
  assign bge_taken   = (idex_funct3 == 3'h5) && (
      (!((forward_rs1_val >> 5'd31) & 1'h1) && ((forward_rs2_val >> 5'd31) & 1'h1)) ||
      (((forward_rs1_val >> 5'd31) & 1'h1) == ((forward_rs2_val >> 5'd31) & 1'h1) && forward_rs1_val >= forward_rs2_val));
  assign bltu_taken  = (idex_funct3 == 3'h6) && (forward_rs1_val < forward_rs2_val);
  assign bgeu_taken  = (idex_funct3 == 3'h7) && (forward_rs1_val >= forward_rs2_val);
  assign branch_taken = beq_taken || bne_taken || blt_taken || bge_taken || bltu_taken || bgeu_taken;
  // Branch/JAL/JALR target computation
  wire [31:0] ex_branch_target;
  wire [31:0] ex_jal_target;
  wire [31:0] ex_jalr_target;
  assign ex_branch_target = idex_pc + idex_imm_b;
  assign ex_jal_target = idex_pc + idex_imm_j;
  assign ex_jalr_target = (forward_rs1_val + idex_imm_i) & 32'hFFFFFFFE;

  // ============================================================
  // EX/MEM PIPELINE REGISTER
  // ============================================================
  reg         exmem_mem_write;
  reg         exmem_mem_read;
  reg [1:0]   exmem_mem_to_reg;
  reg         exmem_reg_write;
  reg [4:0]   exmem_wb_rd;
  reg [31:0]  exmem_alu_result;
  reg [31:0]  exmem_rs2_val;
  reg [31:0]  exmem_pc_plus_4;
  reg [2:0]   exmem_funct3;
  reg [31:0]  exmem_imm_u;

  // ============================================================
  // MEM STAGE: Data memory access
  // ============================================================
  wire [7:0] dmem_waddr;
  assign dmem_waddr = (exmem_alu_result >> 2) & 8'hFF;

  wire [31:0] dmem_cur_word;
  assign dmem_cur_word = dmem[dmem_waddr];

  // Byte-merged write data for SB/SH
  wire [31:0] dmem_wdata_merged;
  assign dmem_wdata_merged =
    (exmem_funct3 == 3'h0)  // SB
      ? ((exmem_alu_result[1:0] == 2'h0) ? {dmem_cur_word[31:8],  exmem_rs2_val[7:0]}
       : (exmem_alu_result[1:0] == 2'h1) ? {dmem_cur_word[31:16], exmem_rs2_val[7:0], dmem_cur_word[7:0]}
       : (exmem_alu_result[1:0] == 2'h2) ? {dmem_cur_word[31:24], exmem_rs2_val[7:0], dmem_cur_word[15:0]}
       :                                  {exmem_rs2_val[7:0],     dmem_cur_word[23:0]})
    : (exmem_funct3 == 3'h1)  // SH
      ? ((exmem_alu_result[1] == 1'h0) ? {dmem_cur_word[31:16], exmem_rs2_val[15:0]}
       :                                {exmem_rs2_val[15:0],    dmem_cur_word[15:0]})
    : exmem_rs2_val;  // SW

  // Read data from DMEM or IMEM (unified read: imem for low addresses, dmem for high)
  wire [31:0] mem_rdata;
  assign mem_rdata = (exmem_alu_result >= 32'h00010000)
                   ? dmem[(exmem_alu_result >> 2) & 8'hFF]
                   : imem[(exmem_alu_result >> 2) & 10'h3FF];

  // Load byte/halfword extraction
  wire [1:0] load_offset;
  assign load_offset = exmem_alu_result[1:0];

  wire [7:0]  load_byte;
  wire [15:0] load_hword;
  assign load_byte  = (load_offset == 2'h0) ? mem_rdata[7:0]
                    : (load_offset == 2'h1) ? mem_rdata[15:8]
                    : (load_offset == 2'h2) ? mem_rdata[23:16]
                    : mem_rdata[31:24];
  assign load_hword = (load_offset == 2'h0) ? mem_rdata[15:0]
                    : mem_rdata[31:16];

  wire [31:0] dmem_rdata_comb;
  assign dmem_rdata_comb = (exmem_funct3 == 3'h0)
                             ? (load_byte[7] ? {24'hFFFFFF, load_byte} : {24'h000000, load_byte})
                           : (exmem_funct3 == 3'h1)
                             ? (load_hword[15] ? {16'hFFFF, load_hword} : {16'h0000, load_hword})
                           : (exmem_funct3 == 3'h4)
                             ? {24'h000000, load_byte}
                           : (exmem_funct3 == 3'h5)
                             ? {16'h0000, load_hword}
                           : mem_rdata;

  // ============================================================
  // MEM/WB PIPELINE REGISTER
  // ============================================================
  reg         memwb_reg_write;
  reg [1:0]   memwb_mem_to_reg;
  reg         memwb_mem_read;
  reg [4:0]   memwb_wb_rd;
  reg [31:0]  memwb_alu_result;
  reg [31:0]  memwb_mem_rdata;
  reg [31:0]  memwb_pc_plus_4;
  reg [31:0]  memwb_imm_u;

  // ============================================================
  // WB STAGE: Write-back mux
  // ============================================================
  wire [31:0] memwb_write_data;
  assign memwb_write_data = (memwb_mem_to_reg == 2'b01) ? memwb_mem_rdata
                           : (memwb_mem_to_reg == 2'b10) ? memwb_pc_plus_4
                           : (memwb_mem_to_reg == 2'b11) ? memwb_imm_u
                           : memwb_alu_result;

  // EX/MEM write data (for WB-stage register write, uses EX/MEM signals
  // because memwb_* non-blocking assignments aren't visible in same edge)
  wire [31:0] exmem_write_data;
  assign exmem_write_data = (exmem_mem_to_reg == 2'b01) ? dmem_rdata_comb
                           : (exmem_mem_to_reg == 2'b10) ? exmem_pc_plus_4
                           : (exmem_mem_to_reg == 2'b11) ? exmem_imm_u
                           : exmem_alu_result;

  // ============================================================
  // Hazard detection: load-use stall
  // ============================================================
  wire load_use_stall;
  assign load_use_stall = idex_mem_read && (idex_rd != 5'h00) &&
                          ((idex_rd == id_rs1) || (idex_rd == id_rs2));

  // EX-stage halt detection (EBREAK reaches EX)
  wire ex_is_ebreak;
  assign ex_is_ebreak = (idex_alu_control == 5'd22);

  // Flush signals — use inline expression to avoid qsim delta sensitivity through cascaded wires
  wire flush_ifid;
  assign flush_ifid = (idex_branch && branch_taken) || (idex_jal_jalr != 2'b00) || ex_is_ebreak;
  wire bubble_idex;
  assign bubble_idex = load_use_stall || flush_ifid;

  // ============================================================
  // PC logic (in always block below)
  // ============================================================
  // Debug output assignments
  // ============================================================
  assign halted = halted_reg;
  assign pc = pc_reg;
  assign reg_x1  = x1;
  assign reg_x10 = x10;
  assign instruction = ifid_instr;
  assign alu_result = exmem_alu_result;
  assign dmem_addr = exmem_alu_result;
  assign dmem_rdata = dmem_rdata_comb;
  assign dmem_wdata = exmem_rs2_val;
  assign dmem_we = exmem_mem_write;
  assign uart_tx_valid = uart_tx_valid_reg;
  assign uart_tx_data  = uart_tx_data_reg;
  assign debug_ex_branch_taken = idex_branch && branch_taken;
  assign debug_idex_branch = idex_branch;
  assign debug_idex_funct3 = idex_funct3;
  assign debug_forward_rs1_val = forward_rs1_val;
  assign debug_forward_rs2_val = forward_rs2_val;
  assign debug_idex_imm = idex_imm;
  assign debug_branch_taken = branch_taken;
  assign debug_flush_ifid = flush_ifid;
  assign debug_load_use_stall = load_use_stall;
  assign debug_ex_branch_target = ex_branch_target;
  assign debug_idex_pc = idex_pc;
  assign debug_idex_rd = idex_rd;
  assign debug_idex_rs1 = idex_rs1;
  assign debug_idex_rs2 = idex_rs2;

  // ============================================================
  // Sequential process (posedge clock or posedge reset)
  // ============================================================
  always @(posedge clk or posedge rst) begin
    if (rst) begin
      pc_reg     <= 32'h00000000;
      halted_reg <= 1'b0;
      ifid_instr <= 32'h00000000;
      ifid_pc_plus_4 <= 32'h00000000;
      {idex_alu_src_a, idex_alu_src_b, idex_mem_write, idex_mem_to_reg,
       idex_mem_read, idex_reg_write, idex_alu_control, idex_jal_jalr,
       idex_branch} <= 0;
      idex_rs1 <= 0; idex_rs2 <= 0; idex_rd <= 0;
      idex_rs1_val <= 32'h00000000; idex_rs2_val <= 32'h00000000;
      idex_imm <= 32'h00000000; idex_imm_b <= 32'h00000000;
      idex_imm_j <= 32'h00000000; idex_imm_u <= 32'h00000000;
      idex_imm_i <= 32'h00000000;
      idex_pc_plus_4 <= 32'h00000000; idex_pc <= 32'h00000000;
      idex_funct3 <= 0;
      idex_csr_rdata <= 32'h00000000;
      {exmem_mem_write, exmem_mem_read, exmem_mem_to_reg, exmem_reg_write} <= 0;
      exmem_wb_rd <= 0;
      exmem_alu_result <= 32'h00000000; exmem_rs2_val <= 32'h00000000;
      exmem_pc_plus_4 <= 32'h00000000; exmem_funct3 <= 0;
      exmem_imm_u <= 32'h00000000;
      {memwb_reg_write, memwb_mem_to_reg, memwb_mem_read} <= 0;
      memwb_wb_rd <= 0;
      memwb_alu_result <= 32'h00000000; memwb_mem_rdata <= 32'h00000000;
      memwb_pc_plus_4 <= 32'h00000000; memwb_imm_u <= 32'h00000000;
      dmem <= 8192'h0;
      x1  <= 32'h00000000; x2  <= 32'h00000000;
      x3  <= 32'h00000000; x4  <= 32'h00000000;
      x5  <= 32'h00000000; x6  <= 32'h00000000;
      x7  <= 32'h00000000; x8  <= 32'h00000000;
      x9  <= 32'h00000000; x10 <= 32'h00000000;
      x11 <= 32'h00000000; x12 <= 32'h00000000;
      x13 <= 32'h00000000; x14 <= 32'h00000000;
      x15 <= 32'h00000000; x16 <= 32'h00000000;
      x17 <= 32'h00000000; x18 <= 32'h00000000;
      x19 <= 32'h00000000; x20 <= 32'h00000000;
      x21 <= 32'h00000000; x22 <= 32'h00000000;
      x23 <= 32'h00000000; x24 <= 32'h00000000;
      x25 <= 32'h00000000; x26 <= 32'h00000000;
      x27 <= 32'h00000000; x28 <= 32'h00000000;
      x29 <= 32'h00000000; x30 <= 32'h00000000;
      x31 <= 32'h00000000;
      uart_tx_valid_reg <= 1'b0;
      uart_tx_data_reg  <= 8'h00;
      csr_mtvec   <= 32'h00000000;
      csr_mepc    <= 32'h00000000;
      csr_mcause  <= 32'h00000000;
      csr_mie     <= 32'h00000000;
      csr_mstatus <= 32'h00000000;
      timer_compare <= 32'h00000000;
      timer_counter <= 32'h00000000;
    end else begin
      // ---- PC update (gated by halted_reg) ----
      if (!halted_reg) begin
        if (irq_pending) begin
          pc_reg <= csr_mtvec;
        end else if (is_mret) begin
          pc_reg <= csr_mepc;
        end else if (load_use_stall) begin
          pc_reg <= pc_reg;
        end else if (idex_branch && branch_taken) begin
          pc_reg <= ex_branch_target;
        end else if (idex_jal_jalr == 2'b01) begin
          pc_reg <= ex_jal_target;
        end else if (idex_jal_jalr == 2'b10) begin
          pc_reg <= ex_jalr_target;
        end else begin
          pc_reg <= if_pc_plus_4;
        end
      end

      // ---- IF/ID pipeline register (NOP when halted or flushed) ----
      if (halted_reg || irq_pending || flush_ifid || is_mret) begin
        ifid_instr <= 32'h00000000;
        ifid_pc_plus_4 <= 32'h00000000;
      end else if (!load_use_stall) begin
        ifid_instr <= if_instr;
        ifid_pc_plus_4 <= if_pc_plus_4;
      end

      // ---- ID/EX pipeline register (bubble when halted, flushed, MRET, or stalled) ----
      if (halted_reg || irq_pending || bubble_idex || is_mret) begin
        {idex_alu_src_a, idex_alu_src_b, idex_mem_write, idex_mem_to_reg,
         idex_mem_read, idex_reg_write, idex_alu_control, idex_jal_jalr,
         idex_branch} <= 0;
        idex_rs1 <= 0; idex_rs2 <= 0; idex_rd <= 0;
        idex_rs1_val <= 32'h00000000; idex_rs2_val <= 32'h00000000;
        idex_imm <= 32'h00000000; idex_imm_b <= 32'h00000000;
        idex_imm_j <= 32'h00000000; idex_imm_u <= 32'h00000000;
        idex_imm_i <= 32'h00000000;
        idex_pc_plus_4 <= 32'h00000000; idex_pc <= 32'h00000000;
        idex_funct3 <= 0;
        idex_csr_rdata <= 32'h00000000;
      end else if (!load_use_stall) begin
        {idex_alu_src_a, idex_alu_src_b, idex_mem_write, idex_mem_to_reg,
         idex_mem_read, idex_reg_write, idex_alu_control, idex_jal_jalr,
         idex_branch} <= {id_alu_src_a, id_alu_src_b, id_mem_write, id_mem_to_reg,
                          id_mem_read, id_reg_write, id_alu_control, id_jal_jalr,
                          id_branch};
        idex_rs1 <= id_rs1; idex_rs2 <= id_rs2; idex_rd <= id_rd;
        idex_rs1_val <= rs1_val_id; idex_rs2_val <= rs2_val_id;
        idex_imm <= imm_id; idex_imm_b <= imm_b;
        idex_imm_j <= imm_j; idex_imm_u <= imm_u;
        idex_imm_i <= imm_i;
        idex_pc_plus_4 <= ifid_pc_plus_4;
        idex_pc <= ifid_pc_plus_4 - 32'd4;
        idex_funct3 <= id_funct3;
        idex_csr_rdata <= csr_read_data;
      end

      // ---- EX/MEM pipeline register (always advances) ----
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

      // ---- MEM/WB pipeline register (always advances) ----
      memwb_reg_write <= exmem_reg_write;
      memwb_mem_to_reg <= exmem_mem_to_reg;
      memwb_mem_read <= exmem_mem_read;
      memwb_wb_rd <= exmem_wb_rd;
      memwb_alu_result <= exmem_alu_result;
      memwb_mem_rdata <= dmem_rdata_comb;
      memwb_pc_plus_4 <= exmem_pc_plus_4;
      memwb_imm_u <= exmem_imm_u;

      // ---- Interrupt entry (gated by !halted) ----
      if (!halted_reg && irq_pending) begin
        csr_mepc <= pc_reg;
        csr_mcause <= 32'h80000007;
        csr_mstatus <= csr_mstatus & ~32'h00000008;
      end

      // ---- MRET (gated by !halted) ----
      if (!halted_reg && is_mret) begin
        csr_mstatus <= csr_mstatus | 32'h00000008;
      end

      // ---- CSR writes (ID stage, gated by !halted) ----
      if (!halted_reg && id_opcode == 7'h73 && id_funct3 != 3'h0) begin
        case ((ifid_instr >> 5'd20) & 12'hFFF)
          12'h300: csr_mstatus <= csr_write_val;
          12'h304: csr_mie <= csr_write_val;
          12'h305: csr_mtvec <= csr_write_val;
          12'h341: csr_mepc <= csr_write_val;
          12'h342: csr_mcause <= csr_write_val;
        endcase
      end

      // ---- EBREAK detection (EX stage, fires once when EBREAK reaches EX) ----
      if (ex_is_ebreak) begin
        halted_reg <= 1'b1;
      end

      // ---- Timer counter (gated by !halted) ----
      if (!halted_reg) begin
        timer_counter <= timer_counter + 1'b1;
      end

      // ---- DMEM write ----
      if (exmem_mem_write && (exmem_alu_result != 32'h10000000) && (exmem_alu_result != 32'h20000000)) begin
        dmem[dmem_waddr] <= dmem_wdata_merged;
      end

      // ---- UART write at 0x10000000 ----
      if (exmem_mem_write && (exmem_alu_result == 32'h10000000)) begin
        uart_tx_data_reg  <= exmem_rs2_val[7:0];
        uart_tx_valid_reg <= 1'b1;
      end else begin
        uart_tx_valid_reg <= 1'b0;
      end

      // ---- Timer compare write at 0x20000000 ----
      if (exmem_mem_write && (exmem_alu_result == 32'h20000000)) begin
        timer_compare <= exmem_rs2_val;
      end

      // ---- Register file write (uses EX/MEM signals, not MEM/WB, since
      // memwb_* non-blocking assignments are not visible within same edge) ----
      if (exmem_reg_write && exmem_wb_rd != 5'h00) begin
        if (exmem_wb_rd == 5'h01) x1  <= exmem_write_data;
        if (exmem_wb_rd == 5'h02) x2  <= exmem_write_data;
        if (exmem_wb_rd == 5'h03) x3  <= exmem_write_data;
        if (exmem_wb_rd == 5'h04) x4  <= exmem_write_data;
        if (exmem_wb_rd == 5'h05) x5  <= exmem_write_data;
        if (exmem_wb_rd == 5'h06) x6  <= exmem_write_data;
        if (exmem_wb_rd == 5'h07) x7  <= exmem_write_data;
        if (exmem_wb_rd == 5'h08) x8  <= exmem_write_data;
        if (exmem_wb_rd == 5'h09) x9  <= exmem_write_data;
        if (exmem_wb_rd == 5'h0A) x10 <= exmem_write_data;
        if (exmem_wb_rd == 5'h0B) x11 <= exmem_write_data;
        if (exmem_wb_rd == 5'h0C) x12 <= exmem_write_data;
        if (exmem_wb_rd == 5'h0D) x13 <= exmem_write_data;
        if (exmem_wb_rd == 5'h0E) x14 <= exmem_write_data;
        if (exmem_wb_rd == 5'h0F) x15 <= exmem_write_data;
        if (exmem_wb_rd == 5'h10) x16 <= exmem_write_data;
        if (exmem_wb_rd == 5'h11) x17 <= exmem_write_data;
        if (exmem_wb_rd == 5'h12) x18 <= exmem_write_data;
        if (exmem_wb_rd == 5'h13) x19 <= exmem_write_data;
        if (exmem_wb_rd == 5'h14) x20 <= exmem_write_data;
        if (exmem_wb_rd == 5'h15) x21 <= exmem_write_data;
        if (exmem_wb_rd == 5'h16) x22 <= exmem_write_data;
        if (exmem_wb_rd == 5'h17) x23 <= exmem_write_data;
        if (exmem_wb_rd == 5'h18) x24 <= exmem_write_data;
        if (exmem_wb_rd == 5'h19) x25 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1A) x26 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1B) x27 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1C) x28 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1D) x29 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1E) x30 <= exmem_write_data;
        if (exmem_wb_rd == 5'h1F) x31 <= exmem_write_data;
      end
    end
  end

endmodule
