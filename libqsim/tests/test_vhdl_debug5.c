/* Minimal VHDL process debug: test if process(clk, rst) fires via session API */
#include "libqsim/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show(qsim_session_t *s, const char *n) {
    char *v = qsim_session_eval_str(s, n);
    printf("  %s=%s\n", n, v ? v : "NULL");
    free(v);
}

int main(void) {
    const char *src =
        "entity top is\n"
        "  port (clk: in bit; rst: in bit; q: out bit);\n"
        "end entity;\n"
        "architecture rtl of top is\n"
        "  signal s_q: bit;\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_q <= '0';\n"
        "    elsif clk = '1' then\n"
        "      s_q <= '1';\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s_q;\n"
        "end architecture;\n";

    qsim_session_t *sess = qsim_session_create();
    int ok = qsim_session_compile_string(sess, "top.vhd", src);
    printf("compile: %d\n", ok);
    ok = qsim_session_elaborate(sess);
    printf("elab: %d\n", ok);

    printf("Signals:\n");
    size_t n = qsim_session_get_signal_count(sess);
    for (size_t i = 0; i < n; i++) {
        const char *name = qsim_session_get_signal_name(sess, i);
        printf("  [%zu] %s\n", i, name ? name : "NULL");
    }

    printf("\nInitial:\n");
    show(sess, "rst");
    show(sess, "clk");
    show(sess, "q");

    printf("\nSet rst=1:\n");
    qsim_session_set_str(sess, "rst", "1");
    qsim_session_step_delta(sess);
    show(sess, "rst");
    show(sess, "q");

    /* Step more deltas */
    for (int i = 0; i < 5; i++) {
        qsim_session_step_delta(sess);
        printf("  q (step %d)=", i+1); show(sess, "q");
    }

    printf("\nSet rst=0, clk=1:\n");
    qsim_session_set_str(sess, "rst", "0");
    qsim_session_set_str(sess, "clk", "1");
    qsim_session_step_delta(sess);
    show(sess, "clk");
    show(sess, "q");

    qsim_session_free(sess);
    return 0;
}
