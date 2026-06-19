#include "libqsim/verilog_preprocessor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

/* ── Constants ── */

#define MAX_MACROS 2048
#define MAX_INCLUDE_PATHS 128
#define MAX_COND_STACK 64
#define MAX_RECURSION_DEPTH 64
#define OUTPUT_INIT_CAP 65536

/* ── Internal structures ── */

typedef struct {
    char *name;
    char *value;
} macro_def_t;

struct verilog_preprocessor {
    macro_def_t macros[MAX_MACROS];
    int macro_count;

    char *include_paths[MAX_INCLUDE_PATHS];
    int include_path_count;

    char *output;
    size_t output_len;
    size_t output_cap;

    char error_msg[512];
    int has_error;

    int include_depth;
};

/* ── Portable strndup ── */

static char *pp_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static char *pp_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len + 1);
    return d;
}

/* ── Forward declarations ── */

static void pp_emit(verilog_preprocessor_t *pp, const char *s, size_t len);
static void pp_emit_str(verilog_preprocessor_t *pp, const char *s);
static void pp_emit_char(verilog_preprocessor_t *pp, char c);
static void pp_set_error(verilog_preprocessor_t *pp, const char *fmt, ...);
static void pp_process_source(verilog_preprocessor_t *pp,
                               const char *source, size_t length,
                               const char *current_dir);
static void pp_substitute_macros(verilog_preprocessor_t *pp);

/* ── Output buffer ── */

static void pp_ensure_cap(verilog_preprocessor_t *pp, size_t extra)
{
    if (pp->output_len + extra + 1 > pp->output_cap) {
        size_t new_cap = pp->output_cap * 2;
        if (new_cap < pp->output_len + extra + 1)
            new_cap = pp->output_len + extra + 1;
        char *nb = realloc(pp->output, new_cap);
        if (!nb) { pp_set_error(pp, "out of memory"); return; }
        pp->output = nb;
        pp->output_cap = new_cap;
    }
}

static void pp_emit(verilog_preprocessor_t *pp, const char *s, size_t len)
{
    if (pp->has_error || len == 0) return;
    pp_ensure_cap(pp, len);
    if (pp->has_error) return;
    memcpy(pp->output + pp->output_len, s, len);
    pp->output_len += len;
}

static void pp_emit_str(verilog_preprocessor_t *pp, const char *s)
{
    pp_emit(pp, s, strlen(s));
}

static void pp_emit_char(verilog_preprocessor_t *pp, char c)
{
    pp_emit(pp, &c, 1);
}

/* ── Error handling ── */

static void pp_set_error(verilog_preprocessor_t *pp, const char *fmt, ...)
{
    if (pp->has_error) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(pp->error_msg, sizeof(pp->error_msg), fmt, args);
    va_end(args);
    pp->has_error = 1;
}

/* ── Whitespace/comments helpers ── */

/* Skip whitespace (spaces, tabs, \r). Stops at \n or end. */
static void skip_ws(char **ppos, const char *end)
{
    while (*ppos < end) {
        char c = **ppos;
        if (c == ' ' || c == '\t' || c == '\r')
            (*ppos)++;
        else
            break;
    }
}

/* Skip whitespace AND /*block comments* / (but not // line comments). */
static void skip_ws_block_comments(char **ppos, const char *end)
{
    while (*ppos < end) {
        char c = **ppos;
        if (c == ' ' || c == '\t' || c == '\r')
            (*ppos)++;
        else if (*ppos + 1 < end && c == '/' && (*ppos)[1] == '*') {
            *ppos += 2;
            while (*ppos + 1 < end) {
                if ((*ppos)[0] == '*' && (*ppos)[1] == '/') { *ppos += 2; break; }
                (*ppos)++;
            }
        } else
            break;
    }
}

/* Read an identifier token at *ppos. Advances *ppos past it.
 * Returns malloc'd string, or NULL on failure / not an identifier. */
static char *read_identifier(char **ppos, const char *end)
{
    skip_ws_block_comments(ppos, end);
    if (*ppos >= end) return NULL;
    char c = **ppos;
    if (!isalpha(c) && c != '_' && c != '$')
        return NULL;
    const char *start = *ppos;
    (*ppos)++;
    while (*ppos < end) {
        c = **ppos;
        if (isalnum(c) || c == '_' || c == '$')
            (*ppos)++;
        else
            break;
    }
    return pp_strndup(start, (size_t)(*ppos - start));
}

