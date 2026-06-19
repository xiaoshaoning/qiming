// Minimal test: ID/EX pipeline register behavior
module idex_test (
    input clk, rst,
    output [4:0] idex_rd_out,
    output [4:0] idex_rs2_out,
    output [4:0] id_rd_out,
    output [4:0] id_rs2_out,
    output [4:0] ifid_rd,
    output [4:0] ifid_rs2
);
    reg [31:0] imem [0:3];
    initial begin
        imem[0] = 32'h02A00513;  // ADDI x10, x0, 42
        imem[1] = 32'h00100073;  // EBREAK
        imem[2] = 32'h00000000;
        imem[3] = 32'h00000000;
    end

    reg [31:0] pc_reg;
    reg [31:0] ifid_instr;

    // ID stage decoders
    wire [4:0] id_rd_w, id_rs2_w;
    assign id_rd_w  = (ifid_instr >> 4'd7) & 5'h1F;
    assign id_rs2_w = (ifid_instr >> 5'd20) & 5'h1F;

    // ID/EX pipeline
    reg [2:0]  idex_alu_control;
    reg        idex_branch;
    reg [4:0]  idex_rd;
    reg [4:0]  idex_rs2;
    reg [31:0] idex_pc;

    assign idex_rd_out = idex_rd;
    assign idex_rs2_out = idex_rs2;
    assign id_rd_out = id_rd_w;
    assign id_rs2_out = id_rs2_w;
    assign ifid_rd = id_rd_w;
    assign ifid_rs2 = id_rs2_w;

    wire [31:0] if_instr;
    assign if_instr = imem[pc_reg >> 2];

    always @(posedge clk or posedge rst) begin
        if (rst) begin
            pc_reg <= 0;
            ifid_instr <= 0;
            {idex_alu_control, idex_branch} <= 0;
            idex_rd <= 0; idex_rs2 <= 0;
            idex_pc <= 0;
        end else begin
            // PC
            pc_reg <= pc_reg + 4;

            // IF/ID
            ifid_instr <= if_instr;

            // ID/EX
            // Simple control: bubble=false, capture enabled
            {idex_alu_control, idex_branch} <= {3'd0, 1'b0};  // packed concat
            idex_rd <= id_rd_w;
            idex_rs2 <= id_rs2_w;
            idex_pc <= pc_reg;
        end
    end
endmodule
