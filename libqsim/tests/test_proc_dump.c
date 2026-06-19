/* Standalone test: dump process info for RV32I VHDL CPU */
#include "minunit.h"
#include "libqsim/session.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static size_t read_rv32i_vhdl(size_t *out_len, char **out) {
    const char *paths[] = {"../../example/rv32i_vhdl/rv32i_top.vhd",
                           "../../../example/rv32i_vhdl/rv32i_top.vhd",
                           "example/rv32i_vhdl/rv32i_top.vhd",
                           "../example/rv32i_vhdl/rv32i_top.vhd",
                           "../libqsim/../example/rv32i_vhdl/rv32i_top.vhd"};
    FILE *f = NULL;
    for (int i = 0; i < 5; i++) {
        f = fopen(paths[i], "rb");
        if (f) break;
    }
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)len + 1);
    if (!src) { fclose(f); return 0; }
    size_t nread = fread(src, 1, (size_t)len, f);
    src[nread] = '\0';
    fclose(f);
    *out = src;
    *out_len = nread;
    return (size_t)len;
}

static void test_proc_dump(void) {
    size_t src_len;
    char *src;
    if (!read_rv32i_vhdl(&src_len, &src)) {
        printf("FAIL: could not read rv32i_top.vhd\n");
        return;
    }
    qsim_session_t *sess = qsim_session_create();
    if (!sess) { free(src); return; }
    int ok = qsim_session_compile_string(sess, "rv32i_top.vhd", src);
    free(src);
    if (!ok) { qsim_session_free(sess); printf("FAIL: compile\n"); return; }

    /* Before elaboration, dump process info from the compiled unit */
    printf("=== Session units: ===\n");
    /* We can't access internal session state, so just elaborate and run */
    ok = qsim_session_elaborate(sess);
    if (!ok) { qsim_session_free(sess); printf("FAIL: elaborate\n"); return; }

    printf("Elaboration succeeded.\n");

    /* Force imem, reset, run a few cycles */
    char imem_str[4097];
    memset(imem_str, '0', 4096);
    imem_str[4096] = '\0';
    qsim_session_force_str(sess, "imem_port", imem_str);

    qsim_session_set_port_str(sess, "rst", "1");
    qsim_session_set_port_str(sess, "clk", "0");
    qsim_session_eval(sess);

    qsim_session_set_port_str(sess, "rst", "0");

    /* Read debug signals */
    for (int cycle = 0; cycle < 5; cycle++) {
        qsim_session_set_port_str(sess, "clk", "1");
        qsim_session_eval(sess);
        qsim_session_set_port_str(sess, "clk", "0");
        qsim_session_eval(sess);

        char *pcv = qsim_session_eval_str(sess, "pc");
        char *alu_ex = qsim_session_eval_str(sess, "alu_result_ex");
        printf("cycle=%d pc=%s alu_result_ex=%s\n", cycle,
               pcv ? pcv : "?", alu_ex ? alu_ex : "?");
        free(pcv);
        free(alu_ex);
    }

    qsim_session_free(sess);
    printf("PASS: test_proc_dump\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    test_proc_dump();
    return 0;
}
