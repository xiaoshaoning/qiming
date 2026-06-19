#include "libqsim/verilog_lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct verilog_lexer {
    const char *input;
    size_t len;
    size_t pos;
    size_t line_start;
    uint32_t line;
    token_t current;
    int has_current;
};

verilog_lexer_t *verilog_lexer_create(const char *input, size_t length)
{
    verilog_lexer_t *lex = calloc(1, sizeof(verilog_lexer_t));
    if (!lex) return NULL;
    lex->input = input;
    lex->len = length;
    lex->pos = 0;
    lex->line = 1;
    lex->line_start = 0;
    lex->has_current = 0;
    return lex;
}

void verilog_lexer_destroy(verilog_lexer_t *lexer)
{
    free(lexer);
}

size_t verilog_lexer_position(verilog_lexer_t *lexer)
{
    return lexer->pos;
}

uint32_t verilog_lexer_line(verilog_lexer_t *lexer)
{
    return lexer->line;
}

static void skip_whitespace(verilog_lexer_t *lex)
{
    while (lex->pos < lex->len) {
        char c = lex->input[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r')
            lex->pos++;
        else if (c == '\n') {
            lex->pos++;
            lex->line++;
            lex->line_start = lex->pos;
        } else
            break;
    }
}

/* Lookup keyword from identifier text; returns keyword token or TOK_IDENTIFIER. */
static token_kind_t lookup_keyword(const char *start, size_t len)
{
    /* Simple switch on first char + length for speed */
#define KW(s, tok) do { if (len == sizeof(s)-1 && memcmp(start, s, len) == 0) return tok; } while(0)
    switch (len) {
        case 2: KW("if", TOK_KW_IF); KW("or", TOK_OR); break;
        case 3: KW("for", TOK_KW_FOR); KW("reg", TOK_KW_REG); KW("not", TOK_TILDE); KW("tri", TOK_KW_TRI); KW("wor", TOK_KW_WOR); break;
        case 4:
            KW("wire", TOK_KW_WIRE);
            KW("else", TOK_KW_ELSE);
            KW("case", TOK_KW_CASE);
            KW("task", TOK_KW_TASK);
            KW("begin", TOK_KW_BEGIN);
            KW("wand", TOK_KW_WAND);
            KW("tri0", TOK_KW_TRI0);
            KW("tri1", TOK_KW_TRI1);
            break;
        case 5:
            KW("input", TOK_KW_INPUT);
            KW("output", TOK_KW_OUTPUT);
            KW("inout", TOK_KW_INOUT);
            KW("always", TOK_KW_ALWAYS);
            KW("initial", TOK_KW_INITIAL);
            KW("endcase", TOK_KW_ENDCASE);
            KW("posedge", TOK_KW_POSEDGE);
            KW("negedge", TOK_KW_NEGEDGE);
            KW("module", TOK_KW_MODULE);
            KW("assign", TOK_KW_ASSIGN);
            KW("endtask", TOK_KW_ENDTASK);
            KW("trior", TOK_KW_TRIOR);
            KW("uwire", TOK_KW_UWIRE);
            KW("logic", TOK_KW_LOGIC);
            break;
        case 6:
            KW("default", TOK_KW_DEFAULT);
            KW("endmodule", TOK_KW_ENDMODULE);
            KW("endfunction", TOK_KW_ENDFUNCTION);
            KW("forever", TOK_KW_FOREVER);
            KW("triand", TOK_KW_TRIAND);
            KW("trireg", TOK_KW_TRIREG);
            KW("import", TOK_KW_IMPORT);
            break;
        case 7:
            KW("integer", TOK_KW_INTEGER);
            KW("supply0", TOK_KW_SUPPLY0);
            KW("supply1", TOK_KW_SUPPLY1);
            KW("modport", TOK_KW_MODPORT);
            KW("package", TOK_KW_PACKAGE);
            break;
        case 8:
            KW("generate", TOK_KW_GENERATE);
            KW("endgenerate", TOK_KW_ENDGENERATE);
            KW("localparam", TOK_KW_LOCALPARAM);
            break;
        case 9:
            KW("parameter", TOK_KW_PARAMETER);
            KW("endmodule", TOK_KW_ENDMODULE);
            KW("always_ff", TOK_KW_ALWAYS_FF);
            KW("interface", TOK_KW_INTERFACE);
            break;
        case 10:
            KW("endgenerate", TOK_KW_ENDGENERATE);
            KW("endpackage", TOK_KW_ENDPACKAGE);
            break;
        case 11:
            KW("always_comb", TOK_KW_ALWAYS_COMB);
            break;
        case 12:
            KW("always_latch", TOK_KW_ALWAYS_LATCH);
            KW("endinterface", TOK_KW_ENDINTERFACE);
            break;
    }
#undef KW
    (void)start;
    return TOK_IDENTIFIER;
}

static token_t make_token(verilog_lexer_t *lex, token_kind_t kind,
                           const char *start, size_t len)
{
    token_t t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.start = start;
    t.length = len;
    return t;
}

static token_t lex_number(verilog_lexer_t *lex, char first)
{
    const char *start = lex->input + lex->pos - 1;
    size_t slen = 1;
    uint64_t value = 0;
    int base = 10;
    int width = 0;

    /* Check for sized constant: <width>'<base><digits> */
    if (first >= '0' && first <= '9') {
        /* Read initial digits to determine if it's a sized constant */
        const char *p = lex->input + lex->pos;
        size_t saved_pos = lex->pos;

        /* Read digits for width */
        while (lex->pos < lex->len && isdigit(lex->input[lex->pos]))
            lex->pos++;

        if (lex->pos < lex->len && lex->input[lex->pos] == '\'') {
            /* Sized constant: width'[s]base digits */
            char width_str[64];
            size_t wlen = (size_t)(lex->pos - saved_pos);
            if (wlen < sizeof(width_str)) {
                memcpy(width_str, start, wlen);
                width_str[wlen] = '\0';
                width = (int)strtoul(width_str, NULL, 10);
            }
            lex->pos++; /* skip ' */
            if (lex->pos < lex->len && (lex->input[lex->pos] == 's' || lex->input[lex->pos] == 'S'))
                lex->pos++; /* skip signed indicator */

            if (lex->pos < lex->len) {
                char b = (char)tolower(lex->input[lex->pos]);
                if (b == 'b') base = 2;
                else if (b == 'o') base = 8;
                else if (b == 'd') base = 10;
                else if (b == 'h') base = 16;
                lex->pos++; /* skip base char */
            }

            /* Parse digits */
            slen = lex->pos - (size_t)(start - lex->input);
            while (lex->pos < lex->len) {
                char c = (char)tolower(lex->input[lex->pos]);
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c == '_') { lex->pos++; continue; }
                else if (c == 'x' || c == 'z') { /* X/Z not handled in numeric constant */ break; }
                else break;
                if (digit >= base) break;
                value = value * (uint64_t)base + (uint64_t)digit;
                lex->pos++;
            }
            slen = lex->pos - (size_t)(start - lex->input);
        } else {
            /* Plain decimal number */
            lex->pos = saved_pos;
            while (lex->pos < lex->len && isdigit(lex->input[lex->pos])) {
                value = value * 10 + (uint64_t)(lex->input[lex->pos] - '0');
                lex->pos++;
            }
            slen = lex->pos - (size_t)(start - lex->input);
        }
    }

    token_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TOK_NUMBER;
    t.start = start;
    t.length = slen;
    t.num_value = value;
    t.num_base = base;
    t.num_width = width;
    return t;
}

