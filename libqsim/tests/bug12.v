module bug12(
  input clk, input rst_n,
  input weight_load_en,
  input [15:0] weight_in,
  input [15:0] act_in,
  input [31:0] acc_in,
  output reg [15:0] act_out,
  output reg [31:0] acc_out);

  reg [15:0] weight_reg;

  always @(posedge clk) begin
    if (!rst_n) begin
      weight_reg <= 0;
      acc_out <= 0;
      act_out <= 0;
    end else begin
      if (weight_load_en)
        weight_reg <= weight_in;
      acc_out <= acc_in + (act_in * weight_reg);
      act_out <= act_in;
    end
  end
endmodule
