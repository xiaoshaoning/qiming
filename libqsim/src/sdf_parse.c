/* SDF (Standard Delay Format) parser — IEEE 1497 minimal subset.
 * Supports (DELAY (ABSOLUTE (IOPATH ...))) for path delay annotation.
 * Token-based recursive descent. */
#include "libqsim/sdf_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ── Tokenizer ── */

typedef enum {
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_IDENT,    /* keyword or identifier */
    TOK_NUMBER,   /* numeric value (int or float) as string */
    TOK_STRING,   /* quoted string content */
    TOK_EOF,
    TOK_ERROR
} sdf_token_kind_t;

typedef struct {
    sdf_token_kind_t kind;
    char text[256];
    double num_val;   /* parsed for TOK_NUMBER */
} sdf_token_t;

/* Max recursion depth for SDF nesting */
#define SDF_MAX_DEPTH 64

/* Parser context */
typedef struct {
    const char *p;           /* current position in input */
    const char *filepath;    /* for error messages */
    sdf_token_t tok;         /* current lookahead token */
    int tok_valid;           /* 0 = need to advance */
    int error;
    int depth;               /* recursion depth guard */
    uint64_t scale;          /* timescale multiplier */
    char divider;            /* hierarchy divider from (DIVIDER ...) */
    sdf_cell_t *cells;
    size_t cell_count, cell_cap;
} sdf_ctx_t;

/* Read next token from input */
static void sdf_next_token(sdf_ctx_t *ctx) {
    const char **input = &ctx->p;

    /* Skip whitespace and SDF comments (; to end of line) */
    while (**input) {
        char c = **input;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            (*input)++;
            continue;
        }
        if (c == ';') {
            while (**input && **input != '\n') (*input)++;
            if (**input == '\n') (*input)++;
            continue;
        }
        break;
    }

    ctx->tok.text[0] = '\0';
    if (**input == '\0') {
        ctx->tok.kind = TOK_EOF;
        ctx->tok_valid = 1;
        return;
    }

    if (**input == '(') {
        (*input)++;
        ctx->tok.kind = TOK_LPAREN;
        ctx->tok_valid = 1;
        return;
    }
    if (**input == ')') {
        (*input)++;
        ctx->tok.kind = TOK_RPAREN;
        ctx->tok_valid = 1;
        return;
    }
    if (**input == '"') {
        (*input)++;
        size_t i = 0;
        ctx->tok.kind = TOK_STRING;
        while (**input && **input != '"' && i < sizeof(ctx->tok.text) - 1) {
            ctx->tok.text[i++] = *(*input)++;
        }
        ctx->tok.text[i] = '\0';
        if (**input == '"') (*input)++;
        ctx->tok_valid = 1;
        return;
    }

    /* Colon (used in triple delay values) */
    if (**input == ':') {
        (*input)++;
        ctx->tok.kind = TOK_IDENT;
        ctx->tok.text[0] = ':';
        ctx->tok.text[1] = '\0';
        ctx->tok_valid = 1;
        return;
    }

    /* Number or identifier */
    size_t i = 0;
    ctx->tok.kind = TOK_IDENT;
    /* Check if it starts with a digit, dot, or sign followed by digit */
    int is_num = (**input >= '0' && **input <= '9');
    if ((**input == '+' || **input == '-') && i < sizeof(ctx->tok.text)-1) {
        ctx->tok.text[i++] = *(*input)++;
        if (**input >= '0' && **input <= '9') is_num = 1;
    }
    while (**input && !strchr(" \t\r\n();\":", **input)) {
        char c = **input;
        if (i >= sizeof(ctx->tok.text) - 1) break;
        ctx->tok.text[i++] = c;
        (*input)++;
        /* Validity check for numeric: digits, dots, 'e', 'E' only */
        if (is_num && !((c >= '0' && c <= '9') || c == '.' ||
            c == 'e' || c == 'E' || c == '+' || c == '-')) {
            is_num = 0;
        }
    }
    ctx->tok.text[i] = '\0';

    if (is_num && i > 0 && (ctx->tok.text[0] >= '0' && ctx->tok.text[0] <= '9')) {
        ctx->tok.kind = TOK_NUMBER;
        ctx->tok.num_val = strtod(ctx->tok.text, NULL);
    }
    ctx->tok_valid = 1;
}

