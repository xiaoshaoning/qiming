/* TODO: Verilog Flex lexer — IEEE 1364-2005 subset */
%option reentrant noyywrap
%%
"module"    return TOK_MODULE;
"endmodule" return TOK_ENDMODULE;
"input"     return TOK_INPUT;
"output"    return TOK_OUTPUT;
"wire"      return TOK_WIRE;
"reg"       return TOK_REG;
"assign"    return TOK_ASSIGN;
"always"    return TOK_ALWAYS;
"initial"   return TOK_INITIAL;
"begin"     return TOK_BEGIN;
"end"       return TOK_END;
"if"        return TOK_IF;
"else"      return TOK_ELSE;
"case"      return TOK_CASE;
"endcase"   return TOK_ENDCASE;
"for"       return TOK_FOR;
"generate"  return TOK_GENERATE;
"endgenerate" return TOK_ENDGENERATE;
"posedge"   return TOK_POSEDGE;
"negedge"   return TOK_NEGEDGE;
[ \t\r\n]+  /* skip whitespace */
"//"[^\n]*  /* skip line comment */
"/*"([^*]|\*[^/])*"*/" /* skip block comment */
[a-zA-Z_][a-zA-Z0-9_$]* yylval->str = strdup(yytext); return TOK_ID;
[0-9]+      yylval->num = strtoul(yytext, NULL, 10); return TOK_NUM;
.           return yytext[0];
%%
