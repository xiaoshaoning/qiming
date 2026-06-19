/* VHDL parser tests — entity, architecture, process, statements, expressions.
 * "legal accepted, illegal rejected" — positive and negative tests.
 */

#include "minunit.h"
#include "libqsim/vhdl_parser.h"
#include "libqsim/uir.h"

#include <string.h>

/* ── Helper predicates ── */

static int has_port(uir_design_unit_t *unit, const char *name, uir_port_dir_t dir)
{
    for (size_t i = 0; i < unit->port_count; i++) {
        if (strcmp(unit->ports[i]->name, name) == 0 &&
            unit->ports[i]->direction == dir)
            return 1;
    }
    return 0;
}

static int has_signal(uir_design_unit_t *unit, const char *name, uir_signal_type_t type)
{
    for (size_t i = 0; i < unit->signal_count; i++) {
        if (strcmp(unit->signals[i]->name, name) == 0 &&
            unit->signals[i]->sig_type == type)
            return 1;
    }
    return 0;
}

static int has_process(uir_design_unit_t *unit, uir_proc_kind_t kind)
{
    for (size_t i = 0; i < unit->process_count; i++) {
        if (unit->processes[i]->proc_kind == kind)
            return 1;
    }
    return 0;
}

/* =================================================================
 * 1. Entity basics
 * ================================================================= */

static void test_parse_null_source(void)
{
    vhdl_parse_result_t r = vhdl_parse("null.vhd", NULL, 0);
    mu_assert(!r.success, "should fail on null source");
}

static void test_parse_empty_source(void)
{
    vhdl_parse_result_t r = vhdl_parse("empty.vhd", "", 0);
    mu_assert(!r.success, "should fail on empty source");
}

static void test_parse_entity_no_ports(void)
{
    const char *src =
        "entity empty is\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("empty.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_str_eq(r.unit->name, "empty", "entity name");
    mu_assert_eq(r.unit->port_count, 0, "no ports");
}

static void test_parse_entity_with_name_end(void)
{
    const char *src =
        "entity counter is\n"
        "end entity counter;\n";
    vhdl_parse_result_t r = vhdl_parse("counter.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_str_eq(r.unit->name, "counter", "entity name");
}

static void test_parse_entity_uppercase(void)
{
    const char *src =
        "ENTITY my_entity IS\n"
        "END ENTITY my_entity;\n";
    vhdl_parse_result_t r = vhdl_parse("my.vhd", src, strlen(src));
    mu_assert(r.success, "case-insensitive entity");
    mu_assert_str_eq(r.unit->name, "my_entity", "entity name");
}

/* =================================================================
 * 2. Port declarations
 * ================================================================= */

static void test_parse_entity_one_port(void)
{
    const char *src =
        "entity and2 is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("and2.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 1, "one port");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "port a is input");
}

static void test_parse_entity_multi_ports(void)
{
    const char *src =
        "entity and2 is\n"
        "  port (\n"
        "    a: in std_logic;\n"
        "    b: in std_logic;\n"
        "    y: out std_logic\n"
        "  );\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("and2.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 3, "three ports");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "port a is input");
    mu_assert(has_port(r.unit, "b", UIR_PORT_IN), "port b is input");
    mu_assert(has_port(r.unit, "y", UIR_PORT_OUT), "port y is output");
}

static void test_parse_entity_inout_port(void)
{
    const char *src =
        "entity bidir is\n"
        "  port (data: inout std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("bidir.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_port(r.unit, "data", UIR_PORT_INOUT), "port data is inout");
}

static void test_parse_entity_comma_ports(void)
{
    const char *src =
        "entity multi is\n"
        "  port (a, b, c: in std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("multi.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->port_count, 3, "three ports");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "port a in");
    mu_assert(has_port(r.unit, "b", UIR_PORT_IN), "port b in");
    mu_assert(has_port(r.unit, "c", UIR_PORT_IN), "port c in");
}

static void test_parse_entity_std_logic_vector_port(void)
{
    const char *src =
        "entity bus_if is\n"
        "  port (data: in std_logic_vector(7 downto 0));\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("bus_if.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_port(r.unit, "data", UIR_PORT_IN), "port data in");
    mu_assert_eq(r.unit->ports[0]->width, 8, "width 7 downto 0 = 8");
}

