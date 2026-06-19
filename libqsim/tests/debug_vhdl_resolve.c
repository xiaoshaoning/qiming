#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *src =
        "entity multi_driver is\n"
        "  port (a: in std_logic; b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of multi_driver is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  p1: process(a) is\n"
        "  begin\n"
        "    s <= a;\n"
        "  end process;\n"
        "  p2: process(b) is\n"
        "  begin\n"
        "    s <= b;\n"
        "  end process;\n"
        "  y <= s;\n"
        "end architecture;\n";

    qsim_session_t *sess = qsim_session_create();
    printf("compile: %d\n", qsim_session_compile_string(sess, "test.vhd", src));
    printf("elab: %d\n", qsim_session_elaborate(sess));

    char *v;
    v = qsim_session_eval_str(sess, "a");
    printf("a before: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "b");
    printf("b before: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "s");
    printf("s before: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "y");
    printf("y before: '%s'\n", v ? v : "NULL"); free(v);

    qsim_session_force_str(sess, "a", "1");
    qsim_session_force_str(sess, "b", "0");
    qsim_session_step_delta(sess);

    v = qsim_session_eval_str(sess, "a");
    printf("a after force 1: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "b");
    printf("b after force 0: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "s");
    printf("s after step: '%s'\n", v ? v : "NULL"); free(v);
    v = qsim_session_eval_str(sess, "y");
    printf("y after step: '%s' (len=%zu)\n", v ? v : "NULL", v ? strlen(v) : 0);
    if (v) {
        for (size_t i = 0; i < strlen(v); i++)
            printf("  y[%zu] = %d (0x%02x '%c')\n", i, v[i], (unsigned char)v[i],
                   v[i] >= 32 && v[i] < 127 ? v[i] : '.');
    }
    free(v);

    qsim_session_free(sess);
    return 0;
}
