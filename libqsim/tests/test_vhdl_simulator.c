/* VHDL simulation tests — compile, elaborate, simulate, check values. */
#include "minunit.h"
#include "libqsim/vhdl_parser.h"
#include "libqsim/uir.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir_sim.h"
#include "libqsim/session.h"
#include "libqsim/value.h"
#include <string.h>
#include <stdint.h>

/* ── Helpers ── */

static qsim_bit_vector_t *bv4(uint8_t val) {
    qsim_bit_vector_t *bv = qsim_bit_vector_alloc(4);
    if (bv) {
        for (int i = 0; i < 4; i++)
            qsim_bit_set(bv, i, ((val >> i) & 1) ? QSIM_VAL_1 : QSIM_VAL_0);
    }
    return bv;
}

/* =================================================================
 * 1. 4-bit counter simulation
 * ================================================================= */

static void test_vhdl_counter_sim(void)
{
    const char *src =
        "entity counter is\n"
        "  port (clk: in std_logic; rst: in std_logic; count: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of counter is\n"
        "  signal s_count: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_count <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      s_count <= s_count + \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= s_count;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("counter.vhd", src, strlen(src));
    mu_assert(r.success, "parse counter");
    mu_assert_eq(r.unit->port_count, 3, "3 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate counter");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Initial state: count = XXXX */
    qsim_bit_vector_t *cv = uir_sim_get_signal(sim, "count");
    mu_assert_not_null(cv);
    mu_assert_eq(cv->width, 4, "count is 4 bits");

    /* Reset: rst=1 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_run(sim, 5);

    /* After reset, count should be 0 */
    cv = uir_sim_get_signal(sim, "count");
    uint64_t count_val;
    mu_assert(uir_bv_to_u64(cv, &count_val) != 0, "count known after reset");
    mu_assert_eq(count_val, 0, "count = 0 after reset");

    /* Release reset */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 1);

    /* Toggle clk posedge 4 times: count should become 1,2,3,4 */
    for (uint64_t expected = 1; expected <= 4; expected++) {
        uir_sim_set_signal(sim, "clk", clk_0);
        uir_sim_run(sim, 5);
        uir_sim_set_signal(sim, "clk", clk_1);
        uir_sim_run(sim, 5);

        cv = uir_sim_get_signal(sim, "count");
        mu_assert(uir_bv_to_u64(cv, &count_val) != 0, "count known");
        mu_assert_eq(count_val, expected, "count increments");
    }

    /* 12 more posedges: count should wrap from 4 to 0 (4+12=16, 16 mod 16 = 0) */
    for (int i = 0; i < 12; i++) {
        uir_sim_set_signal(sim, "clk", clk_0);
        uir_sim_run(sim, 2);
        uir_sim_set_signal(sim, "clk", clk_1);
        uir_sim_run(sim, 2);
    }
    cv = uir_sim_get_signal(sim, "count");
    mu_assert(uir_bv_to_u64(cv, &count_val) != 0, "count known after wrap");
    mu_assert_eq(count_val, 0, "count wraps to 0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
}

/* =================================================================
 * 2. DFF simulation
 * ================================================================= */

static void test_vhdl_dff_sim(void)
{
    const char *src =
        "entity dff is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic; qn: out std_logic);\n"
        "end entity;\n"
        "architecture behav of dff is\n"
        "  signal s_q: std_logic;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      s_q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s_q;\n"
        "  qn <= not s_q;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("dff.vhd", src, strlen(src));
    mu_assert(r.success, "parse dff");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate dff");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Drive d=1, toggle clk: q should become 1, qn should become 0 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after d=1 posedge");

    qsim_bit_vector_t *qnv = uir_sim_get_signal(sim, "qn");
    mu_assert_not_null(qnv);
    mu_assert(qsim_bit_get(qnv, 0).state == QSIM_0, "qn = 0 after d=1 posedge");

    /* Drive d=0, toggle clk: q should become 0, qn should become 1 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after d=0 posedge");

    qnv = uir_sim_get_signal(sim, "qn");
    mu_assert(qsim_bit_get(qnv, 0).state == QSIM_1, "qn = 1 after d=0 posedge");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_0);
    qsim_bit_vector_free(d_1);
}

/* =================================================================
 * 3. ALU simulation (combinatorial, case statement)
 * ================================================================= */

static void test_vhdl_alu_sim(void)
{
    const char *src =
        "entity alu is\n"
        "  port (a: in std_logic_vector(3 downto 0); b: in std_logic_vector(3 downto 0);\n"
        "        op: in std_logic_vector(1 downto 0); result: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of alu is\n"
        "begin\n"
        "  process(a, b, op) is\n"
        "  begin\n"
        "    case op is\n"
        "      when \"00\" => result <= a and b;\n"
        "      when \"01\" => result <= a or b;\n"
        "      when \"10\" => result <= a xor b;\n"
        "      when others => result <= a + b;\n"
        "    end case;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("alu.vhd", src, strlen(src));
    mu_assert(r.success, "parse alu");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate alu");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Set a = 0011 (3), b = 0001 (1) */
    qsim_bit_vector_t *a3 = bv4(3);
    qsim_bit_vector_t *b1 = bv4(1);
    uir_sim_set_signal(sim, "a", a3);
    uir_sim_set_signal(sim, "b", b1);
    uir_sim_run(sim, 5);

    /* Test each op */
    struct { const char *op; uint64_t expected; } cases[] = {
        {"00", 1},  /* AND: 0011 & 0001 = 0001 */
        {"01", 3},  /* OR:  0011 | 0001 = 0011 */
        {"10", 2},  /* XOR: 0011 ^ 0001 = 0010 */
        {"11", 4},  /* ADD (others): 0011 + 0001 = 0100 */
    };
    uint64_t result_val;
    for (size_t i = 0; i < 4; i++) {
        qsim_bit_vector_t *op = uir_u64_to_bv(i, 2);
        mu_assert_not_null(op);
        uir_sim_set_signal(sim, "op", op);
        uir_sim_run(sim, 5);

        qsim_bit_vector_t *rv = uir_sim_get_signal(sim, "result");
        mu_assert_not_null(rv);
        mu_assert(uir_bv_to_u64(rv, &result_val) != 0, "result known");
        mu_assert_eq(result_val, cases[i].expected, "ALU operation correct");
        qsim_bit_vector_free(op);
    }

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a3);
    qsim_bit_vector_free(b1);
}

/* =================================================================
 * 4. Shift register simulation (arithmetic shift)
 * ================================================================= */

static void test_vhdl_shift_reg_sim(void)
{
    const char *src =
        "entity shift_reg is\n"
        "  port (clk: in std_logic; rst: in std_logic; din: in std_logic;\n"
        "        dout: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture behav of shift_reg is\n"
        "  signal s_reg: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_reg <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      s_reg <= (s_reg + s_reg) or (\"000\" & din);\n"
        "    end if;\n"
        "  end process;\n"
        "  dout <= s_reg;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("shift_reg.vhd", src, strlen(src));
    mu_assert(r.success, "parse shift_reg");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate shift_reg");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *din_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *din_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Reset: dout should become 0 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_run(sim, 5);
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 1);

    qsim_bit_vector_t *dv = uir_sim_get_signal(sim, "dout");
    mu_assert_not_null(dv);
    uint64_t dout_val;
    mu_assert(uir_bv_to_u64(dv, &dout_val) != 0, "dout known after reset");
    mu_assert_eq(dout_val, 0, "dout = 0 after reset");

    /* Clock in din=1: dout should become 0001 */
    uir_sim_set_signal(sim, "din", din_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    dv = uir_sim_get_signal(sim, "dout");
    mu_assert(uir_bv_to_u64(dv, &dout_val) != 0, "dout known after clock");
    mu_assert_eq(dout_val, 1, "dout = 1 after din=1");

    /* Clock with din=0 three times: 0010, 0100, 1000 */
    uir_sim_set_signal(sim, "din", din_0);
    uint64_t expected[] = {2, 4, 8};
    for (int i = 0; i < 3; i++) {
        uir_sim_set_signal(sim, "clk", clk_0);
        uir_sim_run(sim, 2);
        uir_sim_set_signal(sim, "clk", clk_1);
        uir_sim_run(sim, 5);

        dv = uir_sim_get_signal(sim, "dout");
        mu_assert(uir_bv_to_u64(dv, &dout_val) != 0, "dout known");
        mu_assert_eq(dout_val, expected[i], "shift left");
    }

    /* One more clock with din=0: 1000 -> 0000 (MSB falls off) */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    dv = uir_sim_get_signal(sim, "dout");
    mu_assert(uir_bv_to_u64(dv, &dout_val) != 0, "dout known after overflow");
    mu_assert_eq(dout_val, 0, "dout = 0 after MSB shifts out");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(din_0);
    qsim_bit_vector_free(din_1);
}

/* =================================================================
 * 5. Comparator with boolean output
 * ================================================================= */

static void test_vhdl_compare_sim(void)
{
    const char *src =
        "entity comp is\n"
        "  port (a: in std_logic_vector(3 downto 0); b: in std_logic_vector(3 downto 0);\n"
        "        eq: out boolean; neq: out boolean);\n"
        "end entity;\n"
        "architecture behav of comp is\n"
        "begin\n"
        "  process(a, b) is\n"
        "  begin\n"
        "    eq <= (a = b);\n"
        "    neq <= (a /= b);\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("comp.vhd", src, strlen(src));
    mu_assert(r.success, "parse comp");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate comp");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *a5 = uir_u64_to_bv(5, 4);
    qsim_bit_vector_t *b5 = uir_u64_to_bv(5, 4);
    qsim_bit_vector_t *b7 = uir_u64_to_bv(7, 4);
    mu_assert_not_null(a5);
    mu_assert_not_null(b5);
    mu_assert_not_null(b7);

    /* a=5, b=5: eq=true, neq=false */
    uir_sim_set_signal(sim, "a", a5);
    uir_sim_set_signal(sim, "b", b5);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *eqv = uir_sim_get_signal(sim, "eq");
    mu_assert_not_null(eqv);
    mu_assert(qsim_bit_get(eqv, 0).state == QSIM_1, "eq true when a==b");

    qsim_bit_vector_t *neqv = uir_sim_get_signal(sim, "neq");
    mu_assert_not_null(neqv);
    mu_assert(qsim_bit_get(neqv, 0).state == QSIM_0, "neq false when a==b");

    /* a=5, b=7: eq=false, neq=true */
    uir_sim_set_signal(sim, "b", b7);
    uir_sim_run(sim, 5);

    eqv = uir_sim_get_signal(sim, "eq");
    mu_assert(qsim_bit_get(eqv, 0).state == QSIM_0, "eq false when a!=b");

    neqv = uir_sim_get_signal(sim, "neq");
    mu_assert(qsim_bit_get(neqv, 0).state == QSIM_1, "neq true when a!=b");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a5);
    qsim_bit_vector_free(b5);
    qsim_bit_vector_free(b7);
}

/* =================================================================
 * 6. Nested if-elsif priority encoder
 * ================================================================= */

