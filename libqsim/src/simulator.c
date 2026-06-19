#include "libqsim/simulator.h"
#include "libqsim/uir.h"
#include "libqsim/verilog_parser.h"
#include "libqsim/vhdl_parser.h"
#include "libqsim/elaboration.h"
#include "libqsim/uir_sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

void qsim_init(void)
{
    /* Phase 1.1: no global state to initialize yet */
}

void qsim_shutdown(void)
{
    /* Phase 1.1: no global state to tear down yet */
}

/* ── Language detection helpers ── */

static int is_vhdl_extension(const char *filename) {
    if (!filename) return 0;
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return (strcmp(dot, ".vhd") == 0) || (strcmp(dot, ".vhdl") == 0);
}

static const char *skip_utf8_bom(const char *s, size_t *len) {
    if (s && *len >= 3 && (unsigned char)s[0] == 0xEF &&
                           (unsigned char)s[1] == 0xBB &&
                           (unsigned char)s[2] == 0xBF) {
        if (len) *len -= 3;
        return s + 3;
    }
    return s;
}

static int is_vhdl_source(const char *src) {
    if (!src) return 0;
    /* Skip UTF-8 BOM and leading whitespace */
    size_t dummy;
    src = skip_utf8_bom(src, &dummy);
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
        src++;
    return (strncmp(src, "entity", 6) == 0) || (strncmp(src, "architecture", 12) == 0);
}

/* ── Levenshtein distance for name suggestions ── */

static size_t lev_distance(const char *s, const char *t) {
    size_t n = strlen(s);
    size_t m = strlen(t);
    /* Stack-allocate distance matrix for short identifiers */
    size_t d[256][256];
    if (n > 255) n = 255;
    if (m > 255) m = 255;

    for (size_t i = 0; i <= n; i++) d[i][0] = i;
    for (size_t j = 0; j <= m; j++) d[0][j] = j;

    for (size_t j = 1; j <= m; j++) {
        for (size_t i = 1; i <= n; i++) {
            size_t cost = (s[i-1] == t[j-1]) ? 0 : 1;
            size_t best = d[i-1][j] + 1; /* deletion */
            size_t v = d[i][j-1] + 1;    /* insertion */
            if (v < best) best = v;
            v = d[i-1][j-1] + cost;      /* substitution */
            if (v < best) best = v;
            d[i][j] = best;
        }
    }
    return d[n][m];
}

qsim_recovery_t *qsim_recovery_from_names(const char *bad_name,
                                           const char **valid_names,
                                           size_t valid_count,
                                           const char *next_tool)
{
    if (!bad_name || !valid_names || valid_count == 0) return NULL;

    /* Compute distances */
    size_t *dists = malloc(valid_count * sizeof(size_t));
    if (!dists) return NULL;

    for (size_t i = 0; i < valid_count; i++)
        dists[i] = valid_names[i] ? lev_distance(bad_name, valid_names[i]) : SIZE_MAX;

    /* Find top 3 closest with distance in (0, 5) — exact match is not an error */
    size_t best_idx[3] = {0, 0, 0};
    size_t best_dist[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};

    for (size_t i = 0; i < valid_count; i++) {
        if (dists[i] == 0 || dists[i] >= 5) continue;
        for (int k = 0; k < 3; k++) {
            if (dists[i] < best_dist[k]) {
                for (int k2 = 2; k2 > k; k2--) {
                    best_idx[k2] = best_idx[k2-1];
                    best_dist[k2] = best_dist[k2-1];
                }
                best_idx[k] = i;
                best_dist[k] = dists[i];
                break;
            }
        }
    }

    size_t n_suggestions = 0;
    for (int k = 0; k < 3; k++)
        if (best_dist[k] < 5) n_suggestions++;

    free(dists);

    if (n_suggestions == 0) return NULL;

    qsim_recovery_t *rec = calloc(1, sizeof(qsim_recovery_t));
    if (!rec) return NULL;

    rec->suggestions = calloc(n_suggestions, sizeof(char *));
    if (!rec->suggestions) { free(rec); return NULL; }

    for (size_t k = 0; k < n_suggestions; k++)
        rec->suggestions[k] = strdup(valid_names[best_idx[k]]);
    rec->suggestion_count = n_suggestions;
    rec->next_tool = next_tool;

    return rec;
}

/* ── Library path search helpers (for -y style module discovery) ── */

static int is_module_defined(uir_design_unit_t **units, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (units[i] && units[i]->name && strcmp(units[i]->name, name) == 0)
            return 1;
    return 0;
}

