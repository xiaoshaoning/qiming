/* VITAL primitive simulation tests — compile, elaborate, simulate, verify. */
#include "minunit.h"
#include "libqsim/vhdl_parser.h"
#include "libqsim/uir.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir_sim.h"
#include "libqsim/value.h"
#include <string.h>
#include <stdint.h>

/* =================================================================
 * 1. All VITAL primitives in one entity
 * ================================================================= */

static void test_vital_all_primitives(void)
{
    const char *src =
        "entity vital_test is\n"
        "  port (a: in std_logic; b: in std_logic;\n"
        "        and_out, or_out, nand_out, nor_out,\n"
        "        xor_out, xnor_out, buf_out, inv_out: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_test is\n"
        "  signal s_and, s_or, s_nand, s_nor: std_logic;\n"
        "  signal s_xor, s_xnor, s_buf, s_inv: std_logic;\n"
        "begin\n"
        "  process(a, b) is\n"
        "  begin\n"
        "    s_and <= vitaland(a & b, s_and);\n"
        "    s_or  <= vitalor(a & b, s_or);\n"
        "    s_nand <= vitalnand(a & b, s_nand);\n"
        "    s_nor  <= vitalnor(a & b, s_nor);\n"
        "    s_xor  <= vitalxor(a & b, s_xor);\n"
        "    s_xnor <= vitalxnor(a & b, s_xnor);\n"
        "    s_buf  <= vitalbuf(a, s_buf);\n"
        "    s_inv  <= vitalinv(a, s_inv);\n"
        "  end process;\n"
        "  and_out <= s_and;\n"
        "  or_out  <= s_or;\n"
        "  nand_out <= s_nand;\n"
        "  nor_out  <= s_nor;\n"
        "  xor_out  <= s_xor;\n"
        "  xnor_out <= s_xnor;\n"
        "  buf_out  <= s_buf;\n"
        "  inv_out  <= s_inv;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("vital_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse vital_test");
    mu_assert_eq(r.unit->port_count, 10, "10 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate vital_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_not_null(v0);
    mu_assert_not_null(v1);

    /* Test all 4 input combinations: (0,0), (0,1), (1,0), (1,1) */
    struct {
        uint8_t a, b;
        uint8_t and_, or_, nand_, nor_, xor_, xnor_, buf_, inv_;
    } cases[] = {
        {0, 0, 0, 0, 1, 1, 0, 1, 0, 1},
        {0, 1, 0, 1, 1, 0, 1, 0, 0, 1},
        {1, 0, 0, 1, 1, 0, 1, 0, 1, 0},
        {1, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    };

    for (size_t i = 0; i < 4; i++) {
        uir_sim_set_signal(sim, "a", cases[i].a ? v1 : v0);
        uir_sim_set_signal(sim, "b", cases[i].b ? v1 : v0);
        uir_sim_run(sim, 10);

        qsim_bit_vector_t *v;
        qsim_logic_state_t s;

        v = uir_sim_get_signal(sim, "and_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].and_ ? QSIM_1 : QSIM_0),
                  "VitalAND");

        v = uir_sim_get_signal(sim, "or_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].or_ ? QSIM_1 : QSIM_0),
                  "VitalOR");

        v = uir_sim_get_signal(sim, "nand_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].nand_ ? QSIM_1 : QSIM_0),
                  "VitalNAND");

        v = uir_sim_get_signal(sim, "nor_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].nor_ ? QSIM_1 : QSIM_0),
                  "VitalNOR");

        v = uir_sim_get_signal(sim, "xor_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].xor_ ? QSIM_1 : QSIM_0),
                  "VitalXOR");

        v = uir_sim_get_signal(sim, "xnor_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].xnor_ ? QSIM_1 : QSIM_0),
                  "VitalXNOR");

        v = uir_sim_get_signal(sim, "buf_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].buf_ ? QSIM_1 : QSIM_0),
                  "VitalBUF");

        v = uir_sim_get_signal(sim, "inv_out");
        mu_assert_not_null(v);
        s = qsim_bit_get(v, 0).state;
        mu_assert(s == (cases[i].inv_ ? QSIM_1 : QSIM_0),
                  "VitalINV");
    }

    uir_sim_destroy(sim);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

/* =================================================================
 * 2. VitalLevel — convert logic to 0/1 (1 maps to 1, everything else to 0)
 * ================================================================= */

static void test_vital_level(void)
{
    const char *src =
        "entity vital_level_test is\n"
        "  port (d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_level_test is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  process(d) is\n"
        "  begin\n"
        "    s <= vitallevel(d, s);\n"
        "  end process;\n"
        "  q <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("vital_level_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse vital_level_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate vital_level_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_not_null(v0);
    mu_assert_not_null(v1);

    /* d=1 => q=1 */
    uir_sim_set_signal(sim, "d", v1);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "VitalLevel: 1->1");

    /* d=0 => q=0 */
    uir_sim_set_signal(sim, "d", v0);
    uir_sim_run(sim, 5);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "VitalLevel: 0->0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

/* =================================================================
 * 3. VitalIdent — identity (output = input)
 * ================================================================= */

static void test_vital_ident(void)
{
    const char *src =
        "entity vital_ident_test is\n"
        "  port (d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_ident_test is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  process(d) is\n"
        "  begin\n"
        "    s <= vitalident(d, s);\n"
        "  end process;\n"
        "  q <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("vital_ident_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse vital_ident_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate vital_ident_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_not_null(v0);
    mu_assert_not_null(v1);

    uir_sim_set_signal(sim, "d", v1);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "VitalIdent: 1->1");

    uir_sim_set_signal(sim, "d", v0);
    uir_sim_run(sim, 5);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "VitalIdent: 0->0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

/* =================================================================
 * 4. Multi-bit VitalAND with 3 inputs
 * ================================================================= */

static void test_vital_multi_input_and(void)
{
    const char *src =
        "entity vital_and3_test is\n"
        "  port (a, b, c: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_and3_test is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  process(a, b, c) is\n"
        "  begin\n"
        "    s <= vitaland(a & b & c, s);\n"
        "  end process;\n"
        "  q <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("vital_and3_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse vital_and3_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate vital_and3_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_not_null(v0);
    mu_assert_not_null(v1);

    /* a=1,b=1,c=1 => AND = 1 */
    uir_sim_set_signal(sim, "a", v1);
    uir_sim_set_signal(sim, "b", v1);
    uir_sim_set_signal(sim, "c", v1);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "111 -> 1");

    /* a=1,b=1,c=0 => AND = 0 */
    uir_sim_set_signal(sim, "c", v0);
    uir_sim_run(sim, 5);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "110 -> 0");

    /* a=1,b=0,c=1 => AND = 0 */
    uir_sim_set_signal(sim, "b", v0);
    uir_sim_set_signal(sim, "c", v1);
    uir_sim_run(sim, 5);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "101 -> 0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

/* =================================================================
 * 5. VITAL through concurrent signal assignment (no process)
 * ================================================================= */

static void test_vital_concurrent(void)
{
    const char *src =
        "entity vital_conc_test is\n"
        "  port (a, b: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_conc_test is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  s <= vitaland(a & b, s);\n"
        "  q <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("vital_conc_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse vital_conc_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate vital_conc_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *v0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *v1 = qsim_bit_vector_from_state(1, QSIM_1);
    mu_assert_not_null(v0);
    mu_assert_not_null(v1);

    /* a=0,b=0 => AND=0 */
    uir_sim_set_signal(sim, "a", v0);
    uir_sim_set_signal(sim, "b", v0);
    uir_sim_run(sim, 10);
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "VitalAND concurrent: 00->0");

    /* a=1,b=1 => AND=1 */
    uir_sim_set_signal(sim, "a", v1);
    uir_sim_set_signal(sim, "b", v1);
    uir_sim_run(sim, 10);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "VitalAND concurrent: 11->1");

    /* a=1,b=0 => AND=0 */
    uir_sim_set_signal(sim, "b", v0);
    uir_sim_run(sim, 10);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "VitalAND concurrent: 10->0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(v0);
    qsim_bit_vector_free(v1);
}

/* =================================================================
 * Registration
 * ================================================================= */

void register_vital_sim_tests(void)
{
    printf("[VITAL Simulator]\n");
    mu_run_test(test_vital_all_primitives);
    mu_run_test(test_vital_level);
    mu_run_test(test_vital_ident);
    mu_run_test(test_vital_multi_input_and);
    mu_run_test(test_vital_concurrent);
    printf("\n");
}