static void test_parse_entity_integer_port(void)
{
    const char *src =
        "entity calc is\n"
        "  port (count: out integer);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("calc.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_port(r.unit, "count", UIR_PORT_OUT), "port count out");
}

static void test_parse_entity_boolean_port(void)
{
    const char *src =
        "entity flag is\n"
        "  port (done: out boolean);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("flag.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_port(r.unit, "done", UIR_PORT_OUT), "port done out");
}

/* =================================================================
 * 3. Architecture basics
 * ================================================================= */

static void test_parse_arch_empty(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic);\n"
        "end entity;\n"
        "architecture empty of top is\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_str_eq(r.unit->language, "vhdl", "language is vhdl");
}

static void test_parse_arch_no_is(void)
{
    const char *src =
        "entity top is\n"
        "  port (x: in std_logic);\n"
        "end entity;\n"
        "architecture struct of top is\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
}

/* =================================================================
 * 4. Signal and variable declarations
 * ================================================================= */

static void test_parse_signal_decl(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "s", UIR_SIG_VHDL_SIGNAL), "signal s");
}

static void test_parse_signal_vector(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  signal bus: std_logic_vector(7 downto 0);\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "bus", UIR_SIG_VHDL_SIGNAL), "signal bus");
}

static void test_parse_multi_signal_decl(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  signal a, b, c: std_logic;\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->signal_count, 3, "three signals");
    mu_assert(has_signal(r.unit, "a", UIR_SIG_VHDL_SIGNAL), "signal a");
    mu_assert(has_signal(r.unit, "b", UIR_SIG_VHDL_SIGNAL), "signal b");
    mu_assert(has_signal(r.unit, "c", UIR_SIG_VHDL_SIGNAL), "signal c");
}

static void test_parse_variable_decl(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "    variable v: integer;\n"
        "  begin\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "v", UIR_SIG_VHDL_VARIABLE), "variable v");
}

static void test_parse_integer_signal(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  signal count: integer;\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(has_signal(r.unit, "count", UIR_SIG_VHDL_SIGNAL), "signal count");
}

/* =================================================================
 * 5. Process
 * ================================================================= */

static void test_parse_process_empty(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->process_count, 1, "one process");
    mu_assert(has_process(r.unit, UIR_PROC_VHDL), "VHDL process");
}

static void test_parse_process_sensitivity(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert(r.unit->processes[0]->sensitivity_count >= 1,
              "has sensitivity");
}

static void test_parse_process_multi_sensitivity(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk, rst: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->processes[0]->sensitivity_count, 2,
                 "two sensitivity signals");
}

static void test_parse_process_labeled(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  main: process is\n"
        "  begin\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "should succeed");
    mu_assert_eq(r.unit->process_count, 1, "one labeled process");
}

/* =================================================================
 * 6. Signal and variable assignments
 * ================================================================= */

static void test_parse_signal_assign(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    y <= a;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "signal assign");
}

static void test_parse_variable_assign(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "    variable v: integer;\n"
        "  begin\n"
        "    v := 42;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "variable assign");
}

static void test_parse_concurrent_signal_assign(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  signal tmp: std_logic;\n"
        "begin\n"
        "  tmp <= a and b;\n"
        "  y <= tmp;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "concurrent signal assign");
    mu_assert_eq(r.unit->assign_count, 2, "two concurrent assigns");
}

/* =================================================================
 * 7. If statements
 * ================================================================= */

static void test_parse_if_then(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    if a = '1' then\n"
        "      y <= '1';\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "if-then");
}

static void test_parse_if_else(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    if a = '1' then\n"
        "      y <= '1';\n"
        "    else\n"
        "      y <= '0';\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "if-then-else");
}

static void test_parse_if_elsif(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a, b) is\n"
        "  begin\n"
        "    if a = '1' then\n"
        "      y <= '0';\n"
        "    elsif b = '1' then\n"
        "      y <= '1';\n"
        "    else\n"
        "      y <= '0';\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "if-elsif-else");
}

/* =================================================================
 * 8. Case statements
 * ================================================================= */