/* ── Macro table ── */

static int pp_find_macro(verilog_preprocessor_t *pp, const char *name)
{
    for (int i = 0; i < pp->macro_count; i++)
        if (strcmp(pp->macros[i].name, name) == 0)
            return i;
    return -1;
}

static int pp_is_defined(verilog_preprocessor_t *pp, const char *name)
{
    return pp_find_macro(pp, name) >= 0;
}

/* Handle `define line (text after "`define "). */
static int pp_define(verilog_preprocessor_t *pp, const char *def_text, size_t text_len)
{
    const char *end = def_text + text_len;
    char *p = (char *)def_text;

    char *name = read_identifier(&p, end);
    if (!name) {
        pp_set_error(pp, "`define: expected macro name");
        return 0;
    }

    /* Rest is value (strip leading whitespace) */
    skip_ws(&p, end);
    const char *val_start = p;
    size_t val_len = (size_t)(end - val_start);
    /* Strip trailing whitespace */
    while (val_len > 0 && (val_start[val_len-1] == ' ' || val_start[val_len-1] == '\t'))
        val_len--;

    char *value = (val_len > 0) ? pp_strndup(val_start, val_len) : pp_strdup("");

    /* Overwrite if redefined */
    int idx = pp_find_macro(pp, name);
    if (idx >= 0) {
        free(pp->macros[idx].value);
        pp->macros[idx].value = value;
        free(name);
        return 1;
    }

    if (pp->macro_count >= MAX_MACROS) {
        pp_set_error(pp, "`define: too many macros (max %d)", MAX_MACROS);
        free(name);
        free(value);
        return 0;
    }

    idx = pp->macro_count++;
    pp->macros[idx].name = name;
    pp->macros[idx].value = value;
    return 1;
}

/* Handle `undef. */
static int pp_undef(verilog_preprocessor_t *pp, const char *ud_text, size_t text_len)
{
    const char *end = ud_text + text_len;
    char *p = (char *)ud_text;
    char *name = read_identifier(&p, end);
    if (!name) {
        pp_set_error(pp, "`undef: expected macro name");
        return 0;
    }
    int idx = pp_find_macro(pp, name);
    if (idx >= 0) {
        free(pp->macros[idx].name);
        free(pp->macros[idx].value);
        for (int i = idx; i < pp->macro_count - 1; i++)
            pp->macros[i] = pp->macros[i+1];
        pp->macro_count--;
    }
    free(name);
    return 1;
}

/* Expand a macro with recursive substitution up to a depth limit.
 * Returns malloc'd string (caller must free) or NULL on failure/empty. */
static char *expand_macro(verilog_preprocessor_t *pp, const char *name, int depth)
{
    if (depth > MAX_RECURSION_DEPTH) return NULL;
    int idx = pp_find_macro(pp, name);
    if (idx < 0) return NULL;

    const char *value = pp->macros[idx].value;
    if (!value || !value[0]) return NULL;

    /* Check if value contains references to other macros (starting with `) */
    const char *vp = value;
    while (*vp) {
        if (*vp == '`') {
            const char *mn = vp + 1;
            if (isalpha(*mn) || *mn == '_') {
                const char *mn_end = mn;
                while (isalnum(*mn_end) || *mn_end == '_' || *mn_end == '$')
                    mn_end++;
                size_t mn_len = (size_t)(mn_end - mn);
                char *nested = pp_strndup(mn, mn_len);
                if (nested) {
                    char *ev = expand_macro(pp, nested, depth + 1);
                    free(nested);
                    if (ev) {
                        size_t before = (size_t)(mn - 1 - value);
                        size_t after = strlen(mn_end);
                        size_t nlen = before + strlen(ev) + after + 1;
                        char *nv = malloc(nlen);
                        if (nv) {
                            memcpy(nv, value, before);
                            memcpy(nv + before, ev, strlen(ev));
                            memcpy(nv + before + strlen(ev), mn_end, after + 1);
                            free(ev);
                            return nv;
                        }
                        free(ev);
                    }
                }
                break;
            }
        }
        vp++;
    }

    return pp_strdup(value);
}

/* ── Text-level macro substitution ── */

