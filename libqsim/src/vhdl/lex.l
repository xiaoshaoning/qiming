/* TODO: VHDL Flex lexer — IEEE 1076-2008 subset */
%option reentrant noyywrap
%%
"entity"    return TOK_ENTITY;
"end"       return TOK_END;
"architecture" return TOK_ARCH;
"of"        return TOK_OF;
"is"        return TOK_IS;
"port"      return TOK_PORT;
"signal"    return TOK_SIGNAL;
"variable"  return TOK_VARIABLE;
"process"   return TOK_PROCESS;
"begin"     return TOK_BEGIN;
"if"        return TOK_IF;
"else"      return TOK_ELSE;
"elsif"     return TOK_ELSIF;
"case"      return TOK_CASE;
"when"      return TOK_WHEN;
"loop"      return TOK_LOOP;
"for"       return TOK_FOR;
"in"        return TOK_IN;
"to"        return TOK_TO;
"downto"    return TOK_DOWNTO;
"std_logic" return TOK_STD_LOGIC;
"std_logic_vector" return TOK_STD_LOGIC_VECTOR;
"in"        return TOK_IN;
"out"       return TOK_OUT;
"inout"     return TOK_INOUT;
"map"       return TOK_MAP;
"generic"   return TOK_GENERIC;
[ \t\r\n]+  /* skip whitespace */
"--"[^\n]*  /* skip line comment */
[a-zA-Z][a-zA-Z0-9_]* yylval->str = strdup(yytext); return TOK_ID;
[0-9]+      yylval->num = strtoul(yytext, NULL, 10); return TOK_NUM;
.           return yytext[0];
%%
