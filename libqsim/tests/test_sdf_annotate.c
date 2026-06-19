/* Phase 4d — SDF back-annotation integration test */
#include "libqsim/sdf_parse.h"
#include "libqsim/simulator.h"
#include "libqsim/uir_sim.h"
#include "libqsim/elaboration.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests = 0, passed = 0;
#define CHECK(cond, msg) do { tests++; if (cond) { passed++; } else { printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while(0)

/* Create a temporary SDF file with the given content. Returns the filename. */
static char *write_temp_sdf(const char *content) {
    static int counter = 0;
    char buf[64];
    sprintf(buf, "_test_sdf_%d.sdf", counter++);
    char *path = strdup(buf);
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
    return path;
}

static void remove_file(const char *path) {
    if (path) remove(path);
}

/* Test 1: Parse a minimal SDF file with one CELL and one IOPATH */
static void test_parse_sdf(void) {
    const char *sdf_content =
        "(DELAYFILE\n"
        "  (TIMESCALE 1ns)\n"
        "  (DIVIDER .)\n"
        "  (CELL\n"
        "    (CELLTYPE (test))\n"
        "    (INSTANCE top.u1)\n"
        "    (DELAY\n"
        "      (ABSOLUTE\n"
        "        (IOPATH a b (3.0) (5.0))\n"
        "      )\n"
        "    )\n"
        "  )\n"
        ")\n";

    char *path = write_temp_sdf(sdf_content);
    CHECK(path != NULL, "temp sdf file");
    if (!path) return;

    sdf_file_t *sdf = sdf_parse_file(path);
    CHECK(sdf != NULL, "sdf parse");
    if (!sdf) { remove_file(path); free(path); return; }

    CHECK(sdf->cell_count >= 1, "cell count");
    if (sdf->cell_count >= 1) {
        CHECK(strcmp(sdf->cells[0].instance, "top.u1") == 0, "instance path");
        CHECK(sdf->cells[0].iopath_count >= 1, "iopath count");
        if (sdf->cells[0].iopath_count >= 1) {
            CHECK(strcmp(sdf->cells[0].iopaths[0].src_pin, "a") == 0, "iopath src");
            CHECK(strcmp(sdf->cells[0].iopaths[0].dst_pin, "b") == 0, "iopath dst");
            CHECK(sdf->cells[0].iopaths[0].rise_delay == 3000, "iopath rise (1ns=1000ps * 3.0)");
            CHECK(sdf->cells[0].iopaths[0].fall_delay == 5000, "iopath fall (1ns=1000ps * 5.0)");
        }
    }

    sdf_file_free(sdf);
    remove_file(path);
    free(path);
}

/* Test 2: Parse SDF with triple delay values (min:typ:max) */
static void test_parse_triple(void) {
    const char *sdf_content =
        "(DELAYFILE\n"
        "  (TIMESCALE 100ps)\n"
        "  (CELL\n"
        "    (CELLTYPE (buf))\n"
        "    (INSTANCE top)\n"
        "    (DELAY\n"
        "      (ABSOLUTE\n"
        "        (IOPATH in out (2.5:3.0:3.5) (4.0:5.0:6.0))\n"
        "      )\n"
        "    )\n"
        "  )\n"
        ")\n";

    char *path = write_temp_sdf(sdf_content);
    CHECK(path != NULL, "triple temp file");
    if (!path) return;

    sdf_file_t *sdf = sdf_parse_file(path);
    CHECK(sdf != NULL, "triple sdf parse");
    if (!sdf) { remove_file(path); free(path); return; }

    CHECK(sdf->cell_count >= 1, "triple cell count");
    if (sdf->cell_count >= 1 && sdf->cells[0].iopath_count >= 1) {
        /* TIMESCALE 100ps, so typ=3.0 -> 3.0 * 100 = 300 */
        CHECK(sdf->cells[0].iopaths[0].rise_delay == 300, "triple rise typ");
        /* typ=5.0 -> 5.0 * 100 = 500 */
        CHECK(sdf->cells[0].iopaths[0].fall_delay == 500, "triple fall typ");
    }

    sdf_file_free(sdf);
    remove_file(path);
    free(path);
}