/* Collect instantiated-but-not-defined module names. Returns NULL-terminated
   array of strdup'd strings; caller must free each and the array. */
static char **collect_missing_modules(uir_design_unit_t **units, size_t count) {
    size_t cap = 16, n = 0;
    char **missing = calloc(cap, sizeof(char *));
    if (!missing) return NULL;

    for (size_t i = 0; i < count; i++) {
        uir_design_unit_t *u = units[i];
        if (!u) continue;
        for (size_t j = 0; j < u->instance_count; j++) {
            uir_instance_t *inst = u->instances[j];
            if (!inst || !inst->module_name) continue;
            if (inst->bound_to) continue;              /* already resolved */
            if (is_module_defined(units, count, inst->module_name)) continue;

            /* Deduplicate */
            int already = 0;
            for (size_t k = 0; k < n; k++)
                if (strcmp(missing[k], inst->module_name) == 0) { already = 1; break; }
            if (already) continue;

            if (n + 1 >= cap) {
                cap *= 2;
                char **nm = realloc(missing, cap * sizeof(char *));
                if (!nm) { for (size_t k = 0; k < n; k++) free(missing[k]); free(missing); return NULL; }
                missing = nm;
            }
            missing[n++] = strdup(inst->module_name);
            missing[n] = NULL;
        }
    }
    return missing;
}

/* Search a library directory for a file matching module_name.
   Tries .v and .vhd extensions. Returns strdup'd full path or NULL. */
static char *search_library_for_module(const char *lib_path, const char *module_name) {
    const char *extensions[] = {".v", ".vhd"};
    char path[1024];

    for (size_t e = 0; e < 2; e++) {
        int len = snprintf(path, sizeof(path), "%s/%s%s", lib_path, module_name, extensions[e]);
        if (len < 0 || (size_t)len >= sizeof(path)) continue;
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); return strdup(path); }
    }
    return NULL;
}

