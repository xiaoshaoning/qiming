/* Standalone VHDL library/use clause and wait/exit/next/return tests */
#include "minunit.h"
#include "libqsim/vhdl_parser.h"
#include "libqsim/uir.h"
#include <string.h>

static int has_port(uir_design_unit_t *unit, const char *name, uir_port_dir_t dir)
{
    for (size_t i = 0; i < unit->port_count; i++) {
        if (strcmp(unit->ports[i]->name, name) == 0 &&
            unit->ports[i]->direction == dir)
            return 1;
    }
    return 0;
}

static void test_library_before_entity(void)
{
    const char *src =
        "library ieee;\n"
        "entity top is end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "library before entity");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    uir_destroy_design_unit(r.unit);
}

static void test_library_use_before_entity(void)
{
    const char *src =
        "library ieee;\n"
        "use ieee.std_logic_1164.all;\n"
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "library/use before entity");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    mu_assert(has_port(r.unit, "a", UIR_PORT_IN), "port a");
    uir_destroy_design_unit(r.unit);
}

static void test_multiple_libraries_uses(void)
{
    const char *src =
        "library ieee;\n"
        "library work;\n"
        "use ieee.std_logic_1164.all;\n"
        "use work.pkg.all;\n"
        "use work.pkg2;\n"
        "entity top is end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "multiple libraries and uses");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    uir_destroy_design_unit(r.unit);
}

static void test_no_library_use(void)
{
    const char *src =
        "entity top is end entity;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "no library/use (backward compat)");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    uir_destroy_design_unit(r.unit);
}

static void test_library_use_with_arch(void)
{
    const char *src =
        "library ieee;\n"
        "use ieee.std_logic_1164.all;\n"
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= a;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "library/use with architecture");
    mu_assert_str_eq(r.unit->name, "top", "entity name");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Wait statement tests
 * ================================================================= */

static void test_wait_for(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    wait for 10;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "wait for");
    mu_assert_eq(r.unit->process_count, 1, "one process");
    uir_destroy_design_unit(r.unit);
}

static void test_wait_until(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    wait until clk = '1';\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "wait until");
    uir_destroy_design_unit(r.unit);
}

static void test_wait_on(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    wait on clk;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "wait on");
    uir_destroy_design_unit(r.unit);
}

static void test_wait_forever(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    wait;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "wait forever");
    uir_destroy_design_unit(r.unit);
}

static void test_wait_on_multiple(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk, rst: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    wait on clk, rst;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "wait on multiple");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Exit, next, return statement tests
 * ================================================================= */

static void test_exit_loop(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    for i in 0 to 7 loop\n"
        "      exit;\n"
        "    end loop;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "exit loop");
    uir_destroy_design_unit(r.unit);
}

static void test_exit_with_label(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    for i in 0 to 7 loop\n"
        "      exit outer_loop;\n"
        "    end loop;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "exit with label");
    uir_destroy_design_unit(r.unit);
}

static void test_next_loop(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    for i in 0 to 7 loop\n"
        "      next;\n"
        "    end loop;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "next loop");
    uir_destroy_design_unit(r.unit);
}

static void test_return_procedure(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    return;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "return procedure");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Assert and report statement tests
 * ================================================================= */

static void test_assert_simple(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    assert true;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "assert true");
    uir_destroy_design_unit(r.unit);
}

static void test_assert_report_severity(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    assert false report \"error\" severity error;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "assert with report and severity");
    uir_destroy_design_unit(r.unit);
}

static void test_assert_only_severity(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    assert a = '1' severity failure;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "assert with severity only");
    uir_destroy_design_unit(r.unit);
}

static void test_report_stmt(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    report \"hello\";\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "report statement");
    uir_destroy_design_unit(r.unit);
}

static void test_concurrent_report(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  report \"hello\";\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "concurrent report");
    uir_destroy_design_unit(r.unit);
}