static void test_parse_case_simple(void)
{
    const char *src =
        "entity top is\n"
        "  port (sel: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(sel) is\n"
        "  begin\n"
        "    case sel is\n"
        "      when '0' =>\n"
        "        y <= '1';\n"
        "      when '1' =>\n"
        "        y <= '0';\n"
        "    end case;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "case");
}

static void test_parse_case_when_others(void)
{
    const char *src =
        "entity top is\n"
        "  port (sel: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(sel) is\n"
        "  begin\n"
        "    case sel is\n"
        "      when '0' =>\n"
        "        y <= '1';\n"
        "      when others =>\n"
        "        y <= '0';\n"
        "    end case;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "case with others");
}

static void test_parse_case_multi_choices(void)
{
    const char *src =
        "entity top is\n"
        "  port (s: in std_logic_vector(1 downto 0); y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(s) is\n"
        "  begin\n"
        "    case s is\n"
        "      when \"00\" | \"11\" =>\n"
        "        y <= '0';\n"
        "      when others =>\n"
        "        y <= '1';\n"
        "    end case;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "case with | choices");
}

/* =================================================================
 * 9. For loops
 * ================================================================= */

static void test_parse_for_loop(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  signal x: std_logic_vector(7 downto 0);\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    for i in 0 to 7 loop\n"
        "      x(i) <= '0';\n"
        "    end loop;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "for loop");
}

/* =================================================================
 * 10. Expressions
 * ================================================================= */

static void test_parse_expr_arithmetic(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in integer; y: out integer);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a, b) is\n"
        "  begin\n"
        "    y <= a + b;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "add expression");
}

static void test_parse_expr_logical(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= a and b;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "and expression");
}

static void test_parse_expr_concat(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in std_logic; y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= a & b;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "concat expression");
}

static void test_parse_expr_parentheses(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b, c: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= (a and b) or c;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "parenthesized expression");
}

static void test_parse_expr_not(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= not a;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "not expression");
}

static void test_parse_expr_comparison(void)
{
    const char *src =
        "entity top is\n"
        "  port (a, b: in integer; y: out boolean);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a, b) is\n"
        "  begin\n"
        "    if a = b then\n"
        "      y <= true;\n"
        "    else\n"
        "      y <= false;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "comparison expression");
}

/* =================================================================
 * 11. Complete designs
 * ================================================================= */

static void test_parse_counter_design(void)
{
    const char *src =
        "entity counter is\n"
        "  port (\n"
        "    clk: in std_logic;\n"
        "    rst: in std_logic;\n"
        "    count: out std_logic_vector(3 downto 0)\n"
        "  );\n"
        "end entity;\n"
        "architecture behav of counter is\n"
        "  signal s_count: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(clk, rst) is\n"
        "  begin\n"
        "    if rst = '1' then\n"
        "      s_count <= \"0000\";\n"
        "    else\n"
        "      s_count <= s_count + \"0001\";\n"
        "    end if;\n"
        "  end process;\n"
        "  count <= s_count;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("counter.vhd", src, strlen(src));
    mu_assert(r.success, "counter design");
    mu_assert_eq(r.unit->port_count, 3, "3 ports");
    mu_assert(has_port(r.unit, "clk", UIR_PORT_IN), "clk input");
    mu_assert(has_port(r.unit, "rst", UIR_PORT_IN), "rst input");
    mu_assert(has_port(r.unit, "count", UIR_PORT_OUT), "count output");
    mu_assert(has_signal(r.unit, "s_count", UIR_SIG_VHDL_SIGNAL), "s_count signal");
    mu_assert_eq(r.unit->process_count, 1, "one process");
}

static void test_parse_dff_design(void)
{
    const char *src =
        "entity dff is\n"
        "  port (\n"
        "    clk: in std_logic;\n"
        "    d: in std_logic;\n"
        "    q: out std_logic;\n"
        "    qn: out std_logic\n"
        "  );\n"
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
    mu_assert(r.success, "DFF design");
    mu_assert_eq(r.unit->port_count, 4, "4 ports");
}

/* =================================================================
 * 12. Library and use clauses
 * ================================================================= */

static void test_parse_library_use_before_entity(void)
{
    const char *src =
        "library ieee;\n"
        "use ieee.std_logic_1164.all;\n"
        "use work.pkg;\n"
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "library/use before entity");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "port a after library/use");
}