qsim_compile_result_t *qsim_compile(const qsim_compile_input_t *input)
{
    qsim_compile_result_t *result = calloc(1, sizeof(qsim_compile_result_t));
    if (!result) return NULL;

    result->success = 1;
    result->units = NULL;
    result->unit_count = 0;

    size_t max_diag = (input ? input->file_count + input->source_count : 0) + 1;
    result->diagnostics = calloc(max_diag, sizeof(qsim_diagnostic_t));
    result->diag_count = 0;

    if (!input || (input->file_count == 0 && input->source_count == 0)) {
        qsim_diagnostic_t *d = &result->diagnostics[result->diag_count++];
        d->is_error = 0;
        d->file = NULL;
        d->line = 0;
        d->column = 0;
        d->message = strdup("no input files or sources provided");
        return result;
    }

    /* Process each file */
    for (size_t i = 0; i < input->file_count; i++) {
        const char *filename = input->files[i];
        if (!filename) continue;

        qsim_diagnostic_t *d = &result->diagnostics[result->diag_count];
        d->file = strdup(filename);

        if (is_vhdl_extension(filename)) {
            vhdl_parse_result_t pr = vhdl_parse_file(filename);

            if (pr.success && pr.unit) {
                d->is_error = 0;
                d->line = 0;
                d->column = 0;
                d->message = strdup("file parsed successfully");

                void **new_units = realloc(result->units,
                                            sizeof(void *) * (result->unit_count + 1));
                if (!new_units) {
                    uir_destroy_design_unit(pr.unit);
                    result->success = 0;
                    d->is_error = 1;
                    free((void *)d->message);
                    d->message = strdup("out of memory adding design unit");
                    result->diag_count++;
                    continue;
                }
                result->units = new_units;
                result->units[result->unit_count++] = (void *)pr.unit;
            } else if (pr.error_count > 0) {
                result->success = 0;
                d->is_error = 1;
                d->line = pr.errors[0].line;
                d->column = pr.errors[0].column;
                d->message = strdup(pr.errors[0].message);
            } else {
                result->success = 0;
                d->is_error = 1;
                d->line = 0;
                d->column = 0;
                char msg[512];
                snprintf(msg, sizeof(msg), "cannot open file: %s", strerror(errno));
                d->message = strdup(msg);
            }
        } else {
            parse_result_t pr = verilog_parse_file(filename);

            if (pr.success && pr.unit_count > 0) {
                d->is_error = 0;
                d->line = 0;
                d->column = 0;
                d->message = strdup("file parsed successfully");

                for (size_t j = 0; j < pr.unit_count; j++) {
                    void **new_units = realloc(result->units,
                                                sizeof(void *) * (result->unit_count + 1));
                    if (!new_units) {
                        for (size_t k = j; k < pr.unit_count; k++)
                            uir_destroy_design_unit(pr.units[k]);
                        result->success = 0;
                        d->is_error = 1;
                        free((void *)d->message);
                        d->message = strdup("out of memory adding design unit");
                        break;
                    }
                    result->units = new_units;
                    result->units[result->unit_count++] = (void *)pr.units[j];
                }
                if (d->is_error) { result->diag_count++; continue; }
            } else if (pr.error_count > 0) {
                result->success = 0;
                d->is_error = 1;
                d->line = pr.errors[0].line;
                d->column = pr.errors[0].column;
                d->message = strdup(pr.errors[0].message);
            } else {
                result->success = 0;
                d->is_error = 1;
                d->line = 0;
                d->column = 0;
                char msg[512];
                snprintf(msg, sizeof(msg), "cannot open file: %s", strerror(errno));
                d->message = strdup(msg);
            }
        }
        result->diag_count++;
    }

    /* Process inline sources */
    for (size_t i = 0; i < input->source_count; i++) {
        const char *src = input->sources[i];
        if (!src) continue;

        qsim_diagnostic_t *d = &result->diagnostics[result->diag_count];
        d->file = strdup("<inline>");

        /* Strip UTF-8 BOM if present */
        size_t src_len = strlen(src);
        src = skip_utf8_bom(src, &src_len);

        if (is_vhdl_source(src)) {
            vhdl_parse_result_t vpr = vhdl_parse("<inline>", src, src_len);

            if (vpr.success && vpr.unit) {
                d->is_error = 0;
                d->line = 0;
                d->column = 0;
                d->message = strdup("inline source parsed successfully");

                void **new_units = realloc(result->units,
                                            sizeof(void *) * (result->unit_count + 1));
                if (!new_units) {
                    uir_destroy_design_unit(vpr.unit);
                    result->success = 0;
                    d->is_error = 1;
                    free((void *)d->message);
                    d->message = strdup("out of memory adding design unit");
                    result->diag_count++;
                    continue;
                }
                result->units = new_units;
                result->units[result->unit_count++] = (void *)vpr.unit;
            } else {
                result->success = 0;
                d->is_error = 1;
                d->line = vpr.error_count > 0 ? vpr.errors[0].line : 0;
                d->column = vpr.error_count > 0 ? vpr.errors[0].column : 0;
                d->message = vpr.error_count > 0
                    ? strdup(vpr.errors[0].message)
                    : strdup("parse failed: empty or invalid source");
            }
        } else {
            parse_result_t pr = verilog_parse("<inline>", src, src_len);

            if (pr.success && pr.unit_count > 0) {
                d->is_error = 0;
                d->line = 0;
                d->column = 0;
                d->message = strdup("inline source parsed successfully");

                for (size_t j = 0; j < pr.unit_count; j++) {
                    void **new_units = realloc(result->units,
                                                sizeof(void *) * (result->unit_count + 1));
                    if (!new_units) {
                        for (size_t k = j; k < pr.unit_count; k++)
                            uir_destroy_design_unit(pr.units[k]);
                        result->success = 0;
                        d->is_error = 1;
                        free((void *)d->message);
                        d->message = strdup("out of memory adding design unit");
                        break;
                    }
                    result->units = new_units;
                    result->units[result->unit_count++] = (void *)pr.units[j];
                }
                if (d->is_error) { result->diag_count++; continue; }
            } else {
                result->success = 0;
                d->is_error = 1;
                d->line = pr.error_count > 0 ? pr.errors[0].line : 0;
                d->column = pr.error_count > 0 ? pr.errors[0].column : 0;
                d->message = pr.error_count > 0
                    ? strdup(pr.errors[0].message)
                    : strdup("parse failed: empty or invalid source");
            }
        }
        result->diag_count++;
    }

    /* ── Library path search for undefined modules ( -y style ) ── */
    if (result->success && result->unit_count > 0 &&
        input->library_path_count > 0 && input->library_paths) {
        int added;
        do {
            added = 0;
            char **missing = collect_missing_modules(
                (uir_design_unit_t **)result->units, result->unit_count);
            if (!missing) break;

            for (size_t m = 0; missing[m]; m++) {
                char *found = NULL;
                size_t found_lp = 0;
                for (size_t lp = 0; lp < input->library_path_count; lp++) {
                    if (!input->library_paths[lp]) continue;
                    found = search_library_for_module(input->library_paths[lp], missing[m]);
                    if (found) { found_lp = lp; break; }
                }
                if (!found) {
                    free(missing[m]);
                    continue;
                }

                parse_result_t pr = verilog_parse_file(found);
                free(found);

                if (pr.success && pr.unit_count > 0) {
                    /* Extend diagnostics array */
                    qsim_diagnostic_t *nd = realloc(result->diagnostics,
                        (result->diag_count + 1) * sizeof(qsim_diagnostic_t));
                    if (!nd) {
                        for (size_t j = 0; j < pr.unit_count; j++)
                            uir_destroy_design_unit(pr.units[j]);
                        free(pr.units);
                        free(missing[m]);
                        continue;
                    }
                    result->diagnostics = nd;

                    qsim_diagnostic_t *d = &result->diagnostics[result->diag_count];
                    d->file = strdup(found ? found : missing[m]);
                    d->is_error = 0;
                    d->line = 0;
                    d->column = 0;
                    {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "library '%s': loaded module '%s'",
                                 input->library_paths[found_lp], missing[m]);
                        d->message = strdup(msg);
                    }
                    d->recovery = NULL;
                    result->diag_count++;

                    for (size_t j = 0; j < pr.unit_count; j++) {
                        void **nu = realloc(result->units,
                            sizeof(void *) * (result->unit_count + 1));
                        if (!nu) {
                            uir_destroy_design_unit(pr.units[j]);
                            continue;
                        }
                        result->units = nu;
                        result->units[result->unit_count++] = (void *)pr.units[j];
                    }
                    added = 1;
                }
                /* else: parse error — non-fatal, log nothing */
                free(missing[m]);
            }
            free(missing);
        } while (added);
    }

    return result;
}