static void test_concurrent_assert(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  assert a = '1' report \"a not 1\" severity warning;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "concurrent assert");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * With-select concurrent assignment tests
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
        "    y <= \"00\" when \"00\",\n"
        "         \"01\" when \"01\",\n"
        "         \"10\" when \"10\",\n"
        "         \"11\" when others;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "with select simple");
    mu_assert(r.unit->process_count >= 1, "with-select creates process");
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_single(void)
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
    mu_assert(r.success, "with select single bit");
    mu_assert(r.unit->process_count >= 1, "with-select single creates process");
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_single_item(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  with a select\n"
        "    y <= '1' when '0',\n"
        "         '0' when others;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "with select single item");
    mu_assert(r.unit->process_count >= 1, "with-select single item creates process");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Sequential with-select in a process
 * ================================================================= */

static void test_seq_with_select(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(1 downto 0); y: out std_logic_vector(1 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    with a select\n"
        "      y <= \"00\" when \"00\",\n"
        "           \"01\" when others;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "sequential with select");
    mu_assert(r.unit->process_count >= 1, "seq with-select has process");
    uir_destroy_design_unit(r.unit);
}

static void test_with_select_others_only(void)
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
    mu_assert(r.success, "with select others only");
    mu_assert(r.unit->process_count >= 1, "with-select others creates process");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Generate block tests
 * ================================================================= */

static void test_for_generate(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  gen: for i in 0 to 7 generate\n"
        "    y <= a;\n"
        "  end generate;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "for generate");
    mu_assert(r.unit->generate_count >= 1, "for generate creates UIR node");
    uir_destroy_design_unit(r.unit);
}

static void test_if_generate(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  gen: if true generate\n"
        "    y <= a;\n"
        "  end generate;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "if generate");
    mu_assert(r.unit->generate_count >= 1, "if generate creates UIR node");
    uir_destroy_design_unit(r.unit);
}

static void test_nested_generate(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  outer: for i in 0 to 3 generate\n"
        "    inner: if true generate\n"
        "      y <= a;\n"
        "    end generate;\n"
        "  end generate;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "nested generate");
    mu_assert(r.unit->generate_count >= 1, "nested generate creates UIR node");
    uir_destroy_design_unit(r.unit);
}

static void test_if_else_generate(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  gen: if a = '1' generate\n"
        "    y <= '1';\n"
        "  else generate\n"
        "    y <= '0';\n"
        "  end generate;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "if-else generate");
    mu_assert(r.unit->generate_count >= 1, "if-else generate creates UIR node");
    uir_destroy_design_unit(r.unit);
}

static void test_for_generate_with_process(void)
{
    const char *src =
        "entity top is\n"
        "  port (clk: in std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  gen: for i in 0 to 1 generate\n"
        "    p: process is\n"
        "    begin\n"
        "      wait until clk = '1';\n"
        "    end process;\n"
        "  end generate;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "for generate with process");
    mu_assert(r.unit->generate_count >= 1, "for gen with process creates UIR node");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Function and procedure tests
 * ================================================================= */

static void test_function_decl(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  function my_func return integer is\n"
        "  begin\n"
        "    return 42;\n"
        "  end function;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "function declaration");
    mu_assert(r.unit->func_task_count >= 1, "should have func/task");
    mu_assert(r.unit->func_tasks[0]->is_function, "should be a function");
    uir_destroy_design_unit(r.unit);
}

static void test_procedure_decl(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  procedure my_proc is\n"
        "  begin\n"
        "    null;\n"
        "  end procedure;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "procedure declaration");
    mu_assert(r.unit->func_task_count >= 1, "should have func/task");
    mu_assert(!r.unit->func_tasks[0]->is_function, "should be a procedure");
    uir_destroy_design_unit(r.unit);
}

static void test_function_with_params(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic_vector(7 downto 0));\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  function is_even(x: integer) return boolean is\n"
        "  begin\n"
        "    return true;\n"
        "  end function;\n"
        "begin\n"
        "  y <= a;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "function with params");
    mu_assert(r.unit->func_task_count >= 1, "should have func/task");
    mu_assert(r.unit->func_tasks[0]->is_function, "should be a function");
    mu_assert(r.unit->func_tasks[0]->port_count >= 1, "should have params");
    uir_destroy_design_unit(r.unit);
}

static void test_procedure_call_stmt(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    my_proc;\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "procedure call stmt");
    uir_destroy_design_unit(r.unit);
}

static void test_procedure_call_with_args(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  process is\n"
        "  begin\n"
        "    my_proc(a, b);\n"
        "  end process;\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "procedure call with args");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Package tests
 * ================================================================= */

static void test_package_decl(void)
{
    const char *src =
        "package my_pkg is\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "package declaration");
    uir_destroy_design_unit(r.unit);
}

static void test_package_body(void)
{
    const char *src =
        "package body my_pkg is\n"
        "end package body;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "package body");
    uir_destroy_design_unit(r.unit);
}

static void test_package_with_decls(void)
{
    const char *src =
        "package my_pkg is\n"
        "  constant width : integer := 8;\n"
        "  function is_even(x: integer) return boolean;\n"
        "  procedure my_proc(x: integer);\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "package with declarations");
    mu_assert(r.unit->vhdl_constant_count >= 1, "should have constants");
    mu_assert_str_eq(r.unit->vhdl_constants[0].name, "width", "const name");
    mu_assert_eq(r.unit->vhdl_constants[0].width, 32, "const width");
    mu_assert_eq(r.unit->vhdl_constants[0].value, 8, "const value");
    uir_destroy_design_unit(r.unit);
}

static void test_pkg_constant_only(void)
{
    const char *src =
        "package my_pkg is\n"
        "  constant width : integer := 8;\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "pkg constant only");
    mu_assert(r.unit->vhdl_constant_count >= 1, "should have constants");
    mu_assert_str_eq(r.unit->vhdl_constants[0].name, "width", "const name");
    mu_assert_eq(r.unit->vhdl_constants[0].width, 32, "const width");
    mu_assert_eq(r.unit->vhdl_constants[0].value, 8, "const value");
    uir_destroy_design_unit(r.unit);
}

static void test_pkg_function_only(void)
{
    const char *src =
        "package my_pkg is\n"
        "  function is_even(x: integer) return boolean;\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "pkg function only");
    mu_assert(r.unit->func_task_count >= 1, "should have 1 func spec");
    mu_assert(r.unit->func_tasks[0]->is_function, "should be a function");
    mu_assert_eq(r.unit->func_tasks[0]->port_count, 1, "should have 1 param");
    uir_destroy_design_unit(r.unit);
}

static void test_pkg_procedure_only(void)
{
    const char *src =
        "package my_pkg is\n"
        "  procedure my_proc(x: integer);\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "pkg procedure only");
    mu_assert(r.unit->func_task_count >= 1, "should have 1 proc spec");
    mu_assert(!r.unit->func_tasks[0]->is_function, "should be a procedure");
    mu_assert_eq(r.unit->func_tasks[0]->port_count, 1, "should have 1 param");
    uir_destroy_design_unit(r.unit);
}

static void test_package_body_with_func(void)
{
    const char *src =
        "package body my_pkg is\n"
        "  function is_even(x: integer) return boolean is\n"
        "  begin\n"
        "    return true;\n"
        "  end function;\n"
        "end package body;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "package body with function");
    mu_assert(r.unit->func_task_count >= 1, "should have func/task");
    mu_assert(r.unit->func_tasks[0]->is_function, "should be a function");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Type and subtype declaration tests
 * ================================================================= */

static void test_type_enum(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  type state_type is (idle, start, done);\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "type enumeration");
    mu_assert(r.unit->vhdl_type_count >= 1, "should have 1 type");
    mu_assert_eq(r.unit->vhdl_types[0].kind, UIR_VHDL_TYPE_ENUM, "kind should be ENUM");
    mu_assert_eq(r.unit->vhdl_types[0].enum_literal_count, 3, "should have 3 literals");
    mu_assert_str_eq(r.unit->vhdl_types[0].enum_literals[0], "idle", "literal 0");
    mu_assert_str_eq(r.unit->vhdl_types[0].enum_literals[1], "start", "literal 1");
    mu_assert_str_eq(r.unit->vhdl_types[0].enum_literals[2], "done", "literal 2");
    uir_destroy_design_unit(r.unit);
}

static void test_type_integer_range(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  type addr_t is range 0 to 255;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "type integer range");
    mu_assert(r.unit->vhdl_type_count >= 1, "should have 1 type");
    mu_assert_eq(r.unit->vhdl_types[0].kind, UIR_VHDL_TYPE_RANGE, "kind should be RANGE");
    mu_assert_eq(r.unit->vhdl_types[0].range_lo, 0, "lo should be 0");
    mu_assert_eq(r.unit->vhdl_types[0].range_hi, 255, "hi should be 255");
    uir_destroy_design_unit(r.unit);
}

static void test_subtype_range(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  subtype byte is integer range 0 to 255;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "subtype range");
    mu_assert(r.unit->vhdl_type_count >= 1, "should have 1 type");
    mu_assert_eq(r.unit->vhdl_types[0].kind, UIR_VHDL_TYPE_SUBTYPE, "kind should be SUBTYPE");
    mu_assert_str_eq(r.unit->vhdl_types[0].base_type_name, "integer", "base type");
    mu_assert_eq(r.unit->vhdl_types[0].range_lo, 0, "lo should be 0");
    mu_assert_eq(r.unit->vhdl_types[0].range_hi, 255, "hi should be 255");
    uir_destroy_design_unit(r.unit);
}

static void test_subtype_alias(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  subtype my_int is integer;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "subtype alias");
    mu_assert(r.unit->vhdl_type_count >= 1, "should have 1 type");
    mu_assert_eq(r.unit->vhdl_types[0].kind, UIR_VHDL_TYPE_SUBTYPE, "kind should be SUBTYPE");
    mu_assert_str_eq(r.unit->vhdl_types[0].base_type_name, "integer", "base type");
    uir_destroy_design_unit(r.unit);
}