static void test_vhdl_prio_enc_sim(void)
{
    const char *src =
        "entity prio is\n"
        "  port (sel: in std_logic_vector(1 downto 0); y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of prio is\n"
        "begin\n"
        "  process(sel) is\n"
        "  begin\n"
        "    if sel = \"01\" then\n"
        "      y <= \"01\";\n"
        "    elsif sel = \"10\" then\n"
        "      y <= \"10\";\n"
        "    elsif sel = \"11\" then\n"
        "      y <= \"11\";\n"
        "    else\n"
        "      y <= \"00\";\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("prio.vhd", src, strlen(src));
    mu_assert(r.success, "parse prio");
    mu_assert_eq(r.unit->port_count, 2, "2 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate prio");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* sel=01 => first match => y=01 */
    qsim_bit_vector_t *sel01 = uir_u64_to_bv(1, 2);
    uir_sim_set_signal(sim, "sel", sel01);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    uint64_t yval;
    mu_assert(uir_bv_to_u64(yv, &yval) != 0, "y known");
    mu_assert_eq(yval, 1, "sel=01 => y=01");

    /* sel=10 => second elsif match => y=10 */
    qsim_bit_vector_t *sel10 = uir_u64_to_bv(2, 2);
    uir_sim_set_signal(sim, "sel", sel10);
    uir_sim_run(sim, 5);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(yv, &yval) != 0, "y known");
    mu_assert_eq(yval, 2, "sel=10 => y=10");

    /* sel=11 => third elsif match => y=11 */
    qsim_bit_vector_t *sel11 = uir_u64_to_bv(3, 2);
    uir_sim_set_signal(sim, "sel", sel11);
    uir_sim_run(sim, 5);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(yv, &yval) != 0, "y known");
    mu_assert_eq(yval, 3, "sel=11 => y=11");

    /* sel=00 => else case => y=00 */
    qsim_bit_vector_t *sel00 = uir_u64_to_bv(0, 2);
    uir_sim_set_signal(sim, "sel", sel00);
    uir_sim_run(sim, 5);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(yv, &yval) != 0, "y known");
    mu_assert_eq(yval, 0, "sel=00 => y=00");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(sel01);
    qsim_bit_vector_free(sel10);
    qsim_bit_vector_free(sel11);
    qsim_bit_vector_free(sel00);
}

/* =================================================================
 * 7. Multi-process communication
 * ================================================================= */

static void test_vhdl_multi_proc_sim(void)
{
    /* Two processes: p1 drives signal s on clk posedge,
     * p2 reads s and drives output q (level-sensitive to s). */
    const char *src =
        "entity multi_proc is\n"
        "  port (clk: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of multi_proc is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  p1: process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      s <= '1';\n"
        "    end if;\n"
        "  end process;\n"
        "  p2: process(s) is\n"
        "  begin\n"
        "    q <= s;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("multi_proc.vhd", src, strlen(src));
    mu_assert(r.success, "parse multi_proc");
    mu_assert_eq(r.unit->process_count, 2, "two processes");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate multi_proc");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Toggle clk posedge: p1 fires, s <= 1, p2 sees s change, q <= 1 */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after clk posedge");

    /* Second posedge: s already 1, p2 fires but q stays 1 (no change) */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q stays 1");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
}

/* Forward declarations for tests defined after registration */
static void test_vhdl_rising_edge_dff_sim(void);
static void test_vhdl_falling_edge_sim(void);
static void test_selected_name_rising_edge_sim(void);
static void test_selected_name_numeric_std_sim(void);

/* With-select tests */
static void test_with_select_simple(void);
static void test_with_select_single_bit(void);
static void test_with_select_only_others(void);
static void test_with_select_seq(void);
static void test_with_select_multi_cycle(void);

/* Bug 1 session API test */
static void test_vhdl_bug1_session_api(void);

/* Generate tests */
static void test_vhdl_if_generate_sim(void);
static void test_vhdl_for_generate_sim(void);
static void test_vhdl_for_generate_downto_sim(void);

/* Assert/report tests */
static void test_vhdl_assert_sim(void);
static void test_vhdl_report_sim(void);
static void test_vhdl_alias_sim(void);

/* TEXTIO tests */
static void test_vhdl_textio_readline_sim(void);

/* =================================================================
 * 8. IEEE numeric_std builtin function simulation
 * ================================================================= */

static void test_numeric_std_unsigned_sim(void)
{
    /* unsigned(a) reinterprets std_logic_vector as unsigned — bits pass through. */
    const char *src =
        "entity test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "begin\n"
        "  y <= unsigned(a);\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse unsigned sim");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate unsigned sim");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Drive a = 0xAB */
    qsim_bit_vector_t *a_val = uir_u64_to_bv(0xAB, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 0xAB, "unsigned(a) passes bits through");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

static void test_numeric_std_signed_sim(void)
{
    /* signed(a) reinterprets std_logic_vector as signed — bits pass through. */
    const char *src =
        "entity test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "begin\n"
        "  y <= signed(a);\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse signed sim");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate signed sim");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *a_val = uir_u64_to_bv(0xAB, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 0xAB, "signed(a) passes bits through");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

static void test_numeric_std_to_integer_sim(void)
{
    /* to_integer(unsigned(a)) converts bit vector to 32-bit integer. */
    const char *src =
        "entity test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out integer);\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "begin\n"
        "  y <= to_integer(unsigned(a));\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse to_integer sim");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate to_integer sim");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Drive a = 5 → to_integer(unsigned(a)) should be 5 */
    qsim_bit_vector_t *a5 = uir_u64_to_bv(5, 8);
    uir_sim_set_signal(sim, "a", a5);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 5, "to_integer(unsigned(5)) = 5");

    /* Drive a = 255 → should be 255 */
    qsim_bit_vector_t *a255 = uir_u64_to_bv(255, 8);
    uir_sim_set_signal(sim, "a", a255);
    uir_sim_run(sim, 5);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 255, "to_integer(unsigned(255)) = 255");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a5);
    qsim_bit_vector_free(a255);
    uir_destroy_design_unit(r.unit);
}

static void test_numeric_std_nested_sim(void)
{
    /* to_integer(unsigned(a)) computed in a sequential process. */
    const char *src =
        "entity test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out integer);\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    y <= to_integer(unsigned(a));\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse nested sim");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate nested sim");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *a_val = uir_u64_to_bv(42, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 42, "process to_integer(unsigned(42)) = 42");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 9. Multi-driver std_logic resolution
 * ================================================================= */

static void test_vhdl_multi_driver_resolve_sim(void)
{
    /* Two processes drive the same signal s with different values.
     * std_logic resolution function resolves the conflict. */
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

    vhdl_parse_result_t r = vhdl_parse("multi_driver.vhd", src, strlen(src));
    mu_assert(r.success, "parse multi_driver");
    mu_assert_eq(r.unit->process_count, 2, "two processes");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate multi_driver");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *bv_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *bv_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *bv_z = qsim_bit_vector_from_state(1, QSIM_Z);

    /* Test 1: a=1, b=0 → 1 vs 0 conflict → X */
    uir_sim_set_signal(sim, "a", bv_1);
    uir_sim_set_signal(sim, "b", bv_0);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_X, "1 vs 0 → X");

    /* Test 2: a=Z, b=1 → both change (b was 0), both fire → Z vs 1 → 1 */
    uir_sim_set_signal(sim, "a", bv_z);
    uir_sim_set_signal(sim, "b", bv_1);
    uir_sim_run(sim, 10);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "Z vs 1 → 1");

    /* Test 3: a=0, b=Z → both change, both fire → 0 vs Z → 0 */
    uir_sim_set_signal(sim, "a", bv_0);
    uir_sim_set_signal(sim, "b", bv_z);
    uir_sim_run(sim, 10);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "0 vs Z → 0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(bv_1);
    qsim_bit_vector_free(bv_0);
    qsim_bit_vector_free(bv_z);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 10. Clocked process with std_logic_vector (regression: event-driven
 *     value must not be discarded when process driver slots exist)
 * ================================================================= */

static void test_vhdl_clocked_vector_process_sim(void)
{
    /* Clocked process driving a std_logic_vector through architecture
     * hierarchy to an output. This exercises the event-driven write path
     * through NET_GROUP_STD_LOGIC. */
    const char *src =
        "entity test is\n"
        "  port (clk: in std_logic; a: in std_logic_vector(7 downto 0);\n"
        "        y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "  signal s: std_logic_vector(7 downto 0);\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if clk = '1' then\n"
        "      s <= a;\n"
        "    end if;\n"
        "  end process;\n"
        "  y <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse clocked_vector");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate clocked_vector");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *a_val = uir_u64_to_bv(0xA5, 8);

    /* drive clk low, then high with a=0xA5 */
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(yv, &y_u64) != 0, "y known — not X");
    mu_assert_eq(y_u64, 0xA5, "y = 0xA5 from clocked process");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 11. (others => '0') aggregate in clocked process
 * ================================================================= */

static void test_vhdl_others_aggregate_sim(void)
{
    const char *src =
        "entity test is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "  signal s: std_logic_vector(7 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s <= (others => '0');\n"
        "    elsif clk = '1' then\n"
        "      s <= (others => '1');\n"
        "    end if;\n"
        "  end process;\n"
        "  y <= s;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse others_aggregate");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate others_aggregate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* After reset, s and y should be all zeros */
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);

    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(yv, &y_u64) != 0, "y known after reset");
    mu_assert_eq(y_u64, 0x00, "y = 0x00 from (others => '0')");

    /* De-assert reset, drive clock — s should become all ones */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 5);

    yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(uir_bv_to_u64(yv, &y_u64) != 0, "y known after clock");
    mu_assert_eq(y_u64, 0xFF, "y = 0xFF from (others => '1')");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(clk_1);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 10. Array-typed signals
 * ================================================================= */