static token_t lex_string(verilog_lexer_t *lex)
{
    const char *start = lex->input + lex->pos - 1;
    while (lex->pos < lex->len) {
        char c = lex->input[lex->pos];
        if (c == '"') { lex->pos++; break; }
        if (c == '\n') break;
        if (c == '\\') { lex->pos++; if (lex->pos < lex->len) lex->pos++; }
        else lex->pos++;
    }
    return make_token(lex, TOK_STRING, start, lex->pos - (size_t)(start - lex->input));
}

token_t verilog_lexer_next(verilog_lexer_t *lexer)
{
    skip_whitespace(lexer);

    if (lexer->pos >= lexer->len) {
        return make_token(lexer, TOK_EOF, NULL, 0);
    }

    char c = lexer->input[lexer->pos];
    const char *start = lexer->input + lexer->pos;

    lexer->pos++;

    /* Identifier or keyword */
    if (isalpha(c) || c == '_' || c == '$') {
        while (lexer->pos < lexer->len) {
            char nc = lexer->input[lexer->pos];
            if (isalnum(nc) || nc == '_' || nc == '$')
                lexer->pos++;
            else
                break;
        }
        size_t len = lexer->pos - (size_t)(start - lexer->input);
        token_kind_t kw = lookup_keyword(start, len);
        return make_token(lexer, kw, start, len);
    }

    /* Number */
    if (isdigit(c)) {
        return lex_number(lexer, c);
    }

    /* String literal */
    if (c == '"') {
        return lex_string(lexer);
    }

    /* Line comment */
    if (c == '/' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '/') {
        while (lexer->pos < lexer->len && lexer->input[lexer->pos] != '\n')
            lexer->pos++;
        return verilog_lexer_next(lexer);
    }

    /* Block comment */
    if (c == '/' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '*') {
        lexer->pos++; /* skip * */
        while (lexer->pos + 1 < lexer->len) {
            if (lexer->input[lexer->pos] == '*' && lexer->input[lexer->pos + 1] == '/') {
                lexer->pos += 2;
                break;
            }
            if (lexer->input[lexer->pos] == '\n') {
                lexer->line++;
                lexer->line_start = lexer->pos + 1;
            }
            lexer->pos++;
        }
        return verilog_lexer_next(lexer);
    }

    /* Multi-character operators */
    if (c == '=' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '=') {
        lexer->pos++;
        /* Check for === */
        if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '=') {
            lexer->pos++;
            return make_token(lexer, TOK_CEQ, start, 3);
        }
        return make_token(lexer, TOK_EQ, start, 2);
    }
    if (c == '!' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '=') {
        lexer->pos++;
        if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '=') {
            lexer->pos++;
            return make_token(lexer, TOK_CNEQ, start, 3);
        }
        return make_token(lexer, TOK_NEQ, start, 2);
    }
    if (c == '<') {
        if (lexer->pos < lexer->len) {
            if (lexer->input[lexer->pos] == '=') {
                lexer->pos++;
                return make_token(lexer, TOK_LE, start, 2);
            }
            if (lexer->input[lexer->pos] == '<') {
                lexer->pos++;
                if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '<') {
                    lexer->pos++;
                    return make_token(lexer, TOK_SSHL, start, 3);
                }
                return make_token(lexer, TOK_SHL, start, 2);
            }
        }
        return make_token(lexer, TOK_LT, start, 1);
    }
    if (c == '>') {
        if (lexer->pos < lexer->len) {
            if (lexer->input[lexer->pos] == '=') {
                lexer->pos++;
                return make_token(lexer, TOK_GE, start, 2);
            }
            if (lexer->input[lexer->pos] == '>') {
                lexer->pos++;
                if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '>') {
                    lexer->pos++;
                    return make_token(lexer, TOK_SSHR, start, 3);
                }
                return make_token(lexer, TOK_SHR, start, 2);
            }
        }
        return make_token(lexer, TOK_GT, start, 1);
    }
    if (c == '&' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '&') {
        lexer->pos++;
        return make_token(lexer, TOK_LAND, start, 2);
    }
    if (c == '|' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '|') {
        lexer->pos++;
        return make_token(lexer, TOK_LOR, start, 2);
    }
    if (c == '*' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '*') {
        lexer->pos++;
        return make_token(lexer, TOK_POW, start, 2);
    }
    if (c == '-' && lexer->pos < lexer->len && lexer->input[lexer->pos] == '>') {
        lexer->pos++;
        return make_token(lexer, TOK_MINUS, start, 1); /* -> is not standard Verilog, treat as - > */
    }
    if (c == ':' && lexer->pos < lexer->len && lexer->input[lexer->pos] == ':') {
        lexer->pos++;
        return make_token(lexer, TOK_DBL_COLON, start, 2);
    }

    /* Single-character tokens */
    switch (c) {
        case '+': return make_token(lexer, TOK_PLUS, start, 1);
        case '-': return make_token(lexer, TOK_MINUS, start, 1);
        case '*': return make_token(lexer, TOK_STAR, start, 1);
        case '/': return make_token(lexer, TOK_SLASH, start, 1);
        case '%': return make_token(lexer, TOK_PERCENT, start, 1);
        case '&': return make_token(lexer, TOK_AND, start, 1);
        case '|': return make_token(lexer, TOK_OR, start, 1);
        case '^': return make_token(lexer, TOK_XOR, start, 1);
        case '~': return make_token(lexer, TOK_TILDE, start, 1);
        case '!': return make_token(lexer, TOK_LNOT, start, 1);
        case '=': return make_token(lexer, TOK_ASSIGN, start, 1);
        case '<': return make_token(lexer, TOK_LT, start, 1);
        case '>': return make_token(lexer, TOK_GT, start, 1);
        case '?': return make_token(lexer, TOK_QUESTION, start, 1);
        case ':': return make_token(lexer, TOK_COLON, start, 1);
        case ',': return make_token(lexer, TOK_COMMA, start, 1);
        case '.': return make_token(lexer, TOK_DOT, start, 1);
        case ';': return make_token(lexer, TOK_SEMI, start, 1);
        case '(': return make_token(lexer, TOK_LPAREN, start, 1);
        case ')': return make_token(lexer, TOK_RPAREN, start, 1);
        case '[': return make_token(lexer, TOK_LBRACKET, start, 1);
        case ']': return make_token(lexer, TOK_RBRACKET, start, 1);
        case '{': return make_token(lexer, TOK_LBRACE, start, 1);
        case '}': return make_token(lexer, TOK_RBRACE, start, 1);
        case '@': return make_token(lexer, TOK_AT, start, 1);
        case '#': return make_token(lexer, TOK_HASH, start, 1);
        default:  return make_token(lexer, TOK_EOF, start, 1);
    }
}

token_t verilog_lexer_peek(verilog_lexer_t *lexer)
{
    if (!lexer->has_current) {
        lexer->current = verilog_lexer_next(lexer);
        lexer->has_current = 1;
    }
    return lexer->current;
}

int verilog_lexer_match(verilog_lexer_t *lexer, token_kind_t kind)
{
    token_t t = verilog_lexer_peek(lexer);
    return t.kind == kind;
}

int verilog_lexer_expect(verilog_lexer_t *lexer, token_kind_t kind)
{
    if (!verilog_lexer_match(lexer, kind)) return 0;
    lexer->has_current = 0;
    return 1;
}