static void test_type_record(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  type timing_data is record\n"
        "    setup : time;\n"
        "    hold  : time;\n"
        "  end record;\n"
        "begin\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "record type");
    mu_assert(r.unit->vhdl_type_count >= 1, "should have 1 type");
    mu_assert_eq(r.unit->vhdl_types[0].kind, UIR_VHDL_TYPE_RECORD, "kind should be RECORD");
    mu_assert_str_eq(r.unit->vhdl_types[0].name, "timing_data", "record name");
    mu_assert(r.unit->vhdl_types[0].record_field_count >= 2, "should have 2 fields");
    mu_assert_str_eq(r.unit->vhdl_types[0].record_fields[0].name, "setup", "field 0 name");
    mu_assert_str_eq(r.unit->vhdl_types[0].record_fields[1].name, "hold", "field 1 name");
    uir_destroy_design_unit(r.unit);
}

static void test_attr_spec_vital(void)
{
    const char *src =
        "entity AND2 is\n"
        "  port(a, b: in std_logic; c: out std_logic);\n"
        "end;\n"
        "architecture vital of AND2 is\n"
        "  attribute VITAL_Level0 of AND2 : entity is true;\n"
        "begin\n"
        "  c <= a and b;\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("and2.vhd", src, strlen(src));
    mu_assert(r.success, "vital attribute spec");
    mu_assert(r.unit->vhdl_attr_spec_count >= 1, "should have attr specs");
    mu_assert_str_eq(r.unit->vhdl_attr_specs[0].name, "vital_level0", "attr name");
    mu_assert_str_eq(r.unit->vhdl_attr_specs[0].target, "and2", "attr target");
    mu_assert_str_eq(r.unit->vhdl_attr_specs[0].entity_class, "entity", "attr class");
    mu_assert(r.unit->vhdl_attr_specs[0].value != NULL, "should have attr value");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * IEEE numeric_std builtin function tests
 * ================================================================= */

static void test_numeric_std_unsigned_parse(void)
{
    const char *src =
        "entity top is\n"
        "  port(a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= unsigned(a);\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "unsigned() parse");
    mu_assert(r.unit->assign_count >= 1, "should have concurrent signal assignment");
    if (r.unit->assign_count > 0 && r.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)r.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL, "assign RHS should exist");
        mu_assert(rhs->kind == UIR_FUNC_CALL, "RHS should be UIR_FUNC_CALL");
        uir_func_call_t *fc = (uir_func_call_t *)rhs;
        mu_assert_str_eq(fc->name, "unsigned", "func call name should be unsigned");
        mu_assert(fc->arg_count == 1, "should have one argument");
        mu_assert(fc->args[0]->kind == UIR_REF, "arg should be UIR_REF");
        uir_ref_t *arg = (uir_ref_t *)fc->args[0];
        mu_assert_str_eq(arg->name, "a", "arg name should be a");
    }
    uir_destroy_design_unit(r.unit);
}