/* Test 3: SDF annotation overrides path delays in simulation */
static void test_sdf_annotation(void) {
    const char *src =
        "module test;\n"
        "  input a;\n"
        "  output b;\n"
        "  specify\n"
        "    (a => b) = (100, 200);\n"
        "  endspecify\n"
        "  initial $sdf_annotate(\"_test_ann.sdf\");\n"
        "endmodule\n";

    const char *sdf_content =
        "(DELAYFILE\n"
        "  (TIMESCALE 1ns)\n"
        "  (CELL\n"
        "    (CELLTYPE (test))\n"
        "    (INSTANCE test)\n"
        "    (DELAY\n"
        "      (ABSOLUTE\n"
        "        (IOPATH a b (5) (7))\n"
        "      )\n"
        "    )\n"
        "  )\n"
        ")\n";

    /* Write the SDF file — must match the $sdf_annotate filename in the module */
    const char *sdf_filename = "_test_ann.sdf";
    FILE *sf = fopen(sdf_filename, "w");
    CHECK(sf != NULL, "ann open sdf");
    if (!sf) return;
    fputs(sdf_content, sf);
    fclose(sf);
    char *sdf_path = strdup(sdf_filename);

    qsim_compile_input_t input;
    memset(&input, 0, sizeof(input));
    input.sources = &src;
    input.source_count = 1;

    qsim_compile_result_t *cr = qsim_compile(&input);
    CHECK(cr && cr->success, "ann compile");
    if (!cr || !cr->success) { remove_file(sdf_path); free(sdf_path); return; }

    uir_design_unit_t **units = (uir_design_unit_t **)cr->units;
    uir_elab_result_t *elab = uir_elaborate(units, cr->unit_count);
    CHECK(elab && elab->success, "ann elab");
    if (!elab || !elab->success) { qsim_compile_result_free(cr); remove_file(sdf_path); free(sdf_path); return; }
    uir_elab_result_free(elab);

    uir_sim_context_t *sim = uir_sim_create(units, cr->unit_count);
    CHECK(sim != NULL, "ann sim create");
    if (!sim) { qsim_compile_result_free(cr); remove_file(sdf_path); free(sdf_path); return; }

    /* Default delays would be rise=100, fall=200.
     * After SDF annotation: rise=5ns=5000ps, fall=7ns=7000ps
     * (TIMESCALE 1ns = 1000ps conversion). */

    /* Drive a=1 (X→1 transition → rise delay=5ns=5000ps from SDF) */
    qsim_bit_vector_t *val_1 = qsim_bit_vector_alloc(1);
    qsim_bit_set(val_1, 0, QSIM_VAL_1);
    uir_sim_set_signal(sim, "a", val_1);
    qsim_bit_vector_free(val_1);

    /* Process time 0 events */
    uir_sim_run(sim, 0);

    /* b should still be X */
    qsim_bit_vector_t *bv = uir_sim_get_signal(sim, "b");
    CHECK(bv != NULL, "ann get b t=0");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "ann b still X at t=0");
    }

    /* Advance to just before rise (t=4999) — b should still be X */
    uir_sim_run(sim, 4999);
    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_X, "ann b still X at t=4999 (before SDF rise=5000)");
    }

    /* Advance to t=6000, past the SDF rise delay (5000) */
    uir_sim_run(sim, 6000);
    bv = uir_sim_get_signal(sim, "b");
    if (bv) {
        qsim_value_t b0 = qsim_bit_get(bv, 0);
        CHECK(b0.state == QSIM_1, "ann b is 1 after SDF rise delay");
    }

    uir_sim_destroy(sim);
    qsim_compile_result_free(cr);
    remove_file(sdf_path);
    free(sdf_path);
}

int main(void) {
    setbuf(stdout, NULL);
    test_parse_sdf();
    test_parse_triple();
    test_sdf_annotation();
    printf("SDF annotate: %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