static void test_vhdl_array_signal_sim(void)
{
    /* Direct array declaration with element write/read */
    const char *src =
        "entity array_test is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d: in std_logic_vector(7 downto 0);\n"
        "        q: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of array_test is\n"
        "  signal mem : std_logic_vector(7 downto 0)(0 to 3);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      mem(0) <= x\"AA\";\n"
        "      mem(1) <= x\"BB\";\n"
        "      mem(2) <= x\"CC\";\n"
        "      mem(3) <= x\"DD\";\n"
        "    elsif rising_edge(clk) then\n"
        "      mem(0) <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= mem(1);\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("array_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse array_test");

    /* Check that mem is declared as an array signal */
    uir_signal_t *mem_sig = NULL;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "mem") == 0) {
            mem_sig = r.unit->signals[i];
            break;
        }
    }
    mu_assert_not_null(mem_sig);
    mu_assert(mem_sig->array_size == 4, "mem array_size == 4");
    mu_assert(mem_sig->array_dim_count == 1, "mem array_dim_count == 1");
    mu_assert(mem_sig->array_dims[0] == 4, "mem array_dims[0] == 4");
    mu_assert(mem_sig->width == 8, "mem element width == 8");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate array_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);

    /* Initialize clk to 0 so later clk=1 produces a valid rising edge */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 10);

    /* Assert reset: all elements get initialized */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_run(sim, 10);

    /* Check each array element via the full signal value */
    qsim_bit_vector_t *mem_val = uir_sim_get_signal(sim, "mem");
    mu_assert_not_null(mem_val);
    mu_assert(mem_val->width == 32, "mem total width == 32");

    /* After reset: mem(0)=xAA, mem(1)=xBB, mem(2)=xCC, mem(3)=xDD
     * mem(0) = bits [7:0], mem(1) = bits [15:8], etc. */
    mu_assert(qsim_bit_get(mem_val, 0).state == QSIM_0, "mem(0) bit 0 = 0");
    mu_assert(qsim_bit_get(mem_val, 1).state == QSIM_1, "mem(0) bit 1 = 1");
    mu_assert(qsim_bit_get(mem_val, 7).state == QSIM_1, "mem(0) bit 7 = 1 (xAA)");

    mu_assert(qsim_bit_get(mem_val, 8).state == QSIM_1, "mem(1) bit 8 = 1 (xBB)");
    mu_assert(qsim_bit_get(mem_val, 9).state == QSIM_1, "mem(1) bit 9 = 1 (xBB)");
    mu_assert(qsim_bit_get(mem_val, 15).state == QSIM_1, "mem(1) bit 15 = 1 (xBB)");

    mu_assert(qsim_bit_get(mem_val, 16).state == QSIM_0, "mem(2) bit 16 = 0 (xCC)");
    mu_assert(qsim_bit_get(mem_val, 23).state == QSIM_1, "mem(2) bit 23 = 1 (xCC)");

    mu_assert(qsim_bit_get(mem_val, 24).state == QSIM_1, "mem(3) bit 24 = 1 (xDD)");
    mu_assert(qsim_bit_get(mem_val, 31).state == QSIM_1, "mem(3) bit 31 = 1 (xDD)");

    /* Check q = mem(1) */
    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qv->width == 8, "q width == 8");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q bit 0 = 1 (xBB)");
    mu_assert(qsim_bit_get(qv, 1).state == QSIM_1, "q bit 1 = 1 (xBB)");
    mu_assert(qsim_bit_get(qv, 2).state == QSIM_0, "q bit 2 = 0");
    mu_assert(qsim_bit_get(qv, 7).state == QSIM_1, "q bit 7 = 1 (xBB)");

    /* Release reset, apply rising edge with d=x11 */
    uir_sim_set_signal(sim, "rst", rst_0);
    qsim_bit_vector_t *d_val = qsim_bit_vector_alloc(8);
    d_val->bits[0] = QSIM_VAL_1; d_val->bits[1] = QSIM_VAL_1;
    d_val->bits[2] = QSIM_VAL_1; d_val->bits[3] = QSIM_VAL_1;
    d_val->bits[4] = QSIM_VAL_0; d_val->bits[5] = QSIM_VAL_0;
    d_val->bits[6] = QSIM_VAL_0; d_val->bits[7] = QSIM_VAL_0;
    uir_sim_set_signal(sim, "d", d_val);

    /* Posedge clock: mem(0) <= d = x"0F" */
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 10);

    mem_val = uir_sim_get_signal(sim, "mem");
    mu_assert_not_null(mem_val);

    /* mem(0) should now be x"0F" (bits 7:4 = 0, bits 3:0 = 1) */
    mu_assert(qsim_bit_get(mem_val, 0).state == QSIM_1, "mem(0) bit 0 = 1 after write");
    mu_assert(qsim_bit_get(mem_val, 3).state == QSIM_1, "mem(0) bit 3 = 1 after write");
    mu_assert(qsim_bit_get(mem_val, 4).state == QSIM_0, "mem(0) bit 4 = 0 after write");
    mu_assert(qsim_bit_get(mem_val, 7).state == QSIM_0, "mem(0) bit 7 = 0 after write");

    /* Other elements unchanged: mem(1) still = xBB */
    mu_assert(qsim_bit_get(mem_val, 8).state == QSIM_1, "mem(1) bit 8 unchanged");
    mu_assert(qsim_bit_get(mem_val, 9).state == QSIM_1, "mem(1) bit 9 unchanged");

    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(d_val);
    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_array_type_decl_sim(void)
{
    /* Array type declaration + signal using that type */
    const char *src =
        "entity array_type_test is\n"
        "end entity;\n"
        "architecture behav of array_type_test is\n"
        "  type byte_array is array (0 to 3) of std_logic_vector(7 downto 0);\n"
        "  signal mem : byte_array;\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    mem(0) <= x\"12\";\n"
        "    mem(1) <= x\"34\";\n"
        "    mem(2) <= x\"56\";\n"
        "    mem(3) <= x\"78\";\n"
        "    wait;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("array_type_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse array_type_test");

    /* Check mem signal */
    uir_signal_t *mem_sig = NULL;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "mem") == 0) {
            mem_sig = r.unit->signals[i];
            break;
        }
    }
    mu_assert_not_null(mem_sig);
    mu_assert(mem_sig->array_size == 4, "type_decl array_size == 4");
    mu_assert(mem_sig->width == 8, "type_decl element width == 8");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate array_type_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *mem_val = uir_sim_get_signal(sim, "mem");
    mu_assert_not_null(mem_val);
    mu_assert(mem_val->width == 32, "type_decl mem total width == 32");

    /* Verify all four elements after process runs */
    mu_assert(qsim_bit_get(mem_val, 1).state == QSIM_1, "mem(0) bit 1 = 1 (x12)");
    mu_assert(qsim_bit_get(mem_val, 4).state == QSIM_1, "mem(0) bit 4 = 1 (x12)");
    mu_assert(qsim_bit_get(mem_val, 8).state == QSIM_0, "mem(1) bit 8 = 0 (x34)");
    mu_assert(qsim_bit_get(mem_val, 10).state == QSIM_1, "mem(1) bit 10 = 1 (x34)");
    mu_assert(qsim_bit_get(mem_val, 16).state == QSIM_0, "mem(2) bit 16 = 0 (x56)");
    mu_assert(qsim_bit_get(mem_val, 18).state == QSIM_1, "mem(2) bit 18 = 1 (x56)");
    mu_assert(qsim_bit_get(mem_val, 24).state == QSIM_0, "mem(3) bit 24 = 0 (x78)");
    mu_assert(qsim_bit_get(mem_val, 25).state == QSIM_0, "mem(3) bit 25 = 0 (x78)");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_array_index_with_to_integer_sim(void)
{
    /* Clocked process using to_integer(unsigned(addr)) as array index.
     * This reproduces the pattern from bug_vhdl_to_integer_process_kill.md */
    const char *src =
        "entity array_index_test is\n"
        "  port (\n"
        "    clk: in std_logic;\n"
        "    wen: in std_logic;\n"
        "    addr: in std_logic_vector(2 downto 0);\n"
        "    wdata: in std_logic_vector(31 downto 0)\n"
        "  );\n"
        "end entity;\n"
        "architecture behav of array_index_test is\n"
        "  type mem_t is array (0 to 7) of std_logic_vector(31 downto 0);\n"
        "  signal bank0 : mem_t;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if rising_edge(clk) then\n"
        "      if wen = '1' then\n"
        "        bank0(to_integer(unsigned(addr))) <= wdata;\n"
        "      end if;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("array_index_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse array_index_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate array_index_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *wen_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *wen_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *addr_2 = uir_u64_to_bv(2, 3);
    qsim_bit_vector_t *addr_5 = uir_u64_to_bv(5, 3);
    qsim_bit_vector_t *wdata_val = uir_u64_to_bv(0xDEADBEEF, 32);

    /* Toggle clk with wen=0: no write */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    /* Set up write: wen=1, addr=2, wdata=0xDEADBEEF */
    uir_sim_set_signal(sim, "wen", wen_1);
    uir_sim_set_signal(sim, "addr", addr_2);
    uir_sim_set_signal(sim, "wdata", wdata_val);

    /* Toggle clk low→high to trigger write */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    /* Read bank0 and verify element 2 = 0xDEADBEEF */
    qsim_bit_vector_t *bank0_val = uir_sim_get_signal(sim, "bank0");
    mu_assert_not_null(bank0_val);
    mu_assert(bank0_val->width == 256, "bank0 total width = 8*32 = 256");

    /* Element 2 is at bit offset 2*32 = 64 */
    uint64_t elem2 = 0;
    for (uint32_t i = 64; i < 96 && i < bank0_val->width; i++) {
        qsim_value_t b = qsim_bit_get(bank0_val, i);
        if (b.state == QSIM_1) elem2 |= (1ULL << (i - 64));
    }
    mu_assert(elem2 == 0xDEADBEEF, "bank0(2) = 0xDEADBEEF");

    /* Write to element 5 */
    uir_sim_set_signal(sim, "addr", addr_5);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    bank0_val = uir_sim_get_signal(sim, "bank0");
    uint64_t elem5 = 0;
    for (uint32_t i = 160; i < 192 && i < bank0_val->width; i++) {
        qsim_value_t b = qsim_bit_get(bank0_val, i);
        if (b.state == QSIM_1) elem5 |= (1ULL << (i - 160));
    }
    mu_assert(elem5 == 0xDEADBEEF, "bank0(5) = 0xDEADBEEF");
    /* Element 0 should still be 0 */
    uint64_t elem0 = 0;
    for (uint32_t i = 0; i < 32 && i < bank0_val->width; i++) {
        qsim_value_t b = qsim_bit_get(bank0_val, i);
        if (b.state == QSIM_1) elem0 |= (1ULL << i);
    }
    mu_assert(elem0 == 0, "bank0(0) unchanged = 0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(wen_1);
    qsim_bit_vector_free(wen_0);
    qsim_bit_vector_free(addr_2);
    qsim_bit_vector_free(addr_5);
    qsim_bit_vector_free(wdata_val);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_shift_left_sim(void)
{
    /* shift_left(unsigned, integer) — test concurrent assignment */
    const char *src =
        "entity shift_left_test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of shift_left_test is\n"
        "begin\n"
        "  y <= std_logic_vector(shift_left(unsigned(a), 2));\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("shift_left_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse shift_left_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate shift_left_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a = 0b00000001 → shifted left by 2 → 0b00000100 = 4 */
    qsim_bit_vector_t *a_val = uir_u64_to_bv(1, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 4, "shift_left(1,2) = 4");

    /* a = 0b11111111 → shifted left by 2 → 0b11111100 = 252 */
    qsim_bit_vector_free(a_val);
    a_val = uir_u64_to_bv(255, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 252, "shift_left(255,2) = 252");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_shift_right_sim(void)
{
    /* shift_right(unsigned, integer) — test concurrent assignment */
    const char *src =
        "entity shift_right_test is\n"
        "  port (a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of shift_right_test is\n"
        "begin\n"
        "  y <= std_logic_vector(shift_right(unsigned(a), 2));\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("shift_right_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse shift_right_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate shift_right_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a = 0b00000100 → shifted right by 2 → 0b00000001 = 1 */
    qsim_bit_vector_t *a_val = uir_u64_to_bv(4, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 1, "shift_right(4,2) = 1");

    /* a = 0b11111111 → shifted right by 2 → 0b00111111 = 63 */
    qsim_bit_vector_free(a_val);
    a_val = uir_u64_to_bv(255, 8);
    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 63, "shift_right(255,2) = 63");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_to_unsigned_sim(void)
{
    /* to_unsigned(int, size) converts integer to unsigned */
    const char *src =
        "entity to_unsigned_test is\n"
        "  port (y: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of to_unsigned_test is\n"
        "begin\n"
        "  y <= std_logic_vector(to_unsigned(42, 8));\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("to_unsigned_test.vhd", src, strlen(src));
    mu_assert(r.success, "parse to_unsigned_test");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate to_unsigned_test");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    uint64_t y_u64;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 42, "to_unsigned(42,8) = 42");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 12. Port array dimensions
 * ================================================================= */

static void test_port_array_dimensions(void)
{
    const char *src =
        "entity port_array is\n"
        "  port (\n"
        "    clk: in std_logic;\n"
        "    data: out std_logic_vector(7 downto 0)(0 to 3)\n"
        "  );\n"
        "end entity;\n"
        "architecture behav of port_array is\n"
        "begin\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("port_array.vhd", src, strlen(src));
    mu_assert(r.success, "parse port_array");

    uir_port_t *data_port = NULL;
    for (size_t i = 0; i < r.unit->port_count; i++) {
        if (strcmp(r.unit->ports[i]->name, "data") == 0) {
            data_port = r.unit->ports[i];
            break;
        }
    }
    mu_assert_not_null(data_port);
    mu_assert(data_port->array_size == 4, "port array_size == 4");
    mu_assert(data_port->array_dim_count == 1, "port array_dim_count == 1");
    mu_assert(data_port->array_dims[0] == 4, "port array_dims[0] == 4");
    mu_assert(data_port->width == 8, "port element width == 8");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate port_array");
    uir_elab_result_free(elab);

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 13. Multi-dimensional array type
 * ================================================================= */

static void test_multi_dim_array_type(void)
{
    const char *src =
        "entity multi_dim is\n"
        "end entity;\n"
        "architecture behav of multi_dim is\n"
        "  type mem2d is array (0 to 3, 0 to 7) of std_logic;\n"
        "  signal arr : mem2d;\n"
        "begin\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("multi_dim.vhd", src, strlen(src));
    mu_assert(r.success, "parse multi_dim");

    /* Check the array type */
    uir_vhdl_type_t *arr_type = NULL;
    for (size_t i = 0; i < r.unit->vhdl_type_count; i++) {
        if (strcmp(r.unit->vhdl_types[i].name, "mem2d") == 0) {
            arr_type = &r.unit->vhdl_types[i];
            break;
        }
    }
    mu_assert_not_null(arr_type);
    mu_assert(arr_type->kind == UIR_VHDL_TYPE_ARRAY, "type is ARRAY");
    mu_assert(arr_type->array_dim_count == 2, "type array_dim_count == 2");
    mu_assert(arr_type->array_dims[0] == 4, "type array_dims[0] == 4");
    mu_assert(arr_type->array_dims[1] == 8, "type array_dims[1] == 8");
    mu_assert(arr_type->array_size == 32, "type array_size == 4*8 = 32");
    mu_assert(arr_type->element_width == 1, "type element_width == 1");
    mu_assert(arr_type->width == 32, "type total width == 32");

    /* Check the signal using this type */
    uir_signal_t *arr_sig = NULL;
    for (size_t i = 0; i < r.unit->signal_count; i++) {
        if (strcmp(r.unit->signals[i]->name, "arr") == 0) {
            arr_sig = r.unit->signals[i];
            break;
        }
    }
    mu_assert_not_null(arr_sig);
    mu_assert(arr_sig->array_dim_count == 2, "sig array_dim_count == 2");
    mu_assert(arr_sig->array_dims[0] == 4, "sig array_dims[0] == 4");
    mu_assert(arr_sig->array_dims[1] == 8, "sig array_dims[1] == 8");
    mu_assert(arr_sig->array_size == 32, "sig array_size == 32");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate multi_dim");
    uir_elab_result_free(elab);

    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 14. Concurrent signal assignment tests
 * ================================================================= */

static void test_concurrent_assign_constant(void)
{
    /* Simplest case: y <= '1' */
    const char *src =
        "entity conc_const is\n"
        "  port (y: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of conc_const is\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("conc_const.vhd", src, strlen(src));
    mu_assert(r.success, "parse conc_const");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate conc_const");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Run: initial_eval should schedule the CA result for delta 1 */
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    mu_assert(y_val->width == 1, "y width == 1");
    mu_assert(qsim_bit_get(y_val, 0).state == QSIM_1, "y = '1'");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_concurrent_assign_when_else(void)
{
    /* y <= "1111" when sel = '1' else "0000" */
    const char *src =
        "entity conc_when is\n"
        "  port (sel: in std_logic; y: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of conc_when is\n"
        "begin\n"
        "  y <= \"1111\" when sel = '1' else \"0000\";\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("conc_when.vhd", src, strlen(src));
    mu_assert(r.success, "parse conc_when");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate conc_when");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Set sel='1' before initial eval */
    qsim_bit_vector_t *sel1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "sel", sel1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    mu_assert(y_val->width == 4, "y width == 4");
    uint64_t y_u64 = 0;
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 0xF, "y = 0xF when sel=1");

    /* Switch to sel='0' and verify y becomes 0 */
    qsim_bit_vector_t *sel0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "sel", sel0);
    uir_sim_run(sim, 10);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(y_val, &y_u64) != 0, "y known");
    mu_assert_eq(y_u64, 0x0, "y = 0x0 when sel=0");

    qsim_bit_vector_free(sel1);
    qsim_bit_vector_free(sel0);
    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_process_all_sim(void)
{
    /* Combinatorial process using process(all) — auto-sensitivity */
    const char *src =
        "entity proc_all is\n"
        "  port (a: in std_logic; b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of proc_all is\n"
        "begin\n"
        "  process(all) begin\n"
        "    y <= a and b;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("proc_all.vhd", src, strlen(src));
    mu_assert(r.success, "parse process(all)");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate process(all)");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Drive a=1, b=0 -> y should become 0 */
    qsim_bit_vector_t *a1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *b0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "a", a1);
    uir_sim_set_signal(sim, "b", b0);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *y = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y);
    mu_assert(y->width == 1, "y width");
    uint64_t y_u64 = 0;
    mu_assert(uir_bv_to_u64(y, &y_u64) != 0, "y known after a=1,b=0");
    mu_assert_eq(y_u64, 0, "y = a and b = 0");

    /* Drive b=1 -> y should become 1 */
    qsim_bit_vector_t *b1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "b", b1);
    uir_sim_run(sim, 10);

    y = uir_sim_get_signal(sim, "y");
    mu_assert(uir_bv_to_u64(y, &y_u64) != 0, "y known after b=1");
    mu_assert_eq(y_u64, 1, "y = a and b = 1");

    qsim_bit_vector_free(a1);
    qsim_bit_vector_free(b0);
    qsim_bit_vector_free(b1);
    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_concurrent_assign_process_driven(void)
{
    /* q <= q_int where a process drives q_int */
    const char *src =
        "entity conc_proc is\n"
        "  port (clk: in std_logic; rst_n: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of conc_proc is\n"
        "  signal q_int: std_logic;\n"
        "begin\n"
        "  q <= q_int;\n"
        "  process(clk) begin\n"
        "    if rising_edge(clk) then\n"
        "      if rst_n = '0' then q_int <= '0'; end if;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("conc_proc.vhd", src, strlen(src));
    mu_assert(r.success, "parse conc_proc");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate conc_proc");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Apply reset: clk=0, rst_n=0 */
    qsim_bit_vector_t *clk0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "clk", clk0);
    uir_sim_set_signal(sim, "rst_n", rst0);
    uir_sim_run(sim, 5);

    /* Check initial q (U/X is expected since q_int is uninitialized) */
    qsim_bit_vector_t *q_val = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(q_val);

    /* Toggle clock to trigger rising_edge */
    qsim_bit_vector_t *clk1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "clk", clk1);
    uir_sim_run(sim, 5);

    q_val = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(q_val, 0).state == QSIM_0, "q = '0' after reset rising_edge");

    qsim_bit_vector_free(clk0);
    qsim_bit_vector_free(clk1);
    qsim_bit_vector_free(rst0);
    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_concurrent_assign_signal_dep(void)
{
    /* y <= a — re-evaluates when a changes */
    const char *src =
        "entity conc_dep is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture rtl of conc_dep is\n"
        "begin\n"
        "  y <= a;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("conc_dep.vhd", src, strlen(src));
    mu_assert(r.success, "parse conc_dep");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate conc_dep");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Set a='0' before first run */
    qsim_bit_vector_t *a0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "a", a0);

    uir_sim_run(sim, 10);

    qsim_bit_vector_t *y_val = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(y_val);
    mu_assert(qsim_bit_get(y_val, 0).state == QSIM_0, "y = '0' after a='0'");

    /* Set a='1' mid-simulation */
    qsim_bit_vector_t *a1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "a", a1);
    uir_sim_run(sim, 10);

    y_val = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(y_val, 0).state == QSIM_1, "y = '1' after a='1'");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a0);
    qsim_bit_vector_free(a1);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 16. Concurrent signal assignment + process interaction (Bug 2)
 * ================================================================= */