static void pp_substitute_macros(verilog_preprocessor_t *pp)
{
    if (pp->macro_count == 0 || pp->has_error || pp->output_len == 0)
        return;

    size_t cap = pp->output_cap;
    char *out = malloc(cap);
    if (!out) return;
    size_t len = 0;

    const char *p = pp->output;
    const char *end = pp->output + pp->output_len;

    while (p < end) {
        char c = *p;
        if (isalpha(c) || c == '_' || c == '$') {
            const char *id_start = p;
            p++;
            while (p < end && (isalnum(*p) || *p == '_' || *p == '$'))
                p++;
            size_t id_len = (size_t)(p - id_start);

            /* Check against macro table */
            int found = 0;
            for (int i = 0; i < pp->macro_count; i++) {
                size_t mn_len = strlen(pp->macros[i].name);
                if (id_len == mn_len &&
                    memcmp(id_start, pp->macros[i].name, id_len) == 0) {
                    char *ev = expand_macro(pp, pp->macros[i].name, 0);
                    if (ev) {
                        size_t elen = strlen(ev);
                        if (len + elen + 1 > cap) {
                            cap = (len + elen + 1) * 2;
                            char *nb = realloc(out, cap);
                            if (!nb) { free(ev); free(out); return; }
                            out = nb;
                        }
                        memcpy(out + len, ev, elen);
                        len += elen;
                        free(ev);
                    }
                    found = 1;
                    break;
                }
            }
            if (found) continue;

            /* Not a macro — emit as-is */
            if (len + id_len + 1 > cap) {
                cap = (len + id_len + 1) * 2;
                char *nb = realloc(out, cap);
                if (!nb) { free(out); return; }
                out = nb;
            }
            memcpy(out + len, id_start, id_len);
            len += id_len;
        } else {
            if (len + 1 > cap) {
                cap *= 2;
                char *nb = realloc(out, cap);
                if (!nb) { free(out); return; }
                out = nb;
            }
            out[len++] = c;
            p++;
        }
    }

    free(pp->output);
    pp->output = out;
    pp->output_len = len;
    pp->output_cap = cap;
    pp->output[len] = '\0';
}

/* ── Include resolution ── */

/* Resolve include spec "foo.v" or <foo.v> to a full path.
 * For "foo.v": search source directory first, then include paths.
 * For <foo.v>: search include paths only.
 * Returns malloc'd path, or NULL on failure. */
static char *pp_resolve_path(verilog_preprocessor_t *pp, const char *spec,
                              const char *current_dir)
{
    size_t slen = strlen(spec);
    int use_current_dir = 1;
    const char *name;
    size_t nlen;

    if (slen >= 2) {
        if ((spec[0] == '"' && spec[slen-1] == '"') ||
            (spec[0] == '<' && spec[slen-1] == '>')) {
            name = spec + 1;
            nlen = slen - 2;
            use_current_dir = (spec[0] == '"');
        } else {
            name = spec;
            nlen = slen;
        }
    } else {
        name = spec;
        nlen = slen;
    }

    if (nlen == 0) return NULL;

    char *name_part = pp_strndup(name, nlen);
    if (!name_part) return NULL;

    /* Try source file directory first (for "..." style) */
    if (use_current_dir && current_dir && current_dir[0]) {
        size_t plen = strlen(current_dir) + 1 + nlen + 1;
        char *path = malloc(plen);
        if (path) {
            snprintf(path, plen, "%s/%s", current_dir, name_part);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); free(name_part); return path; }
            free(path);
        }
    }

    /* Try include paths */
    for (int i = 0; i < pp->include_path_count; i++) {
        size_t plen = strlen(pp->include_paths[i]) + 1 + nlen + 1;
        char *path = malloc(plen);
        if (!path) continue;
        snprintf(path, plen, "%s/%s", pp->include_paths[i], name_part);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); free(name_part); return path; }
        free(path);
    }

    free(name_part);
    return NULL;
}

/* ── Core processing ── */

/* Process source text, emitting preprocessed output to pp->output.
 * current_dir is the directory of the current source file for include
 * resolution (may be NULL or empty).
 *
 * Conditional compilation state is local to this invocation (fresh
 * stack each time), so recursive calls for `include process the
 * included file with independent conditional state. */
