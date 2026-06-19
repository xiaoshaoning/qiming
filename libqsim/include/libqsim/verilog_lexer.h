#ifndef LIBDSIM_VERILOG_LEXER_H
#define LIBDSIM_VERILOG_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_EOF = 0,

    /* Keywords */
    TOK_KW_MODULE, TOK_KW_ENDMODULE,
    TOK_KW_INPUT, TOK_KW_OUTPUT, TOK_KW_INOUT,
    TOK_KW_WIRE, TOK_KW_REG, TOK_KW_INTEGER,
    TOK_KW_ASSIGN, TOK_KW_ALWAYS, TOK_KW_INITIAL,
    TOK_KW_BEGIN, TOK_KW_END,
    TOK_KW_IF, TOK_KW_ELSE,
    TOK_KW_CASE, TOK_KW_ENDCASE, TOK_KW_CASEX, TOK_KW_CASEZ,
    TOK_KW_FOR,
    TOK_KW_GENERATE, TOK_KW_ENDGENERATE,
    TOK_KW_POSEDGE, TOK_KW_NEGEDGE,
    TOK_KW_PARAMETER, TOK_KW_LOCALPARAM,
    TOK_KW_FUNCTION, TOK_KW_ENDFUNCTION,
    TOK_KW_TASK, TOK_KW_ENDTASK,
    TOK_KW_SPECIFY, TOK_KW_ENDSPECIFY,
    TOK_KW_DEFAULT,
    TOK_KW_FOREVER,
    TOK_KW_TRI, TOK_KW_WAND, TOK_KW_WOR,
    TOK_KW_TRI0, TOK_KW_TRI1, TOK_KW_TRIAND, TOK_KW_TRIOR,
    TOK_KW_TRIREG, TOK_KW_SUPPLY0, TOK_KW_SUPPLY1, TOK_KW_UWIRE,
    TOK_KW_LOGIC,
    TOK_KW_ALWAYS_COMB, TOK_KW_ALWAYS_FF, TOK_KW_ALWAYS_LATCH,
    TOK_KW_INTERFACE, TOK_KW_ENDINTERFACE, TOK_KW_MODPORT,
    TOK_KW_PACKAGE, TOK_KW_ENDPACKAGE, TOK_KW_IMPORT,

    /* Identifiers and literals */
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,

    /* Multi-character operators */
    TOK_EQ,       /* == */
    TOK_NEQ,      /* != */
    TOK_CEQ,      /* === */
    TOK_CNEQ,     /* !== */
    TOK_LE,       /* <= */
    TOK_GE,       /* >= */
    TOK_SHL,      /* << */
    TOK_SHR,      /* >> */
    TOK_SSHL,     /* <<< */
    TOK_SSHR,     /* >>> */
    TOK_LAND,     /* && */
    TOK_LOR,      /* || */
    TOK_LNOT,     /* ! */
    TOK_POW,      /* ** */
    TOK_DBL_COLON, /* :: */

    /* Single-character operators / punctuation */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AND, TOK_OR, TOK_XOR, TOK_TILDE,
    TOK_LT, TOK_GT,
    TOK_QUESTION, TOK_COLON,
    TOK_COMMA, TOK_DOT, TOK_SEMI,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE,

    /* Special */
    TOK_ASSIGN,    /* =  (blocking / continuous assignment) */
    TOK_AT,         /* @  (sensitivity) */
    TOK_HASH,       /* #  (delay / parameter) */
} token_kind_t;

typedef struct {
    token_kind_t kind;
    const char *start;
    size_t length;
    uint64_t num_value;
    int num_width;
    int num_base;
} token_t;

typedef struct verilog_lexer verilog_lexer_t;

verilog_lexer_t *verilog_lexer_create(const char *input, size_t length);
void verilog_lexer_destroy(verilog_lexer_t *lexer);
token_t verilog_lexer_next(verilog_lexer_t *lexer);
token_t verilog_lexer_peek(verilog_lexer_t *lexer);
size_t verilog_lexer_position(verilog_lexer_t *lexer);
uint32_t verilog_lexer_line(verilog_lexer_t *lexer);
int verilog_lexer_match(verilog_lexer_t *lexer, token_kind_t kind);
int verilog_lexer_expect(verilog_lexer_t *lexer, token_kind_t kind);

#endif /* LIBDSIM_VERILOG_LEXER_H */