static void test_vhdl_concurrent_assign_proc(void)
{
    /* Process with concurrent signal assignments — should work identically
     * to inlining the same assignments inside the process. */
    const char *src =
        "entity test_ca is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d: in std_logic_vector(7 downto 0);\n"
        "        q: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_ca is\n"
        "  signal d_int : std_logic_vector(7 downto 0);\n"
        "  signal q_int : std_logic_vector(7 downto 0);\n"
        "begin\n"
        "  d_int <= d;\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      q_int <= \"00000000\";\n"
        "    elsif clk = '1' then\n"
        "      q_int <= d_int;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= q_int;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test_ca.vhd", src, strlen(src));
    mu_assert(r.success, "parse test_ca");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate test_ca");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_42 = qsim_bit_vector_from_str("00101010");

    /* Reset: rst=1, d=42 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "d", d_42);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    uint64_t q_u64;
    mu_assert(uir_bv_to_u64(qv, &q_u64) == 0 || q_u64 == 0, "q = 0 after reset");

    /* Release reset, clock posedge: q should capture d=42 */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(uir_bv_to_u64(qv, &q_u64) != 0, "q known after clk");
    mu_assert_eq(q_u64, 42, "q = 42 after clk");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d_42);
    uir_destroy_design_unit(r.unit);
}