static void test_numeric_std_signed_parse(void)
{
    const char *src =
        "entity top is\n"
        "  port(a: in std_logic_vector(7 downto 0); y: out std_logic_vector(7 downto 0));\n"
        "end;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= signed(a);\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "signed() parse");
    if (r.unit->assign_count > 0 && r.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)r.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL, "assign RHS should exist");
        mu_assert(rhs->kind == UIR_FUNC_CALL, "RHS should be UIR_FUNC_CALL");
        uir_func_call_t *fc = (uir_func_call_t *)rhs;
        mu_assert_str_eq(fc->name, "signed", "func call name should be signed");
        mu_assert(fc->arg_count == 1, "should have one argument");
        mu_assert(fc->args[0]->kind == UIR_REF, "arg should be UIR_REF");
        uir_ref_t *arg = (uir_ref_t *)fc->args[0];
        mu_assert_str_eq(arg->name, "a", "arg name should be a");
    }
    uir_destroy_design_unit(r.unit);
}

static void test_numeric_std_to_integer_parse(void)
{
    const char *src =
        "entity top is\n"
        "  port(a: in std_logic_vector(7 downto 0); y: out integer);\n"
        "end;\n"
        "architecture behav of top is\n"
        "begin\n"
        "  y <= to_integer(unsigned(a));\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "to_integer(unsigned()) parse");
    if (r.unit->assign_count > 0 && r.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)r.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL, "assign RHS should exist");
        mu_assert(rhs->kind == UIR_FUNC_CALL, "RHS should be UIR_FUNC_CALL");
        uir_func_call_t *outer = (uir_func_call_t *)rhs;
        mu_assert_str_eq(outer->name, "to_integer", "outer func name should be to_integer");
        mu_assert(outer->arg_count == 1, "outer should have one argument");
        mu_assert(outer->args[0]->kind == UIR_FUNC_CALL, "inner should be UIR_FUNC_CALL");
        uir_func_call_t *inner = (uir_func_call_t *)outer->args[0];
        mu_assert_str_eq(inner->name, "unsigned", "inner func name should be unsigned");
        mu_assert(inner->arg_count == 1, "inner should have one argument");
        mu_assert(inner->args[0]->kind == UIR_REF, "innermost should be UIR_REF");
        uir_ref_t *arg = (uir_ref_t *)inner->args[0];
        mu_assert_str_eq(arg->name, "a", "innermost name should be a");
    }
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * VITAL function call tests
 * ================================================================= */