static void sdf_advance(sdf_ctx_t *ctx) {
    ctx->tok_valid = 0;
}

static void sdf_expect(sdf_ctx_t *ctx, sdf_token_kind_t kind) {
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind != kind) {
        ctx->error = 1;
    }
    sdf_advance(ctx);
}

static int sdf_peek(sdf_ctx_t *ctx, sdf_token_kind_t kind) {
    if (!ctx->tok_valid) sdf_next_token(ctx);
    return ctx->tok.kind == kind;
}

static int sdf_peek_ident(sdf_ctx_t *ctx, const char *name) {
    if (!ctx->tok_valid) sdf_next_token(ctx);
    return ctx->tok.kind == TOK_IDENT && strcmp(ctx->tok.text, name) == 0;
}

/* ── Recursive descent parser ── */

/* Forward declarations */
static void parse_sdf_file(sdf_ctx_t *ctx);
static void parse_cell(sdf_ctx_t *ctx);

/* Parse a delay value: (number) or (min:typ:max).
 * Returns the "typ" (middle) value for triples, or the single value. */
static uint64_t parse_delay_value(sdf_ctx_t *ctx) {
    if (!ctx->tok_valid) sdf_next_token(ctx);
    uint64_t result = 0;

    if (sdf_peek(ctx, TOK_LPAREN)) {
        sdf_advance(ctx);
        if (!ctx->tok_valid) sdf_next_token(ctx);

        if (ctx->tok.kind == TOK_NUMBER) {
            double val = ctx->tok.num_val;
            sdf_advance(ctx);
            /* Check for triple: (min:typ:max) */
            if (!ctx->tok_valid) sdf_next_token(ctx);
            if (ctx->tok.kind == TOK_IDENT && strcmp(ctx->tok.text, ":") == 0) {
                /* Triple — skip min, read typ */
                sdf_advance(ctx);
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_NUMBER) {
                    val = ctx->tok.num_val; /* use typ value */
                    sdf_advance(ctx);
                }
                /* Skip :max */
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_IDENT && strcmp(ctx->tok.text, ":") == 0) {
                    sdf_advance(ctx);
                    if (!ctx->tok_valid) sdf_next_token(ctx);
                    if (ctx->tok.kind == TOK_NUMBER) sdf_advance(ctx);
                }
            }
            /* Scale and convert to uint64 */
            double scaled = val * (double)ctx->scale;
            if (scaled < 0) scaled = 0;
            result = (uint64_t)llround(scaled);
        }

        if (!ctx->tok_valid) sdf_next_token(ctx);
        if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
    }

    return result;
}

/* Parse IOPATH entry: (IOPATH src_pin dst_pin rise_delay fall_delay) */
static void parse_iopath(sdf_ctx_t *ctx, sdf_cell_t *cell) {
    /* Already consumed LPAREN and IOPATH identifier */
    if (!ctx->tok_valid) sdf_next_token(ctx);

    /* Source pin */
    if (ctx->tok.kind != TOK_IDENT) { ctx->error = 1; return; }
    char src_pin[256];
    strncpy(src_pin, ctx->tok.text, sizeof(src_pin)-1);
    src_pin[sizeof(src_pin)-1] = '\0';
    sdf_advance(ctx);

    /* Destination pin */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind != TOK_IDENT) { ctx->error = 1; return; }
    char dst_pin[256];
    strncpy(dst_pin, ctx->tok.text, sizeof(dst_pin)-1);
    dst_pin[sizeof(dst_pin)-1] = '\0';
    sdf_advance(ctx);

    /* Rise delay */
    uint64_t rise = parse_delay_value(ctx);

    /* Fall delay */
    uint64_t fall = parse_delay_value(ctx);

    /* Consume closing RPAREN */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);

    /* Add to cell */
    if (cell->iopath_count >= cell->iopath_cap) {
        size_t new_cap = cell->iopath_cap ? cell->iopath_cap * 2 : 8;
        sdf_iopath_t *np = realloc(cell->iopaths, new_cap * sizeof(sdf_iopath_t));
        if (!np) { ctx->error = 1; return; }
        cell->iopaths = np;
        cell->iopath_cap = new_cap;
    }
    size_t n = cell->iopath_count++;
    cell->iopaths[n].src_pin = strdup(src_pin);
    cell->iopaths[n].dst_pin = strdup(dst_pin);
    cell->iopaths[n].rise_delay = rise;
    cell->iopaths[n].fall_delay = fall;
}

