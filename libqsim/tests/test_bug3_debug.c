#include "libqsim/session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *src =
    "module bug3;\n"
    "  parameter ACTIVATIONS = 4;\n"
    "  reg [3:0] bug3_cnt;\n"
    "  reg [7:0] bug3_result;\n"
    "  initial begin\n"
    "    bug3_cnt = 4'd3;\n"
    "    if (bug3_cnt == ACTIVATIONS - 1)\n"
    "      bug3_result = 8'hDD;\n"
    "    else\n"
    "      bug3_result = 8'hEE;\n"
    "  end\n"
    "endmodule\n";

    qsim_session_t *sess = qsim_session_create();
    printf("compile: %d\n", qsim_session_compile_string(sess, "bug3.v", src));
    printf("elaborate: %d\n", qsim_session_elaborate(sess));

    char *v = qsim_session_eval_str(sess, "bug3_cnt");
    printf("bug3_cnt before step: %s\n", v ? v : "NULL");
    free(v);

    v = qsim_session_eval_str(sess, "bug3_result");
    printf("bug3_result before step: %s\n", v ? v : "NULL");
    free(v);

    v = qsim_session_eval_str(sess, "ACTIVATIONS");
    printf("ACTIVATIONS: %s\n", v ? v : "NULL");
    free(v);

    /* Also try to check param directly by checking signal-like names */
    v = qsim_session_eval_str(sess, "bug3.ACTIVATIONS");
    printf("bug3.ACTIVATIONS: %s\n", v ? v : "NULL");
    free(v);

    v = qsim_session_eval_str(sess, "ACTIVATIONS.value");
    printf("ACTIVATIONS.value: %s\n", v ? v : "NULL");

    qsim_session_step_delta(sess);

    v = qsim_session_eval_str(sess, "bug3_cnt");
    printf("bug3_cnt after step: %s\n", v ? v : "NULL");
    free(v);

    v = qsim_session_eval_str(sess, "bug3_result");
    printf("bug3_result after step: %s\n", v ? v : "NULL");
    free(v);

    qsim_session_free(sess);
    return 0;
}