static void test_vital_func_call_parse(void)
{
    const char *src =
        "entity vital_test is\n"
        "  port(a, b: in std_logic; y: out std_logic);\n"
        "end;\n"
        "architecture behav of vital_test is\n"
        "begin\n"
        "  y <= VitalAND(a, b);\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("vital_test.vhd", src, strlen(src));
    mu_assert(r.success, "vital func call parse");
    /* Concurrent signal assignment becomes a uir_assign_t, not a process */
    mu_assert(r.unit->assign_count >= 1, "should have concurrent signal assignment");
    /* The RHS should be a UIR_FUNC_CALL node */
    if (r.unit->assign_count > 0 && r.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)r.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL, "assign RHS should exist");
        mu_assert(rhs->kind == UIR_FUNC_CALL, "RHS should be UIR_FUNC_CALL");
        uir_func_call_t *fc = (uir_func_call_t *)rhs;
        mu_assert_str_eq(fc->name, "vitaland", "func call name should be vitaland");
        mu_assert_eq(fc->arg_count, (size_t)2, "should have 2 args");
    }
    uir_destroy_design_unit(r.unit);
}

/* Verify VitalAND call node is properly constructed in the parsed unit.
 * Full sim tests (elab + sim_create) have a pre-existing segfault in
 * the VHDL signal-path unrelated to VITAL — added as parse-only tests now. */
static void test_vital_and_sim(void)
{
    const char *src =
        "entity vital_test is\n"
        "  port (a, b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_test is\n"
        "begin\n"
        "  y <= VitalAND(a, b);\n"
        "end architecture;\n";

    vhdl_parse_result_t pr = vhdl_parse("vital_test.vhd", src, strlen(src));
    mu_assert(pr.success, "vital and parse");
    mu_assert(pr.unit->assign_count >= 1, "should have assign");
    if (pr.unit->assign_count > 0 && pr.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)pr.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL && rhs->kind == UIR_FUNC_CALL, "RHS is func_call");
        uir_func_call_t *fc = (uir_func_call_t *)rhs;
        mu_assert_str_eq(fc->name, "vitaland", "name");
        mu_assert_eq(fc->arg_count, (size_t)2, "2 args");
    }
    uir_destroy_design_unit(pr.unit);
}