/* Concurrent assignment with slices (Bug 2 reproduction with slice syntax) */
static void test_vhdl_concurrent_assign_slice(void)
{
    const char *src =
        "entity test_ca_slice is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d: in std_logic_vector(15 downto 0);\n"
        "        q: out std_logic_vector(15 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_ca_slice is\n"
        "  signal d_int : std_logic_vector(15 downto 0);\n"
        "  signal q_int : std_logic_vector(15 downto 0);\n"
        "begin\n"
        "  d_int <= d(15 downto 0);\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      q_int <= \"0000000000000000\";\n"
        "    elsif clk = '1' then\n"
        "      q_int <= d_int;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= q_int;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test_ca_slice.vhd", src, strlen(src));
    mu_assert(r.success, "parse test_ca_slice");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate test_ca_slice");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_val = qsim_bit_vector_from_str("0000000000101010");

    /* Reset: rst=1 */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "d", d_val);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    uint64_t q_u64 = 0;
    mu_assert(uir_bv_to_u64(qv, &q_u64) == 0 || q_u64 == 0, "q = 0 after reset");

    /* Release reset, clock: q should capture d=42 */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(uir_bv_to_u64(qv, &q_u64) != 0, "q known after clk");
    mu_assert_eq(q_u64, 42, "q = 42 after clk (slice ca)");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d_val);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_concurrent_assign_multi(void)
{
    /* Multiple concurrent signal assignments before a clocked process
     * (Bug 2 reproduction — pe_inline_ca pattern from NPU doc). */
    const char *src =
        "entity test_ca_multi is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d0: in std_logic_vector(15 downto 0);\n"
        "        d1: in std_logic_vector(15 downto 0);\n"
        "        q0: out std_logic_vector(15 downto 0);\n"
        "        q1: out std_logic_vector(15 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_ca_multi is\n"
        "  signal w_int : std_logic_vector(15 downto 0);\n"
        "  signal a_int : std_logic_vector(15 downto 0);\n"
        "  signal q0_int : std_logic_vector(15 downto 0);\n"
        "  signal q1_int : std_logic_vector(15 downto 0);\n"
        "begin\n"
        "  w_int <= d0(15 downto 0);\n"
        "  a_int <= d1(15 downto 0);\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      q0_int <= \"0000000000000000\";\n"
        "      q1_int <= \"0000000000000000\";\n"
        "    elsif clk = '1' then\n"
        "      q0_int <= w_int;\n"
        "      q1_int <= a_int;\n"
        "    end if;\n"
        "  end process;\n"
        "  q0 <= q0_int;\n"
        "  q1 <= q1_int;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test_ca_multi.vhd", src, strlen(src));
    mu_assert(r.success, "parse test_ca_multi");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate test_ca_multi");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d0_val = qsim_bit_vector_from_str("0000000000000101");
    qsim_bit_vector_t *d1_val = qsim_bit_vector_from_str("0000000000001010");

    /* Reset */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "d0", d0_val);
    uir_sim_set_signal(sim, "d1", d1_val);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv0 = uir_sim_get_signal(sim, "q0");
    qsim_bit_vector_t *qv1 = uir_sim_get_signal(sim, "q1");
    mu_assert_not_null(qv0);
    mu_assert_not_null(qv1);
    uint64_t q0u = 0, q1u = 0;
    mu_assert(uir_bv_to_u64(qv0, &q0u) == 0 || q0u == 0, "q0 = 0 after reset");
    mu_assert(uir_bv_to_u64(qv1, &q1u) == 0 || q1u == 0, "q1 = 0 after reset");

    /* Release reset, clock posedge */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv0 = uir_sim_get_signal(sim, "q0");
    qv1 = uir_sim_get_signal(sim, "q1");
    mu_assert_not_null(qv0);
    mu_assert_not_null(qv1);
    mu_assert(uir_bv_to_u64(qv0, &q0u) != 0, "q0 known after clk");
    mu_assert(uir_bv_to_u64(qv1, &q1u) != 0, "q1 known after clk");
    mu_assert_eq((int)q0u, 5, "q0 = 5 after clk");
    mu_assert_eq((int)q1u, 10, "q1 = 10 after clk");

    /* Second cycle: change d values */
    qsim_bit_vector_t *d0_new = qsim_bit_vector_from_str("0000000000010100");
    qsim_bit_vector_t *d1_new = qsim_bit_vector_from_str("0000000000101000");
    uir_sim_set_signal(sim, "d0", d0_new);
    uir_sim_set_signal(sim, "d1", d1_new);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);

    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv0 = uir_sim_get_signal(sim, "q0");
    qv1 = uir_sim_get_signal(sim, "q1");
    mu_assert(uir_bv_to_u64(qv0, &q0u) != 0, "q0 known 2nd cycle");
    mu_assert(uir_bv_to_u64(qv1, &q1u) != 0, "q1 known 2nd cycle");
    mu_assert_eq((int)q0u, 20, "q0 = 20 2nd cycle");
    mu_assert_eq((int)q1u, 40, "q1 = 40 2nd cycle");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d0_val);
    qsim_bit_vector_free(d1_val);
    qsim_bit_vector_free(d0_new);
    qsim_bit_vector_free(d1_new);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 18. Double-slice expression in unused branch (Bug 3)
 * ================================================================= */

static void test_vhdl_double_slice(void)
{
    /* Double-slice expression in an ELSE branch that is NOT taken.
     * The mere presence of double-slice should NOT break the process. */
    const char *src =
        "entity test_dslice is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        sel: in std_logic;\n"
        "        d_in: in std_logic_vector(15 downto 0);\n"
        "        d_out: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_dslice is\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      d_out <= \"00000000\";\n"
        "    elsif clk = '1' then\n"
        "      if sel = '0' then\n"
        "        d_out <= d_in(7 downto 0);\n"
        "      else\n"
        "        d_out <= d_in(15 downto 0)(7 downto 4) & d_in(3 downto 0);\n"
        "      end if;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test_dslice.vhd", src, strlen(src));
    mu_assert(r.success, "parse test_dslice");
    mu_assert_eq(r.unit->port_count, 5, "5 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate test_dslice");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *sel_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_val = qsim_bit_vector_from_str("0000000010101010");

    /* Reset */
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "sel", sel_0);
    uir_sim_set_signal(sim, "d_in", d_val);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "d_out");
    mu_assert_not_null(qv);
    uint64_t q_u64;
    mu_assert(uir_bv_to_u64(qv, &q_u64) == 0 || q_u64 == 0, "d_out = 0 after reset");

    /* Release reset, sel='0', clock: should use d_in(7 downto 0) = 0xAA = 170 */
    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "d_out");
    mu_assert_not_null(qv);
    printf("  d_out = %s\n", qsim_bit_vector_to_str(qv));
    mu_assert(uir_bv_to_u64(qv, &q_u64) != 0, "d_out known after clk, sel=0");
    mu_assert_eq(q_u64, 0xAA, "d_out = 0xAA with sel=0 (simple slice)");

    /* Set sel='1', clock: should use d_in(15 downto 0)(7 downto 4) & d_in(3 downto 0) */
    qsim_bit_vector_t *sel_1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "sel", sel_1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "d_out");
    mu_assert_not_null(qv);
    printf("  d_out(sel=1) = %s\n", qsim_bit_vector_to_str(qv));
    mu_assert(uir_bv_to_u64(qv, &q_u64) != 0, "d_out known after clk, sel=1");
    /* d_in = "0000000010101010", d_in(15 downto 0)(7 downto 4) = "1010", d_in(3 downto 0) = "1010" */
    mu_assert_eq(q_u64, 0xAA, "d_out = 0xAA with sel=1 (double-slice)");

    qsim_bit_vector_free(sel_1);
    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(sel_0);
    qsim_bit_vector_free(d_val);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 19. Range aggregate in case RHS (Bug 4)
 * ================================================================= */

static void test_vhdl_range_aggregate_case(void)
{
    /* Range aggregate (N downto M => '0') & sig directly in process.
     * Previously caused parse corruption and ALL processes to stop executing. */
    const char *src =
        "entity test_agg is\n"
        "  port (clk: in std_logic; rst_n: in std_logic;\n"
        "        d_in: in std_logic_vector(3 downto 0);\n"
        "        d_out: out std_logic_vector(31 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_agg is\n"
        "begin\n"
        "  process(clk, rst_n) is\n"
        "  begin\n"
        "    if rst_n = '0' then\n"
        "      d_out <= (others => '0');\n"
        "    elsif clk = '1' then\n"
        "      d_out <= (31 downto 4 => '0') & d_in;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test_agg.vhd", src, strlen(src));
    mu_assert(r.success, "parse test_agg");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate test_agg");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *din_5 = qsim_bit_vector_from_str("0101");

    /* Release reset, set inputs */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_set_signal(sim, "rst_n", rst_1);
    uir_sim_set_signal(sim, "d_in", din_5);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "d_out");
    mu_assert_not_null(qv);
    uint64_t q_u64;

    /* Wait, rst_1 = '1'. The sensitivity list is (clk, rst_n, d_in).
     * rst_n=1 means rst_n changed from 0→1 → process evaluates.
     * Since rst_n='1' and clk='0', none of the if branches trigger.
     * d_out stays at initial 'X'. That's fine. */

    /* Now drive clk='1': process evaluates, clk='1' branch: d_out <= agg & d_in */
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "d_out");
    mu_assert_not_null(qv);
    printf("  d_out after clk = %s\n", qsim_bit_vector_to_str(qv));
    mu_assert(uir_bv_to_u64(qv, &q_u64) != 0, "d_out known");
    mu_assert_eq(q_u64, 5, "d_out = 5 (range aggregate & d_in)");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(din_5);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 20. Multiple entity instances (Bug 1 reproduction)
 * ================================================================= */

static void test_vhdl_bug1_dual_std_logic(void)
{
    /* Same as test_vhdl_dual_instance in test_simulator.c but using
     * std_logic/std_logic_vector types, component instantiation. */
    const char *sub_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity sub is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d: in std_logic_vector(3 downto 0);\n"
        "        q: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of sub is\n"
        "  signal r: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      r <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      r <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= r;\n"
        "end architecture;\n";

    const char *parent_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity dual is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d0: in std_logic_vector(3 downto 0);\n"
        "        q0: out std_logic_vector(3 downto 0);\n"
        "        d1: in std_logic_vector(3 downto 0);\n"
        "        q1: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of dual is\n"
        "  component sub is\n"
        "    port (clk: in std_logic; rst: in std_logic;\n"
        "          d: in std_logic_vector(3 downto 0);\n"
        "          q: out std_logic_vector(3 downto 0));\n"
        "  end component;\n"
        "begin\n"
        "  u0: sub port map(clk => clk, rst => rst, d => d0, q => q0);\n"
        "  u1: sub port map(clk => clk, rst => rst, d => d1, q => q1);\n"
        "end architecture;\n";

    vhdl_parse_result_t sub_r = vhdl_parse("sub.vhd", sub_src, strlen(sub_src));
    mu_assert(sub_r.success, "parse sub");
    vhdl_parse_result_t dual_r = vhdl_parse("dual.vhd", parent_src, strlen(parent_src));
    mu_assert(dual_r.success, "parse dual");

    uir_design_unit_t *units[] = {dual_r.unit, sub_r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d0_5 = qsim_bit_vector_from_str("0101");
    qsim_bit_vector_t *d1_A = qsim_bit_vector_from_str("1010");

    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "d0", d0_5);
    uir_sim_set_signal(sim, "d1", d1_A);
    uir_sim_run(sim, 5);

    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *q0 = uir_sim_get_signal(sim, "q0");
    qsim_bit_vector_t *q1 = uir_sim_get_signal(sim, "q1");
    mu_assert_not_null(q0);
    mu_assert_not_null(q1);
    uint64_t q0v = 0, q1v = 0;
    int q0_ok = uir_bv_to_u64(q0, &q0v);
    int q1_ok = uir_bv_to_u64(q1, &q1v);
    printf("  q0_ok=%d q0v=%llu q1_ok=%d q1v=%llu\n", q0_ok, (unsigned long long)q0v, q1_ok, (unsigned long long)q1v);
    mu_assert(q0_ok != 0, "q0 known");
    mu_assert(q1_ok != 0, "q1 known");
    mu_assert_eq((int)q0v, 5, "q0 = 5");
    mu_assert_eq((int)q1v, 10, "q1 = 10");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d0_5);
    qsim_bit_vector_free(d1_A);
    uir_destroy_design_unit(dual_r.unit);
    uir_destroy_design_unit(sub_r.unit);
}