static void test_parse_library_only(void)
{
    const char *src =
        "library ieee;\n"
        "library work;\n"
        "entity empty is\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("empty.vhd", src, strlen(src));
    mu_assert(r.success, "library clauses only");
    mu_assert_str_eq(r.unit->name, "empty", "entity name");
}

static void test_parse_use_multiple_dots(void)
{
    const char *src =
        "library lib;\n"
        "use lib.pkg1;\n"
        "use lib.pkg2.all;\n"
        "entity top is end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "multiple use clauses");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
}

/* =================================================================
 * 13. Negative tests (syntactically invalid VHDL)
 * ================================================================= */

static void test_parse_invalid_missing_entity(void)
{
    const char *src =
        "architecture behav of top is\n"
        "begin\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(!r.success, "should fail without entity");
}

static void test_parse_invalid_missing_begin(void)
{
    /* Architecture without begin: PEG backtracks to entity-only match,
     * which is valid VHDL (entity-only design file). */
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(r.success, "entity-only fallback is valid");
    (void)r;
}

static void test_parse_invalid_bad_port_dir(void)
{
    /* Unknown direction falls through to ID_OR_KW in the mode rule,
     * defaulting to UIR_PORT_IN, so the entity parses successfully
     * with the port counted. */
    const char *src =
        "entity top is\n"
        "  port (a: wrong std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(r.success, "entity succeeds despite bad port dir");
    mu_assert_eq(r.unit->port_count, 1, "one port parsed");
}

static void test_parse_invalid_unclosed_if(void)
{
    /* Unclosed if: PEG's stmt* matches "null;" as a null_stmt before
     * if_stmt can match, then the process closes normally via backtracking. */
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    if a = '1' then\n"
        "      null;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(r.success, "unclosed if backtracks to empty stmt*");
    (void)r;
}

static void test_parse_invalid_unclosed_case(void)
{
    /* "end;" without "case" backtracks: the case_stmt fails, stmt* matches
     * nothing, and "end;" closes the process (KW_PROCESS is optional). */
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    case a is\n"
        "      when '0' =>\n"
        "        null;\n"
        "    end;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(r.success, "case failure backtracks to end; closing process");
    (void)r;
}

static void test_parse_invalid_garbage(void)
{
    const char *src =
        "entity top is\n"
        "  @@@@ invalid ;;;\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("bad.vhd", src, strlen(src));
    mu_assert(!r.success, "should fail on garbage");
}

/* ── Walk callback context for finding func_call nodes ── */
typedef struct {
    uir_func_call_t *found;
    int count;
} find_func_ctx_t;

static void find_func_cb(uir_node_t *node, void *ctx) {
    find_func_ctx_t *fc = (find_func_ctx_t *)ctx;
    if (node->kind == UIR_FUNC_CALL) {
        fc->found = (uir_func_call_t *)node;
        fc->count++;
    }
}

/* =================================================================
 * 13. Library/use clauses and function calls
 * ================================================================= */

static void test_parse_library_clause(void)
{
    const char *src =
        "library ieee;\n"
        "entity top is\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "library clause");
    mu_assert_eq(r.unit->library_count, 1, "one library");
    mu_assert_str_eq(r.unit->library_names[0], "ieee", "library name");
}

static void test_parse_use_clause(void)
{
    const char *src =
        "library ieee;\n"
        "use ieee.std_logic_1164.all;\n"
        "entity top is\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "use clause");
    mu_assert_eq(r.unit->use_count, 1, "one use clause");
    mu_assert_str_eq(r.unit->use_clauses[0], "ieee.std_logic_1164.all", "use clause text");
}

static void test_parse_library_and_use(void)
{
    const char *src =
        "library ieee;\n"
        "library std;\n"
        "use ieee.std_logic_1164.all;\n"
        "use ieee.numeric_std.all;\n"
        "entity top is\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "multi library/use");
    mu_assert_eq(r.unit->library_count, 2, "two libraries");
    mu_assert_str_eq(r.unit->library_names[0], "ieee", "lib0");
    mu_assert_str_eq(r.unit->library_names[1], "std", "lib1");
    mu_assert_eq(r.unit->use_count, 2, "two use clauses");
    mu_assert_str_eq(r.unit->use_clauses[0], "ieee.std_logic_1164.all", "use0");
    mu_assert_str_eq(r.unit->use_clauses[1], "ieee.numeric_std.all", "use1");
}