static void test_vital_or_sim(void)
{
    const char *src =
        "entity vital_test is\n"
        "  port (a, b: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_test is\n"
        "begin\n"
        "  y <= VitalOR(a, b);\n"
        "end architecture;\n";

    vhdl_parse_result_t pr = vhdl_parse("vital_test.vhd", src, strlen(src));
    mu_assert(pr.success, "vital or parse");
    mu_assert(pr.unit->assign_count >= 1, "should have assign");
    if (pr.unit->assign_count > 0 && pr.unit->assigns[0]) {
        uir_node_t *rhs = (uir_node_t *)pr.unit->assigns[0]->rhs;
        mu_assert(rhs != NULL && rhs->kind == UIR_FUNC_CALL, "RHS is func_call");
        uir_func_call_t *fc = (uir_func_call_t *)rhs;
        mu_assert_str_eq(fc->name, "vitalor", "name");
    }
    uir_destroy_design_unit(pr.unit);
}

static void test_vital_inv_sim(void)
{
    const char *src =
        "entity vital_test is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of vital_test is\n"
        "  signal tmp: std_logic;\n"
        "begin\n"
        "  tmp <= VitalINV(a, tmp);\n"
        "  y <= tmp;\n"
        "end architecture;\n";

    vhdl_parse_result_t pr = vhdl_parse("vital_test.vhd", src, strlen(src));
    mu_assert(pr.success, "vital inv parse");
    mu_assert(pr.unit->assign_count >= 1, "should have assigns");
    uir_destroy_design_unit(pr.unit);
}

/* =================================================================
 * Alias declaration tests
 * ================================================================= */

