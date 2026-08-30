cmake_minimum_required(VERSION 3.20)
project(libqsim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Sources are UTF-8 (box-drawing/em-dash comment separators). Tell MSVC to
# read them as UTF-8 regardless of the system codepage (avoids C4819).
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Increase stack size for PEG recursive-descent parsing of large designs
if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")
elseif(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-stack_size,0x800000")
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,stack-size=8388608")
endif()

# Threading support (required for parallel delta evaluation)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# Find the peg parser generator
if(WIN32)
    find_program(PEG peg.exe HINTS "$ENV{HOME}/github/peg" "D:/github/peg")
else()
    find_program(PEG peg HINTS "$ENV{HOME}/go/bin" "/usr/local/bin" ENV PATH)
endif()

# Pre-generated parser fallback directory (committed to source tree)
set(PREGEN_PEG_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# Generate parser C code from PEG grammar when the tool is available;
# otherwise fall back to pre-generated files under ${PREGEN_PEG_DIR}.
# verilog_parser.c #includes grammar.peg.c directly (single translation unit)
set(VERILOG_PEG_SRC  src/verilog/grammar.peg)
set(VERILOG_PEG_OUT  ${CMAKE_CURRENT_BINARY_DIR}/generated/grammar.peg.c)

set(VHDL_PEG_SRC  src/vhdl/grammar.peg)
set(VHDL_PEG_OUT  ${CMAKE_CURRENT_BINARY_DIR}/generated/vhdl_grammar.peg.c)

if(PEG)
    add_custom_command(
        OUTPUT  ${VERILOG_PEG_OUT}
        DEPENDS ${VERILOG_PEG_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${PEG} -o ${VERILOG_PEG_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/${VERILOG_PEG_SRC}
        COMMENT "Generating Verilog parser from grammar.peg"
    )
    set_source_files_properties(${VERILOG_PEG_OUT} PROPERTIES GENERATED TRUE)

    add_custom_command(
        OUTPUT  ${VHDL_PEG_OUT}
        DEPENDS ${VHDL_PEG_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${PEG} -o ${VHDL_PEG_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/${VHDL_PEG_SRC}
        COMMENT "Generating VHDL parser from grammar.peg"
    )
    set_source_files_properties(${VHDL_PEG_OUT} PROPERTIES GENERATED TRUE)

    add_custom_target(gen_vparser DEPENDS ${VERILOG_PEG_OUT})
    add_custom_target(gen_vparser_vhdl DEPENDS ${VHDL_PEG_OUT})
else()
    message(STATUS "peg parser generator not found — using pre-generated files from ${PREGEN_PEG_DIR}")
    # Create no-op targets so add_dependencies(qsim ...) still works
    add_custom_target(gen_vparser)
    add_custom_target(gen_vparser_vhdl)
endif()

set(QSIM_SOURCES
    src/value.c
    src/uir.c
    src/scheduler.c
    src/sim_thread.c
    src/wave_buffer.c
    src/logger.c
    src/simulator.c
    src/elaboration.c
    src/uir_sim.c
    src/session.c
    src/sdf_parse.c
    src/verilog/verilog_parser.c
    src/verilog/verilog_preprocessor.c
    src/vhdl/vhdl_parser.c
    src/vhdl/vhdl_pkg_registry.c
    src/vhdl/vhdl_library.c
)

add_library(qsim STATIC ${QSIM_SOURCES})
target_include_directories(qsim PUBLIC include)
# Build-generated dir first (when PEG available), then pre-generated fallback
target_include_directories(qsim PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${PREGEN_PEG_DIR}
)

# Generate PEG parser before compiling qsim
add_dependencies(qsim gen_vparser gen_vparser_vhdl)

if(NOT WIN32)
    target_link_libraries(qsim PUBLIC Threads::Threads)
    # math functions (llround etc.) live in libm on Linux/macOS
    target_link_libraries(qsim PUBLIC m)
endif()

if(BUILD_TESTING)
    enable_testing()

    add_executable(qsim_test
        tests/main_test.c
        tests/test_value.c
        tests/test_uir.c
        tests/test_scheduler.c
        tests/test_wave_buffer.c
        tests/test_logger.c
        tests/test_simulator.c
        tests/test_verilog_parser.c
        tests/test_elaboration.c
        tests/test_vhdl_parser.c
        tests/test_vhdl_simulator.c
        tests/test_vital_sim.c
        tests/test_session.c
        tests/test_cpu_rv32i.c
        tests/test_cpu_rv32i_asm.c
        tests/test_cpu_rv32i_hex.c
        tests/test_cpu_rv32i_vhdl.c
        tests/test_perf.c
        tests/test_generate.c
    )
    target_include_directories(qsim_test PRIVATE tests)
    target_link_libraries(qsim_test PRIVATE qsim)
    add_test(NAME qsim_test COMMAND qsim_test)

    # Standalone suites (each has its own main with pass/fail exit codes).
    # Wired into ctest so CI actually executes them; previously they were
    # built but never run. Skipped: run_perf_tests (parallel engine bugs,
    # see docs/review_plan.md NOTE) and the assertion-free debug programs
    # (P2-13).
    add_executable(test_npu_bugs tests/test_npu_bugs.c)
    target_link_libraries(test_npu_bugs PRIVATE qsim)
    add_test(NAME test_npu_bugs COMMAND test_npu_bugs)

    add_executable(test_udp_sim tests/test_udp_sim.c)
    target_link_libraries(test_udp_sim PRIVATE qsim)
    add_test(NAME test_udp_sim COMMAND test_udp_sim)

    add_executable(test_part_select tests/test_part_select.c)
    target_link_libraries(test_part_select PRIVATE qsim)
    add_test(NAME test_part_select COMMAND test_part_select)

    add_executable(test_verilog_preprocessor tests/test_verilog_preprocessor.c)
    target_link_libraries(test_verilog_preprocessor PRIVATE qsim)
    add_test(NAME test_verilog_preprocessor COMMAND test_verilog_preprocessor)

    add_executable(test_net_types tests/test_net_types.c)
    target_include_directories(test_net_types PRIVATE tests)
    target_link_libraries(test_net_types PRIVATE qsim)
    add_test(NAME test_net_types COMMAND test_net_types)

    add_executable(test_specify_parse tests/test_specify_parse.c)
    target_link_libraries(test_specify_parse PRIVATE qsim)
    add_test(NAME test_specify_parse COMMAND test_specify_parse)

    add_executable(test_path_delay tests/test_path_delay.c)
    target_link_libraries(test_path_delay PRIVATE qsim)
    add_test(NAME test_path_delay COMMAND test_path_delay)

    add_executable(test_sdf_annotate tests/test_sdf_annotate.c)
    target_link_libraries(test_sdf_annotate PRIVATE qsim)
    add_test(NAME test_sdf_annotate COMMAND test_sdf_annotate)

    add_executable(test_timing_check tests/test_timing_check.c)
    target_include_directories(test_timing_check PRIVATE tests)
    target_link_libraries(test_timing_check PRIVATE qsim)
    add_test(NAME test_timing_check COMMAND test_timing_check)

    add_executable(test_vhdl_lib_use tests/test_vhdl_lib_use.c)
    target_include_directories(test_vhdl_lib_use PRIVATE tests)
    target_link_libraries(test_vhdl_lib_use PRIVATE qsim)
    add_test(NAME test_vhdl_lib_use COMMAND test_vhdl_lib_use)

    add_executable(test_idex_reg tests/test_idex_reg.c)
    target_include_directories(test_idex_reg PRIVATE tests)
    target_link_libraries(test_idex_reg PRIVATE qsim)
    add_test(NAME test_idex_reg COMMAND test_idex_reg)

    add_executable(test_combi_loop tests/test_combi_loop.c)
    target_include_directories(test_combi_loop PRIVATE tests)
    target_link_libraries(test_combi_loop PRIVATE qsim)
    add_test(NAME test_combi_loop COMMAND test_combi_loop)

    add_executable(test_library_path tests/test_library_path.c)
    target_link_libraries(test_library_path PRIVATE qsim)
    add_test(NAME test_library_path COMMAND test_library_path)

    # Regression suites salvaged from debug/repro scratch (P2-13). Each has
    # real assertions and a proper pass/fail exit code.
    add_executable(test_bugs_from_report tests/test_bugs_from_report.c)
    target_include_directories(test_bugs_from_report PRIVATE tests)
    target_link_libraries(test_bugs_from_report PRIVATE qsim)
    add_test(NAME test_bugs_from_report COMMAND test_bugs_from_report)

    add_executable(test_loop_blocks tests/test_loop_blocks.c)
    target_include_directories(test_loop_blocks PRIVATE tests)
    target_link_libraries(test_loop_blocks PRIVATE qsim)
    add_test(NAME test_loop_blocks COMMAND test_loop_blocks)

    add_executable(test_array_ports tests/test_array_ports.c)
    target_include_directories(test_array_ports PRIVATE tests)
    target_link_libraries(test_array_ports PRIVATE qsim)
    add_test(NAME test_array_ports COMMAND test_array_ports)

    add_executable(test_bug12_diag tests/test_bug12_diag.c)
    target_include_directories(test_bug12_diag PRIVATE tests)
    target_link_libraries(test_bug12_diag PRIVATE qsim)
    add_test(NAME test_bug12_diag COMMAND test_bug12_diag)

    add_executable(test_implicit_wire tests/test_implicit_wire.c)
    target_include_directories(test_implicit_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(test_implicit_wire PRIVATE qsim)
    add_test(NAME test_implicit_wire COMMAND test_implicit_wire)

    add_executable(test_replicate_cpu tests/test_replicate_cpu.c)
    target_link_libraries(test_replicate_cpu PRIVATE qsim)
    add_test(NAME test_replicate_cpu COMMAND test_replicate_cpu)

    add_executable(test_replicate_twoblk tests/test_replicate_twoblk.c)
    target_link_libraries(test_replicate_twoblk PRIVATE qsim)
    add_test(NAME test_replicate_twoblk COMMAND test_replicate_twoblk)

    # Benchmark harness: multi-threaded delta evaluation + bit-identical
    # cross-thread comparisons (parallel engine regression coverage).
    add_executable(run_perf_tests tests/run_perf_tests.c)
    target_link_libraries(run_perf_tests PRIVATE qsim)
    add_test(NAME run_perf_tests COMMAND run_perf_tests)

endif()