static void test_vhdl_bug1_entity_work(void)
{
    /* Same as test_vhdl_bug1_dual_std_logic but using direct entity
     * instantiation syntax: u0: entity work.sub port map(...) */
    const char *sub_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity sub is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d: in std_logic_vector(3 downto 0);\n"
        "        q: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of sub is\n"
        "  signal r: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      r <= \"0000\";\n"
        "    elsif clk = '1' then\n"
        "      r <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= r;\n"
        "end architecture;\n";

    const char *parent_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity dual is\n"
        "  port (clk: in std_logic; rst: in std_logic;\n"
        "        d0: in std_logic_vector(3 downto 0);\n"
        "        q0: out std_logic_vector(3 downto 0);\n"
        "        d1: in std_logic_vector(3 downto 0);\n"
        "        q1: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture rtl of dual is\n"
        "begin\n"
        "  u0: entity work.sub port map(clk => clk, rst => rst, d => d0, q => q0);\n"
        "  u1: entity work.sub port map(clk => clk, rst => rst, d => d1, q => q1);\n"
        "end architecture;\n";

    vhdl_parse_result_t sub_r = vhdl_parse("sub.vhd", sub_src, strlen(sub_src));
    mu_assert(sub_r.success, "parse sub");
    vhdl_parse_result_t dual_r = vhdl_parse("dual.vhd", parent_src, strlen(parent_src));
    mu_assert(dual_r.success, "parse dual (entity work.sub)");

    uir_design_unit_t *units[] = {dual_r.unit, sub_r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d0_5 = qsim_bit_vector_from_str("0101");
    qsim_bit_vector_t *d1_A = qsim_bit_vector_from_str("1010");

    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_set_signal(sim, "rst", rst_1);
    uir_sim_set_signal(sim, "d0", d0_5);
    uir_sim_set_signal(sim, "d1", d1_A);
    uir_sim_run(sim, 5);

    uir_sim_set_signal(sim, "rst", rst_0);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);

    qsim_bit_vector_t *q0 = uir_sim_get_signal(sim, "q0");
    qsim_bit_vector_t *q1 = uir_sim_get_signal(sim, "q1");
    mu_assert_not_null(q0);
    mu_assert_not_null(q1);
    uint64_t q0v = 0, q1v = 0;
    int q0_ok = uir_bv_to_u64(q0, &q0v);
    int q1_ok = uir_bv_to_u64(q1, &q1v);
    printf("  q0_ok=%d q0v=%llu q1_ok=%d q1v=%llu\n", q0_ok, (unsigned long long)q0v, q1_ok, (unsigned long long)q1v);
    mu_assert(q0_ok != 0, "q0 known");
    mu_assert(q1_ok != 0, "q1 known");
    mu_assert_eq((int)q0v, 5, "q0 = 5");
    mu_assert_eq((int)q1v, 10, "q1 = 10");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(d0_5);
    qsim_bit_vector_free(d1_A);
    uir_destroy_design_unit(dual_r.unit);
    uir_destroy_design_unit(sub_r.unit);
}

static void test_vhdl_bug1_pe_dual(void)
{
    /* Reproduce NPU Bug 1: two entity work.pe_test instances with
     * numeric_std arithmetic, direct API (matching test_bug1.c). */
    const char *pe_src =
        "library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;\n"
        "entity pe_test is\n"
        "  port (clk: in std_logic; rst_n: in std_logic;\n"
        "        weight_load_en: in std_logic;\n"
        "        weight_in: in std_logic_vector(15 downto 0);\n"
        "        act_in: in std_logic_vector(15 downto 0);\n"
        "        acc_in: in std_logic_vector(31 downto 0);\n"
        "        acc_out: out std_logic_vector(31 downto 0));\n"
        "end entity;\n"
        "architecture rtl of pe_test is\n"
        "  signal weight_reg: std_logic_vector(15 downto 0);\n"
        "begin\n"
        "  process(clk, rst_n) is\n"
        "  begin\n"
        "    if rst_n = '0' then\n"
        "      weight_reg <= (others => '0');\n"
        "      acc_out <= (others => '0');\n"
        "    elsif clk = '1' then\n"
        "      if weight_load_en = '1' then\n"
        "        weight_reg <= weight_in;\n"
        "      end if;\n"
        "      acc_out <= std_logic_vector(unsigned(acc_in) + (unsigned(act_in) * unsigned(weight_reg)));\n"
        "    end if;\n"
        "  end process;\n"
        "end rtl;\n";

    const char *dual_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity test_bug1_dual is\n"
        "  port (clk: in std_logic; rst_n: in std_logic;\n"
        "        weight_a: in std_logic_vector(15 downto 0);\n"
        "        act_a: in std_logic_vector(15 downto 0);\n"
        "        acc_a: in std_logic_vector(31 downto 0);\n"
        "        weight_b: in std_logic_vector(15 downto 0);\n"
        "        act_b: in std_logic_vector(15 downto 0);\n"
        "        acc_b: in std_logic_vector(31 downto 0);\n"
        "        out_a: out std_logic_vector(31 downto 0);\n"
        "        out_b: out std_logic_vector(31 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_bug1_dual is\n"
        "begin\n"
        "  u0: entity work.pe_test port map(\n"
        "    clk => clk, rst_n => rst_n,\n"
        "    weight_load_en => '1', weight_in => weight_a,\n"
        "    act_in => act_a, acc_in => acc_a,\n"
        "    acc_out => out_a);\n"
        "  u1: entity work.pe_test port map(\n"
        "    clk => clk, rst_n => rst_n,\n"
        "    weight_load_en => '1', weight_in => weight_b,\n"
        "    act_in => act_b, acc_in => acc_b,\n"
        "    acc_out => out_b);\n"
        "end rtl;\n";

    vhdl_parse_result_t pe_r = vhdl_parse("pe_test.vhd", pe_src, strlen(pe_src));
    mu_assert(pe_r.success, "parse pe_test");
    vhdl_parse_result_t dual_r = vhdl_parse("test_bug1_dual.vhd", dual_src, strlen(dual_src));
    mu_assert(dual_r.success, "parse dual (entity work.pe_test)");

    uir_design_unit_t *units[] = {dual_r.unit, pe_r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 2);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 2);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *rst_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *rst_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *w_a = qsim_bit_vector_from_str("0000000000000101");
    qsim_bit_vector_t *a_a = qsim_bit_vector_from_str("0000000000000011");
    qsim_bit_vector_t *acc_a = qsim_bit_vector_from_str("00000000000000000000000000000000");
    qsim_bit_vector_t *w_b = qsim_bit_vector_from_str("0000000000000111");
    qsim_bit_vector_t *a_b = qsim_bit_vector_from_str("0000000000000010");
    qsim_bit_vector_t *acc_b = qsim_bit_vector_from_str("00000000000000000000000000000000");

    /* Reset */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_set_signal(sim, "rst_n", rst_0);
    uir_sim_run(sim, 5);
    uir_sim_set_signal(sim, "rst_n", rst_1);
    uir_sim_run(sim, 5);

    /* Load weight: weight_a=5, weight_b=7, act=0, acc=0 */
    uir_sim_set_signal(sim, "weight_a", w_a);
    uir_sim_set_signal(sim, "act_a", a_a);
    uir_sim_set_signal(sim, "acc_a", acc_a);
    uir_sim_set_signal(sim, "weight_b", w_b);
    uir_sim_set_signal(sim, "act_b", a_b);
    uir_sim_set_signal(sim, "acc_b", acc_b);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    /* Compute cycle: weight loaded, multiply happens */
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 10);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *out_a = uir_sim_get_signal(sim, "out_a");
    qsim_bit_vector_t *out_b = uir_sim_get_signal(sim, "out_b");
    mu_assert_not_null(out_a);
    mu_assert_not_null(out_b);
    uint64_t out_av = 0, out_bv = 0;
    int a_ok = uir_bv_to_u64(out_a, &out_av);
    int b_ok = uir_bv_to_u64(out_b, &out_bv);
    printf("  out_a=%llu out_b=%llu (expect 15, 14)\n",
           (unsigned long long)out_av, (unsigned long long)out_bv);
    mu_assert(a_ok != 0, "out_a known");
    mu_assert(b_ok != 0, "out_b known");
    mu_assert_eq((int)out_av, 15, "out_a = 15 (weight=5 * act=3)");
    mu_assert_eq((int)out_bv, 14, "out_b = 14 (weight=7 * act=2)");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(rst_0);
    qsim_bit_vector_free(rst_1);
    qsim_bit_vector_free(w_a);
    qsim_bit_vector_free(a_a);
    qsim_bit_vector_free(acc_a);
    qsim_bit_vector_free(w_b);
    qsim_bit_vector_free(a_b);
    qsim_bit_vector_free(acc_b);
    uir_destroy_design_unit(pe_r.unit);
    uir_destroy_design_unit(dual_r.unit);
}

void register_vhdl_simulator_tests(void)
{
    printf("[VHDL Simulator]\n");
    mu_run_test(test_vhdl_counter_sim);
    mu_run_test(test_vhdl_dff_sim);
    mu_run_test(test_vhdl_alu_sim);
    mu_run_test(test_vhdl_shift_reg_sim);
    mu_run_test(test_vhdl_compare_sim);
    mu_run_test(test_vhdl_prio_enc_sim);
    mu_run_test(test_vhdl_multi_proc_sim);
    mu_run_test(test_numeric_std_unsigned_sim);
    mu_run_test(test_numeric_std_signed_sim);
    mu_run_test(test_numeric_std_to_integer_sim);
    mu_run_test(test_numeric_std_nested_sim);
    mu_run_test(test_vhdl_multi_driver_resolve_sim);
    mu_run_test(test_vhdl_clocked_vector_process_sim);
    mu_run_test(test_vhdl_others_aggregate_sim);

    /* 8. Rising_edge / falling_edge built-in functions */
    mu_run_test(test_vhdl_rising_edge_dff_sim);
    mu_run_test(test_vhdl_falling_edge_sim);

    /* 9. Selected-name function calls (lib.pkg.func) */
    mu_run_test(test_selected_name_rising_edge_sim);
    mu_run_test(test_selected_name_numeric_std_sim);

    /* 10. Array-typed signals */
    mu_run_test(test_vhdl_array_signal_sim);
    mu_run_test(test_vhdl_array_type_decl_sim);
    mu_run_test(test_vhdl_array_index_with_to_integer_sim);

    /* 11. Numeric_std shift functions */
    mu_run_test(test_vhdl_shift_left_sim);
    mu_run_test(test_vhdl_shift_right_sim);
    mu_run_test(test_vhdl_to_unsigned_sim);

    /* 12. Port array dimensions */
    mu_run_test(test_port_array_dimensions);

    /* 13. Multi-dimensional array type */
    /* mu_run_test(test_multi_dim_array_type); -- TODO: parser: multi-dim type tracking */

    /* 14. Concurrent signal assignment */
    mu_run_test(test_concurrent_assign_constant);
    mu_run_test(test_concurrent_assign_signal_dep);
    mu_run_test(test_concurrent_assign_process_driven);
    /* mu_run_test(test_concurrent_assign_when_else); -- TODO: concurrent when-else eval */

    /* 15. Process(all) auto-sensitivity */
    /* mu_run_test(test_vhdl_process_all_sim); -- TODO: process(all) auto-sensitivity */

    /* 16. With-select (selected signal assignment) */
    mu_run_test(test_with_select_simple);
    mu_run_test(test_with_select_single_bit);
    mu_run_test(test_with_select_only_others);
    mu_run_test(test_with_select_seq);
    mu_run_test(test_with_select_multi_cycle);

    /* 17. Concurrent signal assignment + process interaction */
    mu_run_test(test_vhdl_concurrent_assign_proc);
    mu_run_test(test_vhdl_concurrent_assign_slice);
    mu_run_test(test_vhdl_concurrent_assign_multi);

    /* 18. Double-slice expression (Bug 3) */
    mu_run_test(test_vhdl_double_slice);

    /* 19. Range aggregate in case RHS (Bug 4) */
    mu_run_test(test_vhdl_range_aggregate_case);

    /* 20. Multiple entity instances (Bug 1) */
    mu_run_test(test_vhdl_bug1_dual_std_logic);
    mu_run_test(test_vhdl_bug1_entity_work);
    mu_run_test(test_vhdl_bug1_pe_dual);
    mu_run_test(test_vhdl_bug1_session_api);

    /* 21. Generate (for generate, if generate) */
    mu_run_test(test_vhdl_if_generate_sim);
    mu_run_test(test_vhdl_for_generate_sim);
    mu_run_test(test_vhdl_for_generate_downto_sim);

    /* 22. TEXTIO file I/O */
    /* mu_run_test(test_vhdl_textio_readline_sim); -- TODO: TEXTIO readline path resolution */
    mu_run_test(test_vhdl_assert_sim);
    mu_run_test(test_vhdl_report_sim);
    mu_run_test(test_vhdl_alias_sim);
    printf("\n");
}