static void pp_process_source(verilog_preprocessor_t *pp,
                               const char *source, size_t length,
                               const char *current_dir)
{
    /* Conditional compilation stack.
     * For each level: parent_active (int), branch_taken (int).
     * Two consecutive entries per level. */
    int cond_stack[MAX_COND_STACK * 2];
    int cond_sp = -1;        /* points to the parent_active entry */
    int active = 1;          /* are we currently emitting? */

#define COND_PARENT_ACTIVE(sp)  cond_stack[(sp)]
#define COND_BRANCH_TAKEN(sp)   cond_stack[(sp) + 1]

    const char *end = source + length;
    const char *p = source;

    while (p < end && !pp->has_error) {
        /* Find end of line */
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;

        size_t line_len = (size_t)(eol - p);

        /* Check for ` at start of line (after optional whitespace) */
        const char *line = p;
        int directive_start_col = 0;

        if (line_len > 0) {
            /* Find backtick position */
            const char *bp = line;
            const char *backtick = NULL;
            while (bp < eol && (*bp == ' ' || *bp == '\t')) { bp++; directive_start_col = (int)(bp - line); }

            if (bp < eol && *bp == '`') {
                const char *dn = bp + 1;  /* directive name starts after ` */

                /* Read directive name */
                if (dn < eol && (isalpha(*dn) || *dn == '_')) {
                    const char *dn_end = dn;
                    while (dn_end < eol && (isalpha(*dn_end) || *dn_end == '_'))
                        dn_end++;
                    size_t dn_len = (size_t)(dn_end - dn);

                    /* Rest of the line after directive name */
                    const char *rest = dn_end;
                    skip_ws((char **)&rest, eol);
                    size_t rest_len = (size_t)(eol - rest);

                    int is_cond = 0;  /* directives that affect conditional nesting */
                    int consumed = 0; /* line was consumed as a directive */

#define DN_MATCH(s) (dn_len == sizeof(s)-1 && memcmp(dn, s, sizeof(s)-1) == 0)

                    if (DN_MATCH("define")) {
                        if (active) {
                            char *def_text = pp_strndup(rest, rest_len);
                            if (def_text) { pp_define(pp, def_text, rest_len); free(def_text); }
                        }
                        consumed = 1;
                    } else if (DN_MATCH("undef")) {
                        if (active) {
                            char *ud_text = pp_strndup(rest, rest_len);
                            if (ud_text) { pp_undef(pp, ud_text, rest_len); free(ud_text); }
                        }
                        consumed = 1;
                    } else if (DN_MATCH("ifdef")) {
                        is_cond = 1;
                        if (cond_sp >= (MAX_COND_STACK - 1) * 2) {
                            pp_set_error(pp, "`ifdef nesting too deep");
                        } else {
                            cond_sp += 2;
                            COND_PARENT_ACTIVE(cond_sp) = active;
                            COND_BRANCH_TAKEN(cond_sp) = 0;
                            if (active) {
                                char *mname = pp_strndup(rest, rest_len);
                                if (mname && pp_is_defined(pp, mname)) {
                                    active = 1;
                                    COND_BRANCH_TAKEN(cond_sp) = 1;
                                } else {
                                    active = 0;
                                }
                                free(mname);
                            }
                        }
                        consumed = 1;
                    } else if (DN_MATCH("ifndef")) {
                        is_cond = 1;
                        if (cond_sp >= (MAX_COND_STACK - 1) * 2) {
                            pp_set_error(pp, "`ifndef nesting too deep");
                        } else {
                            cond_sp += 2;
                            COND_PARENT_ACTIVE(cond_sp) = active;
                            COND_BRANCH_TAKEN(cond_sp) = 0;
                            if (active) {
                                char *mname = pp_strndup(rest, rest_len);
                                if (mname && !pp_is_defined(pp, mname)) {
                                    active = 1;
                                    COND_BRANCH_TAKEN(cond_sp) = 1;
                                } else {
                                    active = 0;
                                }
                                free(mname);
                            }
                        }
                        consumed = 1;
                    } else if (DN_MATCH("elsif")) {
                        is_cond = 1;
                        if (cond_sp >= 0) {
                            if (COND_BRANCH_TAKEN(cond_sp)) {
                                active = 0;  /* already took a branch */
                            } else if (COND_PARENT_ACTIVE(cond_sp)) {
                                char *mname = pp_strndup(rest, rest_len);
                                if (mname && pp_is_defined(pp, mname)) {
                                    active = 1;
                                    COND_BRANCH_TAKEN(cond_sp) = 1;
                                } else {
                                    active = 0;
                                }
                                free(mname);
                            } else {
                                active = 0;  /* parent was skipping */
                            }
                        } else {
                            pp_set_error(pp, "`elsif without matching `ifdef/`ifndef");
                        }
                        consumed = 1;
                    } else if (DN_MATCH("else")) {
                        is_cond = 1;
                        if (cond_sp >= 0) {
                            if (COND_BRANCH_TAKEN(cond_sp)) {
                                active = 0;  /* already took a branch */
                            } else if (COND_PARENT_ACTIVE(cond_sp)) {
                                active = 1;
                                COND_BRANCH_TAKEN(cond_sp) = 1;
                            } else {
                                active = 0;  /* parent was skipping */
                            }
                        } else {
                            pp_set_error(pp, "`else without matching `ifdef/`ifndef");
                        }
                        consumed = 1;
                    } else if (DN_MATCH("endif")) {
                        is_cond = 1;
                        if (cond_sp >= 0) {
                            active = COND_PARENT_ACTIVE(cond_sp);
                            cond_sp -= 2;
                        } else {
                            pp_set_error(pp, "`endif without matching `ifdef/`ifndef");
                        }
                        consumed = 1;
                    } else if (DN_MATCH("include")) {
                        if (active && !pp->has_error) {
                            char *spec = pp_strndup(rest, rest_len);
                            if (spec) {
                                char *path = pp_resolve_path(pp, spec, current_dir);
                                if (path) {
                                    FILE *f = fopen(path, "rb");
                                    if (f) {
                                        fseek(f, 0, SEEK_END);
                                        long flen = ftell(f);
                                        fseek(f, 0, SEEK_SET);
                                        if (flen > 0 && flen < 1024L * 1024L * 16L) {
                                            char *fc = malloc((size_t)flen + 1);
                                            if (fc) {
                                                size_t nr = fread(fc, 1, (size_t)flen, f);
                                                fc[nr] = '\0';
                                                pp->include_depth++;
                                                if (pp->include_depth > 32) {
                                                    pp_set_error(pp, "`include: too deeply nested");
                                                } else {
                                                    /* Directory for nested includes */
                                                    char *inc_dir = pp_strdup(path);
                                                    if (inc_dir) {
                                                        char *ls = strrchr(inc_dir, '/');
                                                        char *lb = strrchr(inc_dir, '\\');
                                                        char *last = ls;
                                                        if (!last || (lb && lb > last)) last = lb;
                                                        if (last) *last = '\0'; else inc_dir[0] = '\0';
                                                        pp_process_source(pp, fc, nr, inc_dir);
                                                        free(inc_dir);
                                                    } else {
                                                        pp_process_source(pp, fc, nr, NULL);
                                                    }
                                                }
                                                pp->include_depth--;
                                                free(fc);
                                            }
                                        } else if (flen >= 1024L * 1024L * 16L) {
                                            pp_set_error(pp, "`include: file too large (%s)", path);
                                        }
                                        fclose(f);
                                    }
                                    free(path);
                                } else {
                                    pp_set_error(pp, "`include: '%s' not found", spec);
                                }
                                free(spec);
                            }
                        }
                        consumed = 1;
                    } else if (DN_MATCH("timescale")) {
                        /* Preserve as comment for metadata extraction */
                        if (active) {
                            pp_emit_str(pp, "// `timescale ");
                            pp_emit(pp, rest, rest_len);
                            pp_emit_char(pp, '\n');
                        }
                        consumed = 1;
                    } else if (DN_MATCH("resetall")) {
                        if (active) {
                            for (int i = 0; i < pp->macro_count; i++) {
                                free(pp->macros[i].name);
                                free(pp->macros[i].value);
                            }
                            pp->macro_count = 0;
                        }
                        consumed = 1;
                    } else if (DN_MATCH("celldefine") || DN_MATCH("endcelldefine")) {
                        consumed = 1;
                    } else if (DN_MATCH("default_nettype")) {
                        if (active) {
                            pp_emit_str(pp, "// `default_nettype ");
                            pp_emit(pp, rest, rest_len);
                            pp_emit_char(pp, '\n');
                        }
                        consumed = 1;
                    } else if (DN_MATCH("pragma") ||
                               DN_MATCH("unconnected_drive") ||
                               DN_MATCH("nounconnected_drive") ||
                               DN_MATCH("line")) {
                        consumed = 1;
                    }

#undef DN_MATCH

                    if (consumed && !is_cond) {
                        /* Non-conditional directive — no extra handling needed */
                        p = eol;
                        if (p < end && *p == '\n') p++;
                        continue;
                    }
                    if (is_cond) {
                        p = eol;
                        if (p < end && *p == '\n') p++;
                        continue;
                    }
                    /* Unknown `directive — fall through and emit as text */
                }
            }
        }

        /* Not a compiler directive line — emit if active */
        if (active) {
            pp_emit(pp, line, line_len);
            pp_emit_char(pp, '\n');
        }

        p = eol;
        if (p < end && *p == '\n') p++;
    }

    if (cond_sp >= 0 && !pp->has_error) {
        pp_set_error(pp, "unterminated `ifdef/`ifndef (depth %d)", (cond_sp / 2) + 1);
    }
}