void qsim_compile_result_free(qsim_compile_result_t *result)
{
    if (!result) return;

    for (size_t i = 0; i < result->diag_count; i++) {
        free((void *)result->diagnostics[i].file);
        free((void *)result->diagnostics[i].message);
        if (result->diagnostics[i].recovery) {
            for (size_t j = 0; j < result->diagnostics[i].recovery->suggestion_count; j++)
                free(result->diagnostics[i].recovery->suggestions[j]);
            free(result->diagnostics[i].recovery->suggestions);
            for (size_t j = 0; j < result->diagnostics[i].recovery->nearby_count; j++)
                free(result->diagnostics[i].recovery->nearby[j]);
            free(result->diagnostics[i].recovery->nearby);
            free(result->diagnostics[i].recovery);
        }
    }
    free(result->diagnostics);

    for (size_t i = 0; i < result->unit_count; i++) {
        uir_destroy_design_unit((uir_design_unit_t *)result->units[i]);
    }
    free(result->units);
    free(result);
}

qsim_sim_result_t *qsim_sim_run(void *session, uint64_t until_time)
{
    if (!session) return NULL;

    qsim_compile_result_t *compile_res = (qsim_compile_result_t *)session;

    qsim_sim_result_t *result = calloc(1, sizeof(qsim_sim_result_t));
    if (!result) return NULL;

    if (compile_res->unit_count == 0 || !compile_res->units) {
        result->success = 0;
        result->ended_at = 0;
        result->final_delta = 0;
        result->stop_reason = strdup("no design units to simulate");
        return result;
    }

    /* Build array of design unit pointers */
    uir_design_unit_t **units = malloc(compile_res->unit_count * sizeof(uir_design_unit_t *));
    if (!units) {
        result->success = 0;
        result->stop_reason = strdup("out of memory");
        return result;
    }
    for (size_t i = 0; i < compile_res->unit_count; i++)
        units[i] = (uir_design_unit_t *)compile_res->units[i];

    /* Elaborate */
    uir_elab_result_t *elab = uir_elaborate(units, compile_res->unit_count);
    if (!elab || !elab->success) {
        result->success = 0;
        result->stop_reason = strdup(elab && elab->diag_count > 0
            ? elab->diagnostics[0] : "elaboration failed");
        uir_elab_result_free(elab);
        free(units);
        return result;
    }
    uir_elab_result_free(elab);

    /* Create simulation context */
    uir_sim_context_t *sim = uir_sim_create(units, compile_res->unit_count);
    if (!sim) {
        result->success = 0;
        result->stop_reason = strdup("failed to create simulation context");
        free(units);
        return result;
    }

    /* Run simulation */
    uir_sim_run(sim, until_time);

    result->success = 1;
    result->ended_at = uir_sim_current_time(sim);
    result->final_delta = 0;
    result->stop_reason = strdup("simulation completed");

    uir_sim_destroy(sim);
    free(units);
    return result;
}

void qsim_sim_result_free(qsim_sim_result_t *result)
{
    if (!result) return;
    free((void *)result->stop_reason);
    free(result);
}