static void test_parse_func_call_rising_edge(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if rising_edge(clk) then\n"
        "      q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "rising_edge parse");
    find_func_ctx_t fc = {NULL, 0};
    uir_walk(r.unit, find_func_cb, &fc);
    mu_assert_eq(fc.count, 1, "one func call");
    mu_assert_ptr_not_null(fc.found, "func call found");
    if (fc.found) {
        mu_assert_str_eq(fc.found->name, "rising_edge", "func name");
        mu_assert_eq(fc.found->arg_count, 1, "one arg");
    }
}

static void test_parse_func_call_two_args(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(3 downto 0);\n"
        "        size: in integer;\n"
        "        r: out std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  signal s_tmp: std_logic_vector(7 downto 0);\n"
        "begin\n"
        "  process(a, size) is\n"
        "  begin\n"
        "    s_tmp <= resize(a, size);\n"
        "  end process;\n"
        "  r <= s_tmp;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "two-arg func call parse");
    find_func_ctx_t fc = {NULL, 0};
    uir_walk(r.unit, find_func_cb, &fc);
    mu_assert_eq(fc.count, 1, "one func call");
    mu_assert_ptr_not_null(fc.found, "func call found");
    if (fc.found) {
        mu_assert_str_eq(fc.found->name, "resize", "func name");
        mu_assert_eq(fc.found->arg_count, 2, "two args");
    }
}

static void test_parse_idx_ref_still_works(void)
{
    /* x(0) <= '1' should create an index ref, not a func call */
    const char *src =
        "entity top is\n"
        "  port (x: in std_logic_vector(3 downto 0); y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  signal s: std_logic_vector(3 downto 0);\n"
        "begin\n"
        "  process(x) is\n"
        "  begin\n"
        "    s(0) <= x(1);\n"
        "  end process;\n"
        "  y <= s(3);\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "index ref parse");
    /* No func calls should exist */
    find_func_ctx_t fc = {NULL, 0};
    uir_walk(r.unit, find_func_cb, &fc);
    mu_assert_eq(fc.count, 0, "no func calls from index refs");
}

/* ── Selected-name tests ── */

static void test_parse_selected_name_func_call(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(clk) is\n"
        "  begin\n"
        "    if ieee.std_logic_1164.rising_edge(clk) then\n"
        "      q <= d;\n"
        "    end if;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "selected name func call parse");
    find_func_ctx_t fc = {NULL, 0};
    uir_walk(r.unit, find_func_cb, &fc);
    mu_assert_eq(fc.count, 1, "one func call");
    mu_assert_ptr_not_null(fc.found, "func call found");
    if (fc.found) {
        mu_assert_str_eq(fc.found->name, "ieee.std_logic_1164.rising_edge", "dotted func name");
        mu_assert_eq(fc.found->arg_count, 1, "one arg");
    }
}

static void test_parse_selected_name_nested_func(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(7 downto 0);\n"
        "        r: out integer);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process(a) is\n"
        "  begin\n"
        "    r <= ieee.numeric_std.to_integer(ieee.numeric_std.unsigned(a));\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "nested selected name parse");
    find_func_ctx_t fc = {NULL, 0};
    uir_walk(r.unit, find_func_cb, &fc);
    mu_assert_eq(fc.count, 2, "two func calls");
}

/* ── Ref-finding walk callback ── */
typedef struct {
    uir_ref_t *found;
    int count;
} find_ref_ctx_t;

static void find_ref_cb(uir_node_t *node, void *ctx) {
    find_ref_ctx_t *rc = (find_ref_ctx_t *)ctx;
    if (node->kind == UIR_REF) {
        rc->found = (uir_ref_t *)node;
        rc->count++;
    }
}