/* ── Public API ── */

verilog_preprocessor_t *verilog_preprocessor_create(void)
{
    verilog_preprocessor_t *pp = calloc(1, sizeof(verilog_preprocessor_t));
    if (!pp) return NULL;
    pp->output = malloc(OUTPUT_INIT_CAP);
    if (!pp->output) { free(pp); return NULL; }
    pp->output_cap = OUTPUT_INIT_CAP;
    pp->output[0] = '\0';
    return pp;
}

void verilog_preprocessor_destroy(verilog_preprocessor_t *pp)
{
    if (!pp) return;
    for (int i = 0; i < pp->macro_count; i++) {
        free(pp->macros[i].name);
        free(pp->macros[i].value);
    }
    for (int i = 0; i < pp->include_path_count; i++)
        free(pp->include_paths[i]);
    free(pp->output);
    free(pp);
}

void verilog_preprocessor_add_include_path(verilog_preprocessor_t *pp,
                                            const char *path)
{
    if (!pp || !path || pp->include_path_count >= MAX_INCLUDE_PATHS) return;
    pp->include_paths[pp->include_path_count++] = pp_strdup(path);
}

const char *verilog_preprocessor_get_error(verilog_preprocessor_t *pp)
{
    return pp ? pp->error_msg : "NULL preprocessor";
}