static void test_vhdl_bug1_session_api(void)
{
    /* NPU-matching test: use session API with library-prefix VHDL + open port map.
     * This validates that is_vhdl_src() recognizes "library" prefix and the
     * port_map_item grammar accepts "open" as a port actual. */
    const char *pe_src =
        "library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;\n"
        "entity pe_test is\n"
        "  port (clk: in std_logic; rst_n: in std_logic;\n"
        "        weight_load_en: in std_logic;\n"
        "        weight_in: in std_logic_vector(15 downto 0);\n"
        "        act_in: in std_logic_vector(15 downto 0);\n"
        "        acc_in: in std_logic_vector(31 downto 0);\n"
        "        act_out: out std_logic_vector(15 downto 0);\n"
        "        acc_out: out std_logic_vector(31 downto 0));\n"
        "end entity;\n"
        "architecture rtl of pe_test is\n"
        "  signal weight_reg: std_logic_vector(15 downto 0);\n"
        "begin\n"
        "  process(clk, rst_n) is\n"
        "  begin\n"
        "    if rst_n = '0' then\n"
        "      weight_reg <= (others => '0');\n"
        "      acc_out <= (others => '0');\n"
        "      act_out <= (others => '0');\n"
        "    elsif clk = '1' then\n"
        "      if weight_load_en = '1' then\n"
        "        weight_reg <= weight_in;\n"
        "      end if;\n"
        "      acc_out <= std_logic_vector(unsigned(acc_in) + (unsigned(act_in) * unsigned(weight_reg)));\n"
        "      act_out <= act_in;\n"
        "    end if;\n"
        "  end process;\n"
        "end rtl;\n";

    const char *dual_src =
        "library ieee; use ieee.std_logic_1164.all;\n"
        "entity test_bug1_dual is\n"
        "  port (clk: in std_logic; rst_n: in std_logic;\n"
        "        weight_a: in std_logic_vector(15 downto 0);\n"
        "        act_a: in std_logic_vector(15 downto 0);\n"
        "        acc_a: in std_logic_vector(31 downto 0);\n"
        "        weight_b: in std_logic_vector(15 downto 0);\n"
        "        act_b: in std_logic_vector(15 downto 0);\n"
        "        acc_b: in std_logic_vector(31 downto 0);\n"
        "        out_a: out std_logic_vector(31 downto 0);\n"
        "        out_b: out std_logic_vector(31 downto 0));\n"
        "end entity;\n"
        "architecture rtl of test_bug1_dual is\n"
        "begin\n"
        "  u0: entity work.pe_test port map(\n"
        "    clk => clk, rst_n => rst_n,\n"
        "    weight_load_en => '1', weight_in => weight_a,\n"
        "    act_in => act_a, acc_in => acc_a,\n"
        "    act_out => open, acc_out => out_a);\n"
        "  u1: entity work.pe_test port map(\n"
        "    clk => clk, rst_n => rst_n,\n"
        "    weight_load_en => '1', weight_in => weight_b,\n"
        "    act_in => act_b, acc_in => acc_b,\n"
        "    act_out => open, acc_out => out_b);\n"
        "end rtl;\n";

    /* Use session API path (compile_string) to test is_vhdl_src("library ...") */
    qsim_session_t *sess = qsim_session_create();
    mu_assert_not_null(sess);

    int ok = qsim_session_compile_string(sess, "pe_test.vhd", pe_src);
    mu_assert(ok != 0, "session compile pe_test (library prefix)");

    ok = qsim_session_compile_string(sess, "test_bug1_dual.vhd", dual_src);
    mu_assert(ok != 0, "session compile dual (act_out => open)");

    ok = qsim_session_elaborate(sess);
    mu_assert(ok != 0, "session elaborate");

    /* Simulate */
    qsim_session_force_str(sess, "clk", "0");
    qsim_session_force_str(sess, "rst_n", "0");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);
    qsim_session_force_str(sess, "rst_n", "1");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);

    qsim_session_force_str(sess, "weight_a", "0000000000000101"); /* 5 */
    qsim_session_force_str(sess, "act_a",   "0000000000000011"); /* 3 */
    qsim_session_force_str(sess, "acc_a",   "00000000000000000000000000000000");
    qsim_session_force_str(sess, "weight_b", "0000000000000111"); /* 7 */
    qsim_session_force_str(sess, "act_b",   "0000000000000010"); /* 2 */
    qsim_session_force_str(sess, "acc_b",   "00000000000000000000000000000000");

    qsim_session_force_str(sess, "clk", "1");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);
    qsim_session_force_str(sess, "clk", "0");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);

    /* Compute cycle: weight loaded, multiply happens */
    qsim_session_force_str(sess, "clk", "1");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);
    qsim_session_force_str(sess, "clk", "0");
    for (int i = 0; i < 10; i++) qsim_session_step_delta(sess);

    char *out_a = qsim_session_eval_str(sess, "out_a");
    char *out_b = qsim_session_eval_str(sess, "out_b");
    mu_assert_not_null(out_a);
    mu_assert_not_null(out_b);

    uint32_t out_av = 0, out_bv = 0;
    for (size_t i = 0; i < strlen(out_a); i++)
        if (out_a[i] == '1') out_av |= (1U << i);
    for (size_t i = 0; i < strlen(out_b); i++)
        if (out_b[i] == '1') out_bv |= (1U << i);

    printf("  out_a=%u out_b=%u (expect 15, 14)\n", out_av, out_bv);
    mu_assert_eq((int)out_av, 15, "out_a = 15 (weight=5 * act=3)");
    mu_assert_eq((int)out_bv, 14, "out_b = 14 (weight=7 * act=2)");

    qsim_session_free_str(out_a);
    qsim_session_free_str(out_b);
    qsim_session_free(sess);
}

/* =================================================================
 * 8. Rising_edge / falling_edge simulation tests
 * ================================================================= */

static void test_vhdl_rising_edge_dff_sim(void)
{
    const char *src =
        "entity dff is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic; qn: out std_logic);\n"
        "end entity;\n"
        "architecture behav of dff is\n"
        "  signal s_q: std_logic;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if rising_edge(clk) then\n"
        "      s_q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s_q;\n"
        "  qn <= not s_q;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("dff.vhd", src, strlen(src));
    mu_assert(r.success, "parse rising_edge dff");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate rising_edge dff");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Drive d=1, toggle clk low→high: q should become 1, qn should become 0 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after rising_edge d=1");

    qsim_bit_vector_t *qnv = uir_sim_get_signal(sim, "qn");
    mu_assert_not_null(qnv);
    mu_assert(qsim_bit_get(qnv, 0).state == QSIM_0, "qn = 0 after rising_edge d=1");

    /* Toggle clk high→low: q should stay 1 (falling edge should not trigger) */
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q stays 1 on falling edge");

    /* Drive d=0, toggle clk low→high: q should become 0 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after rising_edge d=0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_0);
    qsim_bit_vector_free(d_1);
}

static void test_vhdl_falling_edge_sim(void)
{
    const char *src =
        "entity dff is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic; qn: out std_logic);\n"
        "end entity;\n"
        "architecture behav of dff is\n"
        "  signal s_q: std_logic;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if falling_edge(clk) then\n"
        "      s_q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s_q;\n"
        "  qn <= not s_q;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("dff.vhd", src, strlen(src));
    mu_assert(r.success, "parse falling_edge dff");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate falling_edge dff");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Drive d=1, toggle clk high→low: q should become 1 (falling edge triggers) */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after falling_edge d=1");

    /* Toggle clk low→high: q should stay 1 (rising edge should not trigger) */
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 2);
    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q stays 1 on rising edge");

    /* Drive d=0, toggle clk high→low: q should become 0 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after falling_edge d=0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_0);
    qsim_bit_vector_free(d_1);
}

/* ── Selected-name simulation tests ── */

static void test_selected_name_rising_edge_sim(void)
{
    const char *src =
        "entity dff is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic; qn: out std_logic);\n"
        "end entity;\n"
        "architecture behav of dff is\n"
        "  signal s_q: std_logic;\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if ieee.std_logic_1164.rising_edge(clk) then\n"
        "      s_q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "  q <= s_q;\n"
        "  qn <= not s_q;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("dff.vhd", src, strlen(src));
    mu_assert(r.success, "parse selected-name dff");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate selected-name dff");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *clk_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *clk_1 = qsim_bit_vector_from_state(1, QSIM_1);
    qsim_bit_vector_t *d_0 = qsim_bit_vector_from_state(1, QSIM_0);
    qsim_bit_vector_t *d_1 = qsim_bit_vector_from_state(1, QSIM_1);

    /* Drive d=1, toggle clk low->high: q should become 1 */
    uir_sim_set_signal(sim, "d", d_1);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *qv = uir_sim_get_signal(sim, "q");
    mu_assert_not_null(qv);
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_1, "q = 1 after selected-name rising_edge d=1");

    qsim_bit_vector_t *qnv = uir_sim_get_signal(sim, "qn");
    mu_assert_not_null(qnv);
    mu_assert(qsim_bit_get(qnv, 0).state == QSIM_0, "qn = 0 after selected-name rising_edge d=1");

    /* Drive d=0, toggle clk low->high: q should become 0 */
    uir_sim_set_signal(sim, "d", d_0);
    uir_sim_run(sim, 1);
    uir_sim_set_signal(sim, "clk", clk_0);
    uir_sim_run(sim, 2);
    uir_sim_set_signal(sim, "clk", clk_1);
    uir_sim_run(sim, 5);

    qv = uir_sim_get_signal(sim, "q");
    mu_assert(qsim_bit_get(qv, 0).state == QSIM_0, "q = 0 after selected-name rising_edge d=0");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(clk_0);
    qsim_bit_vector_free(clk_1);
    qsim_bit_vector_free(d_0);
    qsim_bit_vector_free(d_1);
}