/* Parse absolute delay group: (DELAY (ABSOLUTE iopath*)) */
static void parse_absolute_delay(sdf_ctx_t *ctx, sdf_cell_t *cell) {
    /* Already consumed LPAREN, "DELAY" */

    /* LPAREN "ABSOLUTE" */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (!sdf_peek(ctx, TOK_LPAREN)) { ctx->error = 1; return; }
    sdf_advance(ctx);
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind != TOK_IDENT || strcmp(ctx->tok.text, "ABSOLUTE") != 0) {
        ctx->error = 1; return;
    }
    sdf_advance(ctx);

    /* Parse IOPATH entries */
    while (!ctx->error) {
        if (!ctx->tok_valid) sdf_next_token(ctx);
        if (sdf_peek(ctx, TOK_RPAREN)) break;  /* closing ) of (ABSOLUTE ...) */

        if (sdf_peek(ctx, TOK_LPAREN)) {
            sdf_advance(ctx);
            if (!ctx->tok_valid) sdf_next_token(ctx);
            if (ctx->tok.kind == TOK_IDENT && strcmp(ctx->tok.text, "IOPATH") == 0) {
                sdf_advance(ctx);
                parse_iopath(ctx, cell);
            } else {
                /* Unknown entry — skip to matching ) */
                int depth = 1;
                while (depth > 0 && !ctx->error) {
                    if (!ctx->tok_valid) sdf_next_token(ctx);
                    if (ctx->tok.kind == TOK_LPAREN) depth++;
                    else if (ctx->tok.kind == TOK_RPAREN) depth--;
                    else if (ctx->tok.kind == TOK_EOF) break;
                    sdf_advance(ctx);
                }
            }
        } else {
            break;
        }
    }

    /* Consume ) of ABSOLUTE and ) of DELAY */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);  /* ) of ABSOLUTE */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);  /* ) of DELAY */
}

/* Parse CELL entry: (CELL cell_type instance delay*) */
static void parse_cell(sdf_ctx_t *ctx) {
    sdf_cell_t cell;
    memset(&cell, 0, sizeof(cell));
    cell.instance = NULL;
    cell.iopaths = NULL;
    cell.iopath_count = 0;
    cell.iopath_cap = 0;

    /* Consume (CELL ...) */
    /* Already consumed LPAREN and "CELL" */

    while (!ctx->error) {
        if (!ctx->tok_valid) sdf_next_token(ctx);

        if (sdf_peek(ctx, TOK_RPAREN)) {
            sdf_advance(ctx);
            break; /* end of this CELL */
        }

        if (!sdf_peek(ctx, TOK_LPAREN)) { ctx->error = 1; break; }
        sdf_advance(ctx);

        if (!ctx->tok_valid) sdf_next_token(ctx);

        if (ctx->tok.kind == TOK_IDENT) {
            if (strcmp(ctx->tok.text, "CELLTYPE") == 0) {
                sdf_advance(ctx);
                /* Skip optional LPAREN and cell type identifier */
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_LPAREN) sdf_advance(ctx);
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_IDENT) sdf_advance(ctx);
                /* Consume optional closing ) of cell type */
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
                /* Consume closing ) of (CELLTYPE ...) wrapper */
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
            } else if (strcmp(ctx->tok.text, "INSTANCE") == 0) {
                sdf_advance(ctx);
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_IDENT) {
                    cell.instance = strdup(ctx->tok.text);
                    sdf_advance(ctx);
                }
                /* Consume closing ) */
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
            } else if (strcmp(ctx->tok.text, "DELAY") == 0) {
                sdf_advance(ctx);
                parse_absolute_delay(ctx, &cell);
            } else {
                /* Unknown — skip to matching ) */
                int depth = 1;
                while (depth > 0 && !ctx->error) {
                    if (!ctx->tok_valid) sdf_next_token(ctx);
                    if (ctx->tok.kind == TOK_LPAREN) depth++;
                    else if (ctx->tok.kind == TOK_RPAREN) depth--;
                    else if (ctx->tok.kind == TOK_EOF) break;
                    sdf_advance(ctx);
                }
            }
        } else {
            ctx->error = 1;
            break;
        }
    }

    /* Add cell if it has an instance and at least one iopath */
    if (cell.instance && cell.iopath_count > 0) {
        if (ctx->cell_count >= ctx->cell_cap) {
            size_t new_cap = ctx->cell_cap ? ctx->cell_cap * 2 : 16;
            sdf_cell_t *np = realloc(ctx->cells, new_cap * sizeof(sdf_cell_t));
            if (!np) { ctx->error = 1; goto cleanup; }
            ctx->cells = np;
            ctx->cell_cap = new_cap;
        }
        ctx->cells[ctx->cell_count].instance = cell.instance;
        ctx->cells[ctx->cell_count].iopaths = cell.iopaths;
        ctx->cells[ctx->cell_count].iopath_count = cell.iopath_count;
        ctx->cell_count++;
        return;
    }

