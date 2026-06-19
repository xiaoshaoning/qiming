// RV32I testbench -- minimal structural wrapper for qsim
// Clock, reset driven by test harness.

module rv32i_tb;

  reg clk;
  reg rst;

  wire halted;
  wire [31:0] pc;
  wire [31:0] instruction;
  wire [31:0] reg_x1;
  wire [31:0] reg_x10;
  wire [31:0] alu_result;
  wire [31:0] dmem_addr;
  wire [31:0] dmem_rdata;
  wire [31:0] dmem_wdata;
  wire        dmem_we;

  rv32i_top cpu (
    .clk(clk),
    .rst(rst),
    .halted(halted),
    .pc(pc),
    .instruction(instruction),
    .reg_x1(reg_x1),
    .reg_x10(reg_x10),
    .alu_result(alu_result),
    .dmem_addr(dmem_addr),
    .dmem_rdata(dmem_rdata),
    .dmem_wdata(dmem_wdata),
    .dmem_we(dmem_we)
  );

endmodule