char *verilog_preprocessor_process(verilog_preprocessor_t *pp,
                                    const char *filename,
                                    const char *source, size_t length)
{
    if (!pp || !source) return NULL;

    /* Auto-detect length if not provided */
    if (length == 0) length = strlen(source);

    pp->has_error = 0;
    pp->error_msg[0] = '\0';
    pp->output_len = 0;
    pp->output[0] = '\0';
    pp->include_depth = 0;

    /* Extract directory from filename  */
    char *current_dir = NULL;
    if (filename && filename[0]) {
        current_dir = pp_strdup(filename);
        if (current_dir) {
            char *ls = strrchr(current_dir, '/');
            char *lb = strrchr(current_dir, '\\');
            char *last = ls;
            if (!last || (lb && lb > last)) last = lb;
            if (last) *last = '\0'; else current_dir[0] = '\0';
        }
    }

    pp_process_source(pp, source, length, current_dir);
    free(current_dir);
    if (pp->has_error) return NULL;

    pp_ensure_cap(pp, 1);
    if (pp->has_error) return NULL;
    pp->output[pp->output_len] = '\0';

    pp_substitute_macros(pp);
    if (pp->has_error) return NULL;

    return pp_strdup(pp->output);
}

char *verilog_preprocessor_process_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (flen <= 0 || flen >= 1024L * 1024L * 16L) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[nread] = '\0';

    verilog_preprocessor_t *pp = verilog_preprocessor_create();
    if (!pp) { free(buf); return NULL; }

    /* Add the source file's directory as the default include path */
    char *dir = pp_strdup(path);
    if (dir) {
        char *ls = strrchr(dir, '/');
        char *lb = strrchr(dir, '\\');
        char *last = ls;
        if (!last || (lb && lb > last)) last = lb;
        if (last) *last = '\0';
        if (dir[0]) verilog_preprocessor_add_include_path(pp, dir);
        free(dir);
    }

    char *result = verilog_preprocessor_process(pp, path, buf, nread);
    free(buf);
    verilog_preprocessor_destroy(pp);
    return result;
}