cleanup:
    free(cell.instance);
    for (size_t i = 0; i < cell.iopath_count; i++) {
        free(cell.iopaths[i].src_pin);
        free(cell.iopaths[i].dst_pin);
    }
    free(cell.iopaths);
}

/* Parse top-level SDF file: (DELAYFILE header* cell*) */
static void parse_sdf_file(sdf_ctx_t *ctx) {
    /* Top-level ( */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (!sdf_peek(ctx, TOK_LPAREN)) { ctx->error = 1; return; }
    sdf_advance(ctx);

    /* DELAYFILE keyword */
    if (!ctx->tok_valid) sdf_next_token(ctx);
    if (ctx->tok.kind != TOK_IDENT || strcmp(ctx->tok.text, "DELAYFILE") != 0) {
        ctx->error = 1;
        return;
    }
    sdf_advance(ctx);

    /* Parse headers and cells */
    while (!ctx->error) {
        if (!ctx->tok_valid) sdf_next_token(ctx);

        if (sdf_peek(ctx, TOK_RPAREN)) {
            sdf_advance(ctx);
            break; /* end of top-level SDF */
        }
        if (sdf_peek(ctx, TOK_EOF)) break;

        if (!sdf_peek(ctx, TOK_LPAREN)) { ctx->error = 1; break; }
        sdf_advance(ctx);

        if (!ctx->tok_valid) sdf_next_token(ctx);
        if (ctx->tok.kind != TOK_IDENT) { ctx->error = 1; break; }

        if (strcmp(ctx->tok.text, "TIMESCALE") == 0) {
            sdf_advance(ctx);
            if (!ctx->tok_valid) sdf_next_token(ctx);
            /* SDF timescale is unquoted (e.g. 1ns), parsed as TOK_IDENT */
            if (ctx->tok.kind == TOK_IDENT || ctx->tok.kind == TOK_STRING) {
                const char *ts = ctx->tok.text;
                double prefix = 1.0;
                char *end = NULL;
                prefix = strtod(ts, &end);
                if (end && *end) {
                    if (strcmp(end, "ps") == 0) ctx->scale = (uint64_t)(prefix * 1);
                    else if (strcmp(end, "ns") == 0) ctx->scale = (uint64_t)(prefix * 1000);
                    else if (strcmp(end, "us") == 0) ctx->scale = (uint64_t)(prefix * 1000000);
                    else if (strcmp(end, "ms") == 0) ctx->scale = (uint64_t)(prefix * 1000000000);
                    else if (strcmp(end, "s") == 0)  ctx->scale = (uint64_t)(prefix * 1000000000000ULL);
                    else ctx->scale = (uint64_t)(prefix * 1000); /* assume ns */
                } else {
                    ctx->scale = (uint64_t)(prefix * 1000);
                }
                sdf_advance(ctx);
            }
            /* Consume closing ) */
            if (!ctx->tok_valid) sdf_next_token(ctx);
            if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
        } else if (strcmp(ctx->tok.text, "DIVIDER") == 0) {
            sdf_advance(ctx);
            if (!ctx->tok_valid) sdf_next_token(ctx);
            if (ctx->tok.kind == TOK_IDENT && ctx->tok.text[0]) {
                ctx->divider = ctx->tok.text[0];
                sdf_advance(ctx);
            } else if (ctx->tok.kind == TOK_STRING && ctx->tok.text[0]) {
                ctx->divider = ctx->tok.text[0];
                sdf_advance(ctx);
            }
            /* Consume closing ) */
            if (!ctx->tok_valid) sdf_next_token(ctx);
            if (ctx->tok.kind == TOK_RPAREN) sdf_advance(ctx);
        } else if (strcmp(ctx->tok.text, "CELL") == 0) {
            sdf_advance(ctx);
            if (ctx->depth < SDF_MAX_DEPTH) {
                ctx->depth++;
                parse_cell(ctx);
                ctx->depth--;
            } else {
                ctx->error = 1;
            }
        } else {
            /* Unknown — skip to matching ) */
            int depth = 1;
            while (depth > 0 && !ctx->error) {
                if (!ctx->tok_valid) sdf_next_token(ctx);
                if (ctx->tok.kind == TOK_LPAREN) depth++;
                else if (ctx->tok.kind == TOK_RPAREN) depth--;
                else if (ctx->tok.kind == TOK_EOF) break;
                sdf_advance(ctx);
            }
        }
    }
}

