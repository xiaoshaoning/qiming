/* Qiming test runner — minimal framework, no external dependencies. */
#include "minunit.h"

extern void register_value_tests(void);
extern void register_uir_tests(void);
extern void register_scheduler_tests(void);
extern void register_wave_buffer_tests(void);
extern void register_logger_tests(void);
extern void register_simulator_tests(void);
extern void register_verilog_parser_tests(void);
extern void register_elaboration_tests(void);
extern void register_vhdl_parser_tests(void);
extern void register_vhdl_simulator_tests(void);
extern void register_vital_sim_tests(void);
extern void register_session_tests(void);
extern void register_rv32i_tests(void);
extern void register_rv32i_asm_tests(void);
extern void register_rv32i_hex_tests(void);
extern void register_rv32i_vhdl_tests(void);
extern void register_perf_tests(void);
extern void register_generate_tests(void);

/* Global test counters (extern in minunit.h) */
int mu_tests_run = 0;
int mu_tests_passed = 0;
int mu_tests_failed = 0;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Qiming Libdsim Test Suite\n");
    printf("========================================\n\n");

    register_value_tests();
    register_uir_tests();
    register_scheduler_tests();
    register_wave_buffer_tests();
    register_logger_tests();
    register_simulator_tests();
    register_verilog_parser_tests();
    register_elaboration_tests();
    register_generate_tests();
    register_vhdl_parser_tests();
    register_vhdl_simulator_tests();
    register_vital_sim_tests();
    register_session_tests();
    register_rv32i_tests();
    register_rv32i_asm_tests();
    register_rv32i_hex_tests();
    register_rv32i_vhdl_tests();
    register_perf_tests();

    mu_summary();

    return mu_tests_failed > 0 ? 1 : 0;
}