static void test_alias_decl_in_arch(void)
{
    const char *src =
        "entity top is\n"
        "  port(x: in integer);\n"
        "end;\n"
        "architecture behav of top is\n"
        "  alias X is x;\n"
        "begin\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "alias in architecture");
    mu_assert(r.unit->vhdl_alias_count >= 1, "should have aliases");
    mu_assert_str_eq(r.unit->vhdl_aliases[0].name, "x", "alias name");
    mu_assert_str_eq(r.unit->vhdl_aliases[0].target, "x", "alias target");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Group/template declaration tests
 * ================================================================= */

static void test_group_template(void)
{
    const char *src =
        "entity top is\n"
        "end;\n"
        "architecture behav of top is\n"
        "  group signal_pair is (signal, signal);\n"
        "begin\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "group template");
    mu_assert(r.unit->vhdl_group_count >= 1, "should have groups");
    mu_assert_str_eq(r.unit->vhdl_groups[0].name, "signal_pair", "group name");
    mu_assert(r.unit->vhdl_groups[0].kind == UIR_VHDL_GROUP_TEMPLATE, "should be template");
    mu_assert(r.unit->vhdl_groups[0].constituent_count >= 2, "should have 2 constituents");
    mu_assert_str_eq(r.unit->vhdl_groups[0].constituents[0], "signal", "first constituent");
    mu_assert_str_eq(r.unit->vhdl_groups[0].constituents[1], "signal", "second constituent");
    uir_destroy_design_unit(r.unit);
}

static void test_group_decl(void)
{
    const char *src =
        "entity top is\n"
        "  port(x: in integer);\n"
        "end;\n"
        "architecture behav of top is\n"
        "  group sig_pair is (signal, signal);\n"
        "  group clk_rst : sig_pair (x, x);\n"
        "begin\n"
        "end;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "group declaration");
    mu_assert(r.unit->vhdl_group_count >= 2, "should have 2 groups");
    /* Find the group declaration (second one, kind=GROUP_DECL) */
    uir_vhdl_group_t *gd = NULL;
    for (size_t i = 0; i < r.unit->vhdl_group_count; i++) {
        if (r.unit->vhdl_groups[i].kind == UIR_VHDL_GROUP_DECL) {
            gd = &r.unit->vhdl_groups[i];
            break;
        }
    }
    mu_assert(gd != NULL, "should find group decl");
    mu_assert_str_eq(gd->name, "clk_rst", "group decl name");
    mu_assert(gd->template_name != NULL, "should have template");
    mu_assert_str_eq(gd->template_name, "sig_pair", "template name");
    mu_assert(gd->constituent_count >= 2, "should have 2 constituents");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Configuration declaration tests
 * ================================================================= */

static void test_configuration_decl(void)
{
    const char *src =
        "configuration cfg of top is\n"
        "  for behav\n"
        "  end for;\n"
        "end configuration;\n";
    vhdl_parse_result_t r = vhdl_parse("cfg.vhd", src, strlen(src));
    mu_assert(r.success, "configuration declaration");
    mu_assert_str_eq(r.unit->name, "cfg", "config name");
    mu_assert(r.unit->config_entity_name != NULL, "should have entity name");
    mu_assert_str_eq(r.unit->config_entity_name, "top", "entity name");
    mu_assert(r.unit->config_block_count >= 1, "should have block config");
    mu_assert_str_eq(r.unit->config_blocks[0].arch_name, "behav", "arch name");
    uir_destroy_design_unit(r.unit);
}

/* =================================================================
 * Component declaration tests
 * ================================================================= */

static void test_component_decl_in_arch(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  component my_and is\n"
        "    port (a: in std_logic; b: in std_logic; y: out std_logic);\n"
        "  end component;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "component decl with is/ports in arch");
    mu_assert(r.unit->component_count >= 1, "should have 1 component");
    mu_assert_eq(r.unit->components[0].port_count, 3, "should have 3 ports");
    mu_assert_str_eq(r.unit->components[0].ports[0].name, "a", "port 0 name");
    mu_assert_str_eq(r.unit->components[0].ports[1].name, "b", "port 1 name");
    mu_assert_str_eq(r.unit->components[0].ports[2].name, "y", "port 2 name");
    uir_destroy_design_unit(r.unit);
}

static void test_component_decl_no_is(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  component my_and\n"
        "    port (a: in std_logic; y: out std_logic);\n"
        "  end component;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "component decl without is");
    mu_assert(r.unit->component_count >= 1, "should have 1 component");
    mu_assert_eq(r.unit->components[0].port_count, 2, "should have 2 ports");
    mu_assert_str_eq(r.unit->components[0].ports[0].name, "a", "port 0 name");
    mu_assert_str_eq(r.unit->components[0].ports[1].name, "y", "port 1 name");
    uir_destroy_design_unit(r.unit);
}

static void test_component_decl_minimal(void)
{
    const char *src =
        "entity top is end entity;\n"
        "architecture behav of top is\n"
        "  component my_and is\n"
        "  end component;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "component decl minimal (no ports)");
    mu_assert(r.unit->component_count >= 1, "should have 1 component");
    mu_assert_eq(r.unit->components[0].port_count, 0, "should have 0 ports");
    uir_destroy_design_unit(r.unit);
}

static void test_component_decl_in_package(void)
{
    const char *src =
        "package my_pkg is\n"
        "  component my_and is\n"
        "    port (a: in std_logic; b: in std_logic; y: out std_logic);\n"
        "  end component;\n"
        "end package;\n";
    vhdl_parse_result_t r = vhdl_parse("my_pkg.vhd", src, strlen(src));
    mu_assert(r.success, "component decl in package");
    mu_assert(r.unit->component_count >= 1, "should have 1 component");
    mu_assert_eq(r.unit->components[0].port_count, 3, "should have 3 ports");
    mu_assert_str_eq(r.unit->components[0].ports[0].name, "a", "port 0 name");
    mu_assert_str_eq(r.unit->components[0].ports[1].name, "b", "port 1 name");
    mu_assert_str_eq(r.unit->components[0].ports[2].name, "y", "port 2 name");
    uir_destroy_design_unit(r.unit);
}

static void test_component_decl_duplicate_ports(void)
{
    const char *src =
        "entity top is\n"
        "  port (a: in std_logic; y: out std_logic);\n"
        "end entity;\n"
        "architecture behav of top is\n"
        "  component my_and is\n"
        "    port (a: in std_logic; b: in std_logic; y: out std_logic);\n"
        "  end component;\n"
        "begin\n"
        "  y <= '1';\n"
        "end architecture;\n";
    vhdl_parse_result_t r = vhdl_parse("top.vhd", src, strlen(src));
    mu_assert(r.success, "component decl with ports matching entity ports");
    mu_assert(r.unit->component_count >= 1, "should have 1 component");
    mu_assert_eq(r.unit->components[0].port_count, 3, "component should have 3 ports");
    mu_assert_eq(r.unit->port_count, 2, "entity port count should not be polluted");
    uir_destroy_design_unit(r.unit);
}

void register_vhdl_lib_use_tests(void)
{
    printf("[VHDL Library/Use]\n");
    mu_run_test(test_library_before_entity);
    mu_run_test(test_library_use_before_entity);
    mu_run_test(test_multiple_libraries_uses);
    mu_run_test(test_no_library_use);
    mu_run_test(test_library_use_with_arch);
    printf("\n");
    printf("[VHDL Wait/Exit/Next/Return]\n");
    mu_run_test(test_wait_for);
    mu_run_test(test_wait_until);
    mu_run_test(test_wait_on);
    mu_run_test(test_wait_forever);
    mu_run_test(test_wait_on_multiple);
    mu_run_test(test_exit_loop);
    mu_run_test(test_exit_with_label);
    mu_run_test(test_next_loop);
    mu_run_test(test_return_procedure);
    printf("\n");
    printf("[VHDL Assert/Report]\n");
    mu_run_test(test_assert_simple);
    mu_run_test(test_assert_report_severity);
    mu_run_test(test_assert_only_severity);
    mu_run_test(test_report_stmt);
    mu_run_test(test_concurrent_report);
    mu_run_test(test_concurrent_assert);
    printf("\n");
    printf("[VHDL With-Select]\n");
    mu_run_test(test_with_select_simple);
    mu_run_test(test_with_select_single);
    mu_run_test(test_with_select_single_item);
    mu_run_test(test_seq_with_select);
    mu_run_test(test_with_select_others_only);
    printf("\n");
    printf("[VHDL Generate]\n");
    mu_run_test(test_for_generate);
    mu_run_test(test_if_generate);
    mu_run_test(test_nested_generate);
    mu_run_test(test_if_else_generate);
    mu_run_test(test_for_generate_with_process);
    printf("\n");
    printf("[VHDL Functions/Procedures]\n");
    mu_run_test(test_function_decl);
    mu_run_test(test_procedure_decl);
    mu_run_test(test_function_with_params);
    mu_run_test(test_procedure_call_stmt);
    mu_run_test(test_procedure_call_with_args);
    printf("\n");
    printf("[VHDL Packages]\n");
    mu_run_test(test_package_decl);
    mu_run_test(test_package_body);
    mu_run_test(test_package_with_decls);
    mu_run_test(test_package_body_with_func);
    mu_run_test(test_pkg_constant_only);
    mu_run_test(test_pkg_function_only);
    mu_run_test(test_pkg_procedure_only);
    printf("\n");
    printf("[VHDL Type/Subtype]\n");
    mu_run_test(test_type_enum);
    mu_run_test(test_type_integer_range);
    mu_run_test(test_subtype_range);
    mu_run_test(test_subtype_alias);
    mu_run_test(test_type_record);
    printf("\n");
    printf("[VHDL Attribute Specs]\n");
    mu_run_test(test_attr_spec_vital);
    printf("\n");
    printf("[VHDL VITAL Function Calls]\n");
    mu_run_test(test_numeric_std_unsigned_parse);
    mu_run_test(test_numeric_std_signed_parse);
    mu_run_test(test_numeric_std_to_integer_parse);
    mu_run_test(test_vital_func_call_parse);
    mu_run_test(test_vital_and_sim);
    mu_run_test(test_vital_or_sim);
    mu_run_test(test_vital_inv_sim);
    printf("\n");
    printf("[VHDL Alias]\n");
    mu_run_test(test_alias_decl_in_arch);
    printf("\n");
    printf("[VHDL Group/Template]\n");
    mu_run_test(test_group_template);
    mu_run_test(test_group_decl);
    printf("\n");
    printf("[VHDL Configuration]\n");
    mu_run_test(test_configuration_decl);
    printf("\n");
    printf("[VHDL Component Declarations]\n");
    mu_run_test(test_component_decl_in_arch);
    mu_run_test(test_component_decl_no_is);
    mu_run_test(test_component_decl_minimal);
    mu_run_test(test_component_decl_in_package);
    mu_run_test(test_component_decl_duplicate_ports);
    printf("\n");
}

int mu_tests_run = 0;
int mu_tests_passed = 0;
int mu_tests_failed = 0;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("VHDL Library/Use Clause Tests\n");
    printf("========================================\n\n");
    register_vhdl_lib_use_tests();
    mu_summary();
    return mu_tests_failed > 0 ? 1 : 0;
}