/* ── Public API ── */

sdf_file_t *sdf_parse_file(const char *filepath) {
    if (!filepath) return NULL;

    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *data = malloc((size_t)len + 1);
    if (!data) { fclose(f); return NULL; }
    size_t nread = fread(data, 1, (size_t)len, f);
    fclose(f);
    data[nread] = '\0';

    /* Initialize parser context */
    sdf_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.p = data;
    ctx.filepath = filepath;
    ctx.scale = 1000; /* default: 1ns = 1000ps */
    ctx.divider = '.';

    /* Parse */
    parse_sdf_file(&ctx);

    free(data);

    if (ctx.error) {
        for (size_t i = 0; i < ctx.cell_count; i++) {
            free(ctx.cells[i].instance);
            for (size_t j = 0; j < ctx.cells[i].iopath_count; j++) {
                free(ctx.cells[i].iopaths[j].src_pin);
                free(ctx.cells[i].iopaths[j].dst_pin);
            }
            free(ctx.cells[i].iopaths);
        }
        free(ctx.cells);
        return NULL;
    }

    sdf_file_t *result = malloc(sizeof(sdf_file_t));
    if (!result) {
        for (size_t i = 0; i < ctx.cell_count; i++) {
            free(ctx.cells[i].instance);
            for (size_t j = 0; j < ctx.cells[i].iopath_count; j++) {
                free(ctx.cells[i].iopaths[j].src_pin);
                free(ctx.cells[i].iopaths[j].dst_pin);
            }
            free(ctx.cells[i].iopaths);
        }
        free(ctx.cells);
        return NULL;
    }

    result->cells = ctx.cells;
    result->cell_count = ctx.cell_count;
    result->divider = ctx.divider;
    result->scale = ctx.scale;

    return result;
}

void sdf_file_free(sdf_file_t *sdf) {
    if (!sdf) return;
    for (size_t i = 0; i < sdf->cell_count; i++) {
        free(sdf->cells[i].instance);
        for (size_t j = 0; j < sdf->cells[i].iopath_count; j++) {
            free(sdf->cells[i].iopaths[j].src_pin);
            free(sdf->cells[i].iopaths[j].dst_pin);
        }
        free(sdf->cells[i].iopaths);
    }
    free(sdf->cells);
    free(sdf);
}
