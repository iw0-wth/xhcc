/* Ark C Compiler - lexer.h
 * Lexical analysis: turns the source text into a Token stream.
 */
#ifndef ARK_LEXER_H
#define ARK_LEXER_H

#include "common.h"
#include "xh_source_reader.h"

#define XH_LEX_TOKEN_MAX 4096u
#define XH_LEX_ASM_MAX 65536u

typedef struct {
    TokenKind kind;
    long long       value;
    double    fvalue;
    char     *str;
    int       line;
    int       col;          /* 1-based display column of the token start, for
                             * IDE-style diagnostics and the caret. 0 = unknown
                             * (tokens not produced by lexer_next). */
    const char *src_start;  /* stable spelling; original address for memory sources */
    uint32_t src_offset;    /* absolute provider byte offset; never a window pointer */
    int       is_imag;      /* C99 imaginary literal (i/I/j/J suffix) */
    int       is_float;     /* C99 float suffix (f/F): 1 = float literal, 0 = double */
    /* Integer literal suffix flags (C99 6.4.4.1). Dropping them made
     * `-1 < 1u` compare as signed and gave 0xFFFFFFFFu / 1u<<31 the wrong
     * type, so the lexer now records them and the parser types the literal. */
    int       lit_unsigned;
    int       lit_long;
    int       lit_llong;
    /* 1 = decimal spelling, 0 = octal/hexadecimal.  C99 6.4.4.1 gives the
     * two spellings different candidate lists: a decimal constant may not
     * become unsigned until an explicit `u` is given, while 0xFFFFFFFF is
     * unsigned int before it is long. */
    int       lit_dec;
} Token;

typedef struct {
    XhSourceReader *source; /* borrowed: caller keeps reader/path/window alive */
    XhSourceReader memory_source;
    const char *memory_input; /* only legacy lexer_init's immutable input */
    unsigned char memory_window[XH_SOURCE_WINDOW_DEFAULT];
    uint32_t offset, line_start;
    int line, column, line_blank, previous_cr, utf8_remaining;
    int error;
    uint32_t token_start, token_length, peak_token_bytes;
    int token_column, token_active;
    uint32_t raw_token_bytes;
    unsigned int lookahead_tokens; /* always two after initialization */
    char scratch[XH_LEX_TOKEN_MAX + 1u];
    Token       current;
    Token       ahead;
    int         last_line;  /* line of the last non-EOF token; an end-of-source
                             * diagnostic is anchored there (armcc does the
                             * same, the EOF token itself sits one line later
                             * whenever the file ends with a newline) */
} Lexer;

void  lexer_init(Lexer *lex, const char *input);
int   lexer_init_source(Lexer *lex, XhSourceReader *source);
/* Arena-owned NUL terminated span; rejects lengths greater than ASM_MAX. */
char *lexer_copy_source_span(Lexer *lex, uint32_t start, uint32_t length);
Token lexer_next(Lexer *lex);
Token lexer_peek(Lexer *lex);
Token lexer_peek2(Lexer *lex);
void  lexer_advance(Lexer *lex);
int   lexer_expect(Lexer *lex, TokenKind kind);

const char *token_kind_name(TokenKind k);

#endif /* ARK_LEXER_H */
