/* More complex ID/EX test: three-level nesting like rv32i_top.v */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "libqsim/session.h"

static const char *make_src(int use_ifelse) {
    if (use_ifelse) {
        return
        "module test;\n"
        "  reg clk, rst;\n"
        "  reg [31:0] pc_reg;\n"
        "  reg [31:0] ifid_instr;\n"
        "  reg [31:0] ifid_pc_plus_4;\n"
        "  reg [4:0] idex_rd;\n"
        "  reg [4:0] idex_rs2;\n"
        "  reg [31:0] idex_pc;\n"
        "  reg [2:0] idex_alu_control;\n"
        "  reg idex_branch;\n"
        "  reg halted_reg;\n"
        "  wire load_use_stall;\n"
        "  wire bubble_idex;\n"
        "  assign load_use_stall = 1'b0;\n"
        "  assign bubble_idex = 1'b0;\n"
        "  wire [4:0] id_rd;\n"
        "  wire [4:0] id_rs2;\n"
        "  assign id_rd  = (ifid_instr >> 4'd7) & 5'h1F;\n"
        "  assign id_rs2 = (ifid_instr >> 5'd20) & 5'h1F;\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin\n"
        "      pc_reg <= 0; ifid_instr <= 0; ifid_pc_plus_4 <= 0;\n"
        "      {idex_alu_control, idex_branch} <= 0;\n"
        "      idex_rd <= 0; idex_rs2 <= 0; idex_pc <= 0;\n"
        "      halted_reg <= 0;\n"
        "    end else begin\n"
        "      pc_reg <= pc_reg + 4;\n"
        "      ifid_instr <= 32'h02A00513;\n"
        "      ifid_pc_plus_4 <= pc_reg + 4;\n"
        /* Three-level if-else for ID/EX, matching rv32i_top pattern */
        "      if (halted_reg || bubble_idex) begin\n"
        "        {idex_alu_control, idex_branch} <= 0;\n"
        "        idex_rd <= 0; idex_rs2 <= 0; idex_pc <= 0;\n"
        "      end else if (!load_use_stall) begin\n"
        "        {idex_alu_control, idex_branch} <= {3'd0, 1'b0};\n"
        "        idex_rd <= id_rd;\n"
        "        idex_rs2 <= id_rs2;\n"
        "        idex_pc <= ifid_pc_plus_4;\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "endmodule\n";
    } else {
        return
        "module test;\n"
        "  reg clk, rst;\n"
        "  reg [31:0] pc_reg;\n"
        "  reg [31:0] ifid_instr;\n"
        "  reg [31:0] ifid_pc_plus_4;\n"
        "  reg [4:0] idex_rd;\n"
        "  reg [4:0] idex_rs2;\n"
        "  reg [31:0] idex_pc;\n"
        "  reg [2:0] idex_alu_control;\n"
        "  reg idex_branch;\n"
        "  wire [4:0] id_rd;\n"
        "  wire [4:0] id_rs2;\n"
        "  assign id_rd  = (ifid_instr >> 4'd7) & 5'h1F;\n"
        "  assign id_rs2 = (ifid_instr >> 5'd20) & 5'h1F;\n"
        "  always @(posedge clk or posedge rst) begin\n"
        "    if (rst) begin\n"
        "      pc_reg <= 0; ifid_instr <= 0; ifid_pc_plus_4 <= 0;\n"
        "      {idex_alu_control, idex_branch} <= 0;\n"
        "      idex_rd <= 0; idex_rs2 <= 0; idex_pc <= 0;\n"
        "    end else begin\n"
        "      pc_reg <= pc_reg + 4;\n"
        "      ifid_instr <= 32'h02A00513;\n"
        "      ifid_pc_plus_4 <= pc_reg + 4;\n"
        /* Flat block, all assignments direct */
        "      {idex_alu_control, idex_branch} <= {3'd0, 1'b0};\n"
        "      idex_rd <= id_rd;\n"
        "      idex_rs2 <= id_rs2;\n"
        "      idex_pc <= ifid_pc_plus_4;\n"
        "    end\n"
        "  end\n"
        "endmodule\n";
    }
}

static int run_test(int use_ifelse, const char *label) {
    printf("\n=== %s ===\n", label);
    const char *src = make_src(use_ifelse);

    qsim_session_t *sess = qsim_session_create();
    if (!sess) { printf("FAIL: create\n"); return 0; }

    int ok = qsim_session_compile_string(sess, "test.v", src);
    if (!ok) { printf("FAIL: compile\n"); qsim_session_free(sess); return 0; }

    ok = qsim_session_elaborate(sess);
    if (!ok) { printf("FAIL: elaborate\n"); qsim_session_free(sess); return 0; }

    /* reset */
    qsim_session_set_str(sess, "rst", "0"); qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "1"); qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "rst", "0"); qsim_session_step_delta(sess);

    /* Cycle 1 */
    qsim_session_set_str(sess, "clk", "0"); qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "1"); qsim_session_step_delta(sess);

    char *rd  = qsim_session_eval_str(sess, "idex_rd");
    char *rs2 = qsim_session_eval_str(sess, "idex_rs2");
    printf("cyc1: idex_rd=%s, idex_rs2=%s\n", rd ? rd : "?", rs2 ? rs2 : "?");
    free(rd); free(rs2);

    /* Cycle 2: id_rd/id_rs2 wires now reflect the ifid_instr loaded in cycle 1 */
    qsim_session_set_str(sess, "clk", "0"); qsim_session_step_delta(sess);
    qsim_session_set_str(sess, "clk", "1"); qsim_session_step_delta(sess);

    rd  = qsim_session_eval_str(sess, "idex_rd");
    rs2 = qsim_session_eval_str(sess, "idex_rs2");
    printf("cyc2: idex_rd=%s, idex_rs2=%s\n", rd ? rd : "?", rs2 ? rs2 : "?");

    int pass_rd  = rd  && strcmp(rd, "01010") == 0;
    int pass_rs2 = rs2 && strcmp(rs2, "01010") == 0;
    free(rd); free(rs2);

    qsim_session_free(sess);
    printf("%s: %s\n", label, (pass_rd && pass_rs2) ? "PASS" : "FAIL");
    return pass_rd && pass_rs2;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int p1 = run_test(0, "flat block");
    int p2 = run_test(1, "if-else-if block");
    printf("\n%s\n", (p1 && p2) ? "ALL PASS" : "SOME FAIL");
    return (p1 && p2) ? 0 : 1;
}