static void test_selected_name_numeric_std_sim(void)
{
    const char *src =
        "entity test is\n"
        "  port (a: in std_logic_vector(7 downto 0); r: out integer);\n"
        "end entity;\n"
        "architecture behav of test is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    r <= ieee.numeric_std.to_integer(ieee.numeric_std.unsigned(a));\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("test.vhd", src, strlen(src));
    mu_assert(r.success, "parse selected-name numeric_std");
    mu_assert_eq(r.unit->port_count, 2, "2 ports");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate selected-name numeric_std");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    qsim_bit_vector_t *a_val = qsim_bit_vector_alloc(8);
    for (int i = 0; i < 8; i++)
        qsim_bit_set(a_val, i, (i == 2) ? QSIM_VAL_1 : QSIM_VAL_0);

    uir_sim_set_signal(sim, "a", a_val);
    uir_sim_run(sim, 5);

    qsim_bit_vector_t *rv = uir_sim_get_signal(sim, "r");
    mu_assert_not_null(rv);
    /* a=4 -> to_integer(unsigned(a)) = 4 */
    uint64_t result_val = 0;
    for (uint32_t i = 0; i < rv->width && i < 64; i++) {
        if (qsim_bit_get(rv, i).state == QSIM_1) result_val |= (1ULL << i);
    }
    mu_assert_eq((int)result_val, 4, "to_integer(unsigned(a)) = 4");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_val);
}

/* =================================================================
 * 16. With-select (selected signal assignment) tests
 * ================================================================= */

static void test_with_select_simple(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(1 downto 0); y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= \"01\" when \"00\",\n"
        "         \"00\" when \"01\",\n"
        "         \"10\" when \"10\",\n"
        "         \"11\" when others;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse with_select_simple");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate with_select_simple");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a="00" -> y="01" */
    qsim_bit_vector_t *a_00 = qsim_bit_vector_from_state(2, QSIM_0);
    uir_sim_set_signal(sim, "a", a_00);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 for a=00");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_0, "y(1)=0 for a=00");

    /* a="10" -> y="10" */
    qsim_bit_vector_t *a_10 = qsim_bit_vector_alloc(2);
    qsim_bit_set(a_10, 0, QSIM_VAL_0);
    qsim_bit_set(a_10, 1, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", a_10);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0 for a=10");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 for a=10");

    /* a="11" (others) -> y="11" */
    qsim_bit_vector_t *a_11 = qsim_bit_vector_from_state(2, QSIM_1);
    uir_sim_set_signal(sim, "a", a_11);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 for a=11");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 for a=11");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_00);
    qsim_bit_vector_free(a_10);
    qsim_bit_vector_free(a_11);
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_single_bit(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= '1' when '0',\n"
        "         '0' when '1',\n"
        "         'X' when others;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse with_select_single_bit");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate with_select_single_bit");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a='0' -> y='1' */
    qsim_bit_vector_t *a_0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "a", a_0);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 for a=0");

    /* a='1' -> y='0' */
    qsim_bit_vector_t *a_1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "a", a_1);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y=0 for a=1");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_0);
    qsim_bit_vector_free(a_1);
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_only_others(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= '1' when others;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse with_select_only_others");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate with_select_only_others");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a='0' -> y='1' (others) */
    qsim_bit_vector_t *a_0 = qsim_bit_vector_from_state(1, QSIM_0);
    uir_sim_set_signal(sim, "a", a_0);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 for a=0");

    /* a='1' -> y='1' (others still) */
    qsim_bit_vector_t *a_1 = qsim_bit_vector_from_state(1, QSIM_1);
    uir_sim_set_signal(sim, "a", a_1);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 for a=1");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_0);
    qsim_bit_vector_free(a_1);
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_seq(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(1 downto 0); y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= \"01\" when \"00\",\n"
        "         \"11\" when others;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse with_select_seq");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate with_select_seq");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a="00" -> y="01" via explicit process */
    qsim_bit_vector_t *a_00 = qsim_bit_vector_from_state(2, QSIM_0);
    uir_sim_set_signal(sim, "a", a_00);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 for a=00 (seq)");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_0, "y(1)=0 for a=00 (seq)");

    /* a="11" -> y="11" (others) */
    qsim_bit_vector_t *a_11 = qsim_bit_vector_from_state(2, QSIM_1);
    uir_sim_set_signal(sim, "a", a_11);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 for a=11 (seq)");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 for a=11 (seq)");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a_00);
    qsim_bit_vector_free(a_11);
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_multi_cycle(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(1 downto 0); y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= \"01\" when \"00\",\n"
        "         \"10\" when \"01\",\n"
        "         \"11\" when others;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse with_select_multi_cycle");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate with_select_multi_cycle");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* a="00" -> y="01" */
    qsim_bit_vector_t *a = qsim_bit_vector_alloc(2);
    qsim_bit_set(a, 0, QSIM_VAL_0);
    qsim_bit_set(a, 1, QSIM_VAL_0);
    uir_sim_set_signal(sim, "a", a);
    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 cycle a=00");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_0, "y(1)=0 cycle a=00");

    /* a="01" -> y="10" */
    qsim_bit_set(a, 0, QSIM_VAL_1);
    qsim_bit_set(a, 1, QSIM_VAL_0);
    uir_sim_set_signal(sim, "a", a);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0 cycle a=01");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 cycle a=01");

    /* a="11" -> y="11" (others) */
    qsim_bit_set(a, 0, QSIM_VAL_1);
    qsim_bit_set(a, 1, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", a);
    uir_sim_run(sim, 5);
    yv = uir_sim_get_signal(sim, "y");
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y(0)=1 cycle a=11");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 cycle a=11");

    uir_sim_destroy(sim);
    qsim_bit_vector_free(a);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 21. Generate (for generate, if generate) simulation tests
 * ================================================================= */

static void test_vhdl_if_generate_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture gen of top is\n"
        "begin\n"
        "  if_true: if true generate\n"
        "    y <= \"1010\";\n"
        "  end generate;\n"
        "  if_false: if false generate\n"
        "    y <= \"1111\";\n"
        "  end generate;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse if_generate");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate if_generate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0 if_gen true branch");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 if_gen true branch");
    mu_assert(qsim_bit_get(yv, 2).state == QSIM_0, "y(2)=0 if_gen true branch");
    mu_assert(qsim_bit_get(yv, 3).state == QSIM_1, "y(3)=1 if_gen true branch");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_for_generate_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture gen of top is\n"
        "begin\n"
        "  gen: for i in 0 to 3 generate\n"
        "    y <= \"1010\";\n"
        "  end generate;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse for_generate");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate for_generate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0 for_gen");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_1, "y(1)=1 for_gen");
    mu_assert(qsim_bit_get(yv, 2).state == QSIM_0, "y(2)=0 for_gen");
    mu_assert(qsim_bit_get(yv, 3).state == QSIM_1, "y(3)=1 for_gen");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_for_generate_downto_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic_vector(3 downto 0));\n"
        "end entity;\n"
        "architecture gen of top is\n"
        "begin\n"
        "  gen: for i in 3 downto 0 generate\n"
        "    y <= \"1100\";\n"
        "  end generate;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse for_generate_downto");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate for_generate_downto");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);
    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_0, "y(0)=0 for_gen_downto");
    mu_assert(qsim_bit_get(yv, 1).state == QSIM_0, "y(1)=0 for_gen_downto");
    mu_assert(qsim_bit_get(yv, 2).state == QSIM_1, "y(2)=1 for_gen_downto");
    mu_assert(qsim_bit_get(yv, 3).state == QSIM_1, "y(3)=1 for_gen_downto");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 22. TEXTIO (file I/O) simulation tests
 * ================================================================= */

static void test_vhdl_textio_readline_sim(void)
{
    /* Create a temporary stimulus file */
    FILE *tf = fopen("_test_textio_in.txt", "w");
    mu_assert_not_null(tf);
    fprintf(tf, "10101010\n");
    fclose(tf);

    const char *src =
        "entity top is\n"
        "  port (val: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture sim of top is\n"
        "  file stimulus : text open read_mode is \"_test_textio_in.txt\";\n"
        "begin\n"
        "  process\n"
        "    variable L : line;\n"
        "    variable v : std_logic_vector(7 downto 0);\n"
        "  begin\n"
        "    if endfile(stimulus) = '0' then\n"
        "      readline(stimulus, L);\n"
        "      read(L, v);\n"
        "      val <= v;\n"
        "    end if;\n"
        "    wait;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    /* Run simulation — process runs in initial_eval, delta event fires */
    uir_sim_run(sim, 5);

    /* Check val is known — TEXTIO readline+read should have set it */
    qsim_bit_vector_t *val = uir_sim_get_signal(sim, "val");
    mu_assert_not_null(val);
    mu_assert(qsim_bit_get(val, 0).state != QSIM_X, "val not X after TEXTIO read");

    /* Verify "10101010": LSB-first, bit 0=0, bit 7=1 */
    mu_assert(qsim_bit_get(val, 0).state == QSIM_0, "val(0)=0");
    mu_assert(qsim_bit_get(val, 1).state == QSIM_1, "val(1)=1");
    mu_assert(qsim_bit_get(val, 2).state == QSIM_0, "val(2)=0");
    mu_assert(qsim_bit_get(val, 3).state == QSIM_1, "val(3)=1");
    mu_assert(qsim_bit_get(val, 4).state == QSIM_0, "val(4)=0");
    mu_assert(qsim_bit_get(val, 5).state == QSIM_1, "val(5)=1");
    mu_assert(qsim_bit_get(val, 6).state == QSIM_0, "val(6)=0");
    mu_assert(qsim_bit_get(val, 7).state == QSIM_1, "val(7)=1");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 22b. Assert/report simulation tests
 * ================================================================= */

static void test_vhdl_assert_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic);\n"
        "end entity;\n"
        "architecture sim of top is\n"
        "begin\n"
        "  process\n"
        "  begin\n"
        "    assert true;\n"
        "    y <= '1';\n"
        "    wait;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse assert");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 after process");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

static void test_vhdl_report_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic);\n"
        "end entity;\n"
        "architecture sim of top is\n"
        "begin\n"
        "  process\n"
        "  begin\n"
        "    report \"10\";\n"
        "    y <= '1';\n"
        "    wait;\n"
        "  end process;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse report");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 after process");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * 22c. Alias simulation test
 * ================================================================= */

static void test_vhdl_alias_sim(void)
{
    const char *src =
        "entity top is\n"
        "  port (y: out std_logic);\n"
        "end entity;\n"
        "architecture sim of top is\n"
        "  signal s : std_logic;\n"
        "  alias my_sig is s;\n"
        "begin\n"
        "  s <= '1';\n"
        "  y <= my_sig;\n"
        "end architecture;\n";

    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parse alias");

    uir_design_unit_t *units[] = {r.unit};
    uir_elab_result_t *elab = uir_elaborate(units, 1);
    mu_assert(elab->success != 0, "elaborate");
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, 1);
    mu_assert_not_null(sim);

    uir_sim_run(sim, 5);

    qsim_bit_vector_t *yv = uir_sim_get_signal(sim, "y");
    mu_assert_not_null(yv);
    mu_assert(qsim_bit_get(yv, 0).state == QSIM_1, "y=1 after alias resolution");

    uir_sim_destroy(sim);
    uir_destroy_design_unit(r.unit);
}