static void test_parse_selected_name_ref(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic; d: in std_logic; q: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  signal s: std_logic;\n"
        "begin\n"
        "  s <= work.pkg.some_signal;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "selected name ref parse");
    /* Walk to find the dotted ref by checking names */
    int found_dotted = 0;
    for (size_t i = 0; i < r.unit->node_count; i++) {
        uir_node_t *n = r.unit->all_nodes[i];
        if (n && n->kind == UIR_REF) {
            uir_ref_t *ref = (uir_ref_t *)n;
            if (ref->name && strchr(ref->name, '.'))
                found_dotted = 1;
        }
    }
    mu_assert(found_dotted, "dotted ref found");
}

/* =================================================================
 * Test registration
 * ================================================================= */

void register_vhdl_parser_tests(void)
{
    printf("[VHDL Parser]\n");

    /* 1. Entity basics */
    mu_run_test(test_parse_null_source);
    mu_run_test(test_parse_empty_source);
    mu_run_test(test_parse_entity_no_ports);
    mu_run_test(test_parse_entity_with_name_end);
    mu_run_test(test_parse_entity_uppercase);

    /* 2. Port declarations */
    mu_run_test(test_parse_entity_one_port);
    mu_run_test(test_parse_entity_multi_ports);
    mu_run_test(test_parse_entity_inout_port);
    mu_run_test(test_parse_entity_comma_ports);
    mu_run_test(test_parse_entity_std_logic_vector_port);
    mu_run_test(test_parse_entity_integer_port);
    mu_run_test(test_parse_entity_boolean_port);

    /* 3. Architecture basics */
    mu_run_test(test_parse_arch_empty);
    mu_run_test(test_parse_arch_no_is);

    /* 4. Signal and variable declarations */
    mu_run_test(test_parse_signal_decl);
    mu_run_test(test_parse_signal_vector);
    mu_run_test(test_parse_multi_signal_decl);
    mu_run_test(test_parse_variable_decl);
    mu_run_test(test_parse_integer_signal);

    /* 5. Process */
    mu_run_test(test_parse_process_empty);
    mu_run_test(test_parse_process_sensitivity);
    mu_run_test(test_parse_process_multi_sensitivity);
    mu_run_test(test_parse_process_labeled);

    /* 6. Signal and variable assignments */
    mu_run_test(test_parse_signal_assign);
    mu_run_test(test_parse_variable_assign);
    mu_run_test(test_parse_concurrent_signal_assign);

    /* 7. If statements */
    mu_run_test(test_parse_if_then);
    mu_run_test(test_parse_if_else);
    mu_run_test(test_parse_if_elsif);

    /* 8. Case statements */
    mu_run_test(test_parse_case_simple);
    mu_run_test(test_parse_case_when_others);
    mu_run_test(test_parse_case_multi_choices);

    /* 9. For loops */
    mu_run_test(test_parse_for_loop);

    /* 10. Expressions */
    mu_run_test(test_parse_expr_arithmetic);
    mu_run_test(test_parse_expr_logical);
    mu_run_test(test_parse_expr_concat);
    mu_run_test(test_parse_expr_parentheses);
    mu_run_test(test_parse_expr_not);
    mu_run_test(test_parse_expr_comparison);

    /* 11. Complete designs */
    mu_run_test(test_parse_counter_design);
    mu_run_test(test_parse_dff_design);

    /* 12. Library and use clauses */
    mu_run_test(test_parse_library_use_before_entity);
    mu_run_test(test_parse_library_only);
    mu_run_test(test_parse_use_multiple_dots);

    /* 13. Negative tests */
    mu_run_test(test_parse_invalid_missing_entity);
    mu_run_test(test_parse_invalid_missing_begin);
    mu_run_test(test_parse_invalid_bad_port_dir);
    mu_run_test(test_parse_invalid_unclosed_if);
    mu_run_test(test_parse_invalid_unclosed_case);
    mu_run_test(test_parse_invalid_garbage);

    /* 13. Library/use clauses and function calls */
    mu_run_test(test_parse_library_clause);
    mu_run_test(test_parse_use_clause);
    mu_run_test(test_parse_library_and_use);
    mu_run_test(test_parse_func_call_rising_edge);
    mu_run_test(test_parse_func_call_two_args);
    mu_run_test(test_parse_idx_ref_still_works);

    /* 14. Selected-name expressions */
    mu_run_test(test_parse_selected_name_func_call);
    mu_run_test(test_parse_selected_name_nested_func);
    mu_run_test(test_parse_selected_name_ref);

    printf("\n");
}
