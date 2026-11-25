/* Ark C Compiler - lexer
 * Lexical analyzer for the C99 source language.
 *
 * Features:
 *   - C99 keywords including _Bool/inline/restrict.
 *   - Numeric literals: octal 0755, hex 0xFF, decimal with suffixes u/U/l/L.
 *   - Floating literals: 1.5, 1.5e-3, .5, 1.5f.
 *   - Escape sequences in char/string literals: \n \t \r \\ \' \" \0 \xNN \NNN \a \b \f \v.
 *   - Wide char/string literals with the L prefix.
 *   - Line comments // and block comments.
 *   - Operators: . / -> / ...
 */
#include "lexer.h"
#include "diag.h"
#include "memory.h"
#include "platform.h"
#include "config.h"
#include "arkcc_config.h"
#include "type.h"


/* All source access goes through the reader cache; no token points into it. */
static void lex_error(Lexer *lex, const char *message) {
    DiagLoc loc;
    if (lex->error) return;
    loc.file = 0; loc.line = lex->line; loc.col = lex->column;
    lex->error = ARK_ERR_LEX;
    diag_report_msg(DIAG_SEV_ERROR, &loc, message, 0, 0, 0);
}
static int peek(Lexer *lex, uint32_t n) {
    unsigned char c;
    int rc;
    if (lex->error || n >= lex->source->size - lex->offset) return 0;
    rc = xh_source_read_byte(lex->source, lex->offset + n, &c);
    if (rc < 0) { lex_error(lex, "source reader I/O failure"); lex->error = ARK_ERR_IO; return 0; }
    if (rc == XH_SOURCE_OK && !c) lex_error(lex, "NUL byte in source");
    return rc == XH_SOURCE_OK ? c : 0;
}
static void advance(Lexer *lex, uint32_t n) {
    while (n--) {
        int c = peek(lex, 0);
        if (!c) return;
        lex->offset++;
        if (lex->token_active && ++lex->raw_token_bytes > XH_LEX_TOKEN_MAX) {
            lex_error(lex, "token exceeds 4096 byte limit");
            return;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r' || !lex->previous_cr) lex->line++;
            lex->column = 1; lex->line_start = lex->offset;
            lex->line_blank = 1; lex->utf8_remaining = 0;
        } else {
            if (lex->utf8_remaining && (c & 0xc0) == 0x80) lex->utf8_remaining--;
            else {
                lex->utf8_remaining = 0;
                if ((c & 0xe0) == 0xc0) { lex->column++; lex->utf8_remaining=1; }
                else if ((c & 0xf0) == 0xe0) { lex->column+=2; lex->utf8_remaining=2; }
                else if ((c & 0xf8) == 0xf0) { lex->column+=2; lex->utf8_remaining=3; }
                else lex->column++;
            }
            if (c != ' ' && c != '\t') lex->line_blank = 0;
        }
        lex->previous_cr = c == '\r';
    }
}
static int is_alpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_digit(int c) { return c>='0'&&c<='9'; }
static int is_alnum(int c) { return is_alpha(c)||is_digit(c); }
static int is_hex(int c) { return is_digit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static int hex_val(int c) { return is_digit(c)?c-'0':(c>='a'?c-'a'+10:c-'A'+10); }
static int is_ident_start(int c) {
    if (is_alpha(c) || c >= 128) return 1;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    if (c=='$' && g_config.dollar_in_identifiers) return 1;
#endif
    return 0;
}
static int is_ident_char(int c) { return is_ident_start(c)||is_digit(c); }
static Token make_token(TokenKind kind, int line) {
    Token t = {0}; t.kind=kind; t.line=line; return t;
}
static Token make_number(long long value, int line) {
    Token t=make_token(TOK_NUM,line);t.value=value;return t;
}
static void append(Lexer *lex, int c) {
    if (lex->token_length >= XH_LEX_TOKEN_MAX) { lex_error(lex,"token exceeds 4096 byte limit"); return; }
    lex->scratch[lex->token_length++] = (char)c;
    lex->scratch[lex->token_length] = 0;
    if (lex->token_length > lex->peak_token_bytes) lex->peak_token_bytes=lex->token_length;
}
static void take(Lexer *lex) { int c=peek(lex,0); if(c) { append(lex,c); advance(lex,1); } }
static void skip_ws(Lexer *lex) {
    for (;;) {
        int c=peek(lex,0);
        if (c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f') advance(lex,1);
        else if(c=='/' && peek(lex,1)=='/') { while(peek(lex,0)&&peek(lex,0)!='\n'&&peek(lex,0)!='\r') advance(lex,1); }
        else if(c=='/' && peek(lex,1)=='*') {
            advance(lex,2);
            while(peek(lex,0)&&!(peek(lex,0)=='*'&&peek(lex,1)=='/')) advance(lex,1);
            if(peek(lex,0)) advance(lex,2); else lex_error(lex,"unterminated comment");
        } else return;
    }
}
static int escape(Lexer *lex) {
    int c=peek(lex,0), v=0, n=0;
    if(c=='x') { advance(lex,1);while(is_hex(peek(lex,0))){v=(v*16+hex_val(peek(lex,0)))&255;advance(lex,1);}return v; }
    if(c>='0'&&c<='7') { while(n++<3&&peek(lex,0)>='0'&&peek(lex,0)<='7'){v=v*8+peek(lex,0)-'0';advance(lex,1);}return v&255; }
    advance(lex,1);
    switch(c) {case 'n':return '\n';case 'r':return '\r';case 't':return '\t';case 'a':return 7;case 'b':return 8;case 'f':return 12;case 'v':return 11;default:return c;}
}
static void ucn(Lexer *lex) {
    unsigned int cp=0;int i,n=peek(lex,1)=='u'?4:8;
    advance(lex,2);
    for(i=0;i<n&&is_hex(peek(lex,0));i++){cp=cp*16+(unsigned)hex_val(peek(lex,0));advance(lex,1);}
    if(i!=n || cp>0x10ffffu || (cp>=0xd800u&&cp<=0xdfffu)){lex_error(lex,"invalid universal character name");return;}
    if(cp<128) append(lex,(int)cp);
    else if(cp<2048){append(lex,0xc0|(cp>>6));append(lex,0x80|(cp&63));}
    else if(cp<65536){append(lex,0xe0|(cp>>12));append(lex,0x80|((cp>>6)&63));append(lex,0x80|(cp&63));}
    else {append(lex,0xf0|(cp>>18));append(lex,0x80|((cp>>12)&63));append(lex,0x80|((cp>>6)&63));append(lex,0x80|(cp&63));}
}
/* Keyword table */
typedef struct { const char *s; TokenKind k; } Keyword;

static Keyword g_keywords[] = {
    /* C89 */
    {"int",      TOK_INT},
    {"char",     TOK_CHAR},
    {"void",     TOK_VOID},
    {"return",   TOK_RETURN},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"do",       TOK_DO},
    /* C99 stdint keywords */
    {"short",    TOK_SHORT},
    {"long",     TOK_LONG},
    {"signed",   TOK_SIGNED},
    {"unsigned", TOK_UNSIGNED},
    {"float",    TOK_FLOAT},
    {"double",   TOK_DOUBLE},
    {"_Bool",    TOK_BOOL},
    {"const",    TOK_CONST},
    {"volatile", TOK_VOLATILE},
    {"restrict", TOK_RESTRICT},
    {"inline",   TOK_INLINE},
    {"static",   TOK_STATIC},
    {"extern",   TOK_EXTERN},
    {"auto",     TOK_AUTO},
    {"register", TOK_REGISTER},
    {"struct",   TOK_STRUCT},
    {"union",    TOK_UNION},
    {"enum",     TOK_ENUM},
    {"typedef",  TOK_TYPEDEF},
    {"sizeof",   TOK_SIZEOF},
    /* C11 keywords */
    {"_Alignof",       TOK_ALIGNOF},
    {"_Alignas",       TOK_ALIGNAS},
    {"_Static_assert", TOK_STATIC_ASSERT},
    {"_Generic",       TOK_GENERIC},
    {"switch",   TOK_SWITCH},
    {"case",     TOK_CASE},
    {"default",  TOK_DEFAULT},
    {"goto",     TOK_GOTO},
    /* 0.3.0 GNU extended keywords */
    {"__attribute__", TOK_ATTRIBUTE},
    {"asm",           TOK_ASM},
    {"__asm",         TOK_ASM},
    {"__asm__",       TOK_ASM},
    {"__inline",      TOK_INLINE},
    {"__inline__",    TOK_INLINE},
    {"__volatile__",  TOK_VOLATILE},
    {"__const",       TOK_CONST},
    {"__restrict",    TOK_RESTRICT},
    {"__svc",         TOK_SVC},     /* ARM Compiler 5 SVC instruction keyword */
    {"__irq",         TOK_IRQ},     /* ARM Compiler 5 interrupt handler keyword */
    {"__weak",        TOK_WEAK},    /* ARM Compiler 5 weak symbol keyword */
    {"__naked",       TOK_NAKED},   /* ARM Compiler 5 naked function keyword */
    {"__packed",      TOK_PACKED_KW},  /* ARM Compiler 5 packed attribute keyword */
    {"__align",       TOK_ALIGN_KW},   /* ARM Compiler 5 alignment keyword */
    {"__at",          TOK_AT},         /* ARM Compiler 5 absolute address placement */
    {"__softfp",      TOK_SOFTFP},     /* ARM Compiler 5 software floating-point ABI */
    {"__forceinline", TOK_FORCEINLINE},/* ARM Compiler 5 force-inline keyword */
    /* B4: _Complex support (C99) */
    {"_Complex",      TOK_COMPLEX},
    {"__complex__",   TOK_COMPLEX},
    /* C11 6.7.2.4: thread-local storage.  Unconditional -- this is a standard
     * keyword, not an armcc extension, so it must work with the armcc
     * keyword set turned off (it maps onto the same storage class as
     * __thread, which the backends already handle). */
    {"_Thread_local", TOK_THREAD_LOCAL},
        {"_Atomic",       TOK_ATOMIC},
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* 0.6.x: armcc 5 specific keywords; map to internal token kinds. */
    {"__value_in_regs", TOK_VALUE_IN_REGS},
    {"__thread",        TOK_THREAD},
    {"__global_reg",    TOK_GLOBAL_REG},
    {"__pure",          TOK_PURE},
    {"__interwork",     TOK_INTERWORK},
    {"__smc",           TOK_SMC},
    {"__int64",         TOK_INT64},
    {"__int32",         TOK_INT32},
    {"__int16",         TOK_INT16},
    {"__int8",          TOK_INT8},
    {"__typedef",       TOK_TYPEDEF_KW},
    {"__args",          TOK_ARGS},
    {"__return",        TOK_RETURN_KW},
    {"__tp_soft",       TOK_TP_SOFT},
    {"__nothrow",       TOK_NOTHROW},
    {"__deprecated",    TOK_DEPRECATED_KW},
    {"__may_alias",     TOK_MAY_ALIAS},
    {"__declspec",      TOK_DECLSPEC},   /* MSVC-style attribute keyword */
#endif
    /* __asm is treated as TOK_ASM; the parser handles the operand syntax for
     * both basic and extended asm forms, including armcc-style
     * __asm void fn(){...} naked function bodies. */
    {0, TOK_EOF}
};

static int kw_eq(const char *s, int len, const char *kw) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] != kw[i]) return 0;
        if (kw[i] == 0) return 0;
    }
    return kw[len] == 0;
}

static int check_keyword(const char *s, int len) {
    int i;
    for (i = 0; g_keywords[i].s; i++) {
        if (kw_eq(s, len, g_keywords[i].s)) return (int)g_keywords[i].k;
    }
    return -1;
}


static Token lexer_read_token(Lexer *lex) {
    int line,c,wide=0;
    Token tk;
    skip_ws(lex);
    /* Handed-through directives are bounded independently of source lines. */
    while(peek(lex,0)=='#' && lex->line_blank) {
        uint32_t i;
        lex->token_length=0;lex->scratch[0]=0;
        while(peek(lex,0)&&peek(lex,0)!='\n'&&peek(lex,0)!='\r') take(lex);
        i=1;while(lex->scratch[i]==' '||lex->scratch[i]=='\t')i++;
        if(lex->token_length >= i+6 && lex->scratch[i]=='p' && lex->scratch[i+1]=='r' && lex->scratch[i+2]=='a' && lex->scratch[i+3]=='g' && lex->scratch[i+4]=='m' && lex->scratch[i+5]=='a' && !is_alnum(lex->scratch[i+6])) {
            i+=6;while(lex->scratch[i]==' '||lex->scratch[i]=='\t')i++;
            if(lex->token_length >= i+4 && lex->scratch[i]=='p' && lex->scratch[i+1]=='a' && lex->scratch[i+2]=='c' && lex->scratch[i+3]=='k' && !is_alnum(lex->scratch[i+4])) type_pragma_pack(lex->scratch+i+4);
        }
        skip_ws(lex);
    }
    lex->token_start=lex->offset;
    lex->token_active=1;lex->raw_token_bytes=0;
    lex->token_column=lex->column;
    lex->token_length=0;lex->scratch[0]=0;
    line=lex->line;c=peek(lex,0);
    if(!c)return make_token(TOK_EOF,line);
#if ARKCC_FEATURE_WCHAR
    if(g_config.enable_wchar && c=='L' && (peek(lex,1)=='"'||peek(lex,1)=='\'')){wide=1;advance(lex,1);c=peek(lex,0);}
#endif
    if(is_ident_start(c)||(c=='\\'&&(peek(lex,1)=='u'||peek(lex,1)=='U'))) {
        int kw;
        while(is_ident_char(peek(lex,0))||(peek(lex,0)=='\\'&&(peek(lex,1)=='u'||peek(lex,1)=='U'))) {
            if(peek(lex,0)=='\\')ucn(lex);else take(lex);
        }
        kw=check_keyword(lex->scratch,(int)lex->token_length);
        tk=make_token(kw<0?TOK_IDENT:(TokenKind)kw,line);
        if(kw<0)tk.str=mem_strndup(lex->scratch,(int)lex->token_length);
        return tk;
    }
    if(is_digit(c)||(c=='.'&&is_digit(peek(lex,1)))) {
        int hex=c=='0'&&(peek(lex,1)=='x'||peek(lex,1)=='X');
        int floating=0,has_digits=0,bad=0,float_suffix=0;
        uint32_t core_end,i;
        if(hex){take(lex);take(lex);}
        while(hex?is_hex(peek(lex,0)):is_digit(peek(lex,0))){has_digits=1;take(lex);}
        if(peek(lex,0)=='.') {
            floating=1;take(lex);
            while(hex?is_hex(peek(lex,0)):is_digit(peek(lex,0))){has_digits=1;take(lex);}
        }
        if(hex?(peek(lex,0)=='p'||peek(lex,0)=='P'):(peek(lex,0)=='e'||peek(lex,0)=='E')) {
            floating=1;take(lex);
            if(peek(lex,0)=='+'||peek(lex,0)=='-')take(lex);
            if(!is_digit(peek(lex,0)))bad=1;
            while(is_digit(peek(lex,0)))take(lex);
        } else if(hex&&floating)bad=1;
        core_end=lex->token_length;
        if(!has_digits)bad=1;
        if(floating) {
            char *end;double value;
            /* Bounded, complete and NUL terminated: libc never sees a reader window. */
            float_suffix=peek(lex,0)=='f'||peek(lex,0)=='F';
#if !defined(ARKCC_STM32)
            {
                extern double strtod(const char *,char **);
                extern float strtof(const char *,char **);
                value=float_suffix?(double)strtof(lex->scratch,&end):strtod(lex->scratch,&end);
                if(end!=lex->scratch+core_end)bad=1;
            }
#else
            /* Preserve the freestanding target's arithmetic fallback. */
            {
                double mantissa=0.0,scale=1.0;
                int fraction=0,negative=0,exponent=0,radix=hex?16:10;
                end=lex->scratch+(hex?2:0);
                while(end<lex->scratch+core_end && ((hex?is_hex(*end):is_digit(*end))||*end=='.')) {
                    if(*end=='.')fraction=1;
                    else {mantissa=mantissa*radix+(hex?hex_val(*end):*end-'0');if(fraction)scale*=radix;}
                    end++;
                }
                if(end<lex->scratch+core_end) {
                    end++;if(*end=='+'||*end=='-'){negative=*end=='-';end++;}
                    while(end<lex->scratch+core_end){if(exponent<100000)exponent=exponent*10+(*end-'0');end++;}
                }
                value=mantissa/scale;
                if(exponent){extern double pow(double,double);value*=pow(hex?2.0:10.0,negative?-(double)exponent:(double)exponent);}
                if(float_suffix)value=(double)(float)value;
            }
#endif
            if(float_suffix||peek(lex,0)=='l'||peek(lex,0)=='L')take(lex);
            tk=make_token(TOK_FCONST,line);tk.fvalue=value;tk.is_float=float_suffix;
            if(peek(lex,0)=='i'||peek(lex,0)=='I'||peek(lex,0)=='j'||peek(lex,0)=='J'){tk.is_imag=1;take(lex);}
        } else {
            unsigned long long v=0,limit=~(unsigned long long)0;
            int radix=hex?16:(c=='0'?8:10),overflow=0;
            tk=make_number(0,line);tk.lit_dec=radix==10;
            for(i=hex?2u:0u;i<core_end;i++) {
                unsigned d=(unsigned)(hex?hex_val(lex->scratch[i]):lex->scratch[i]-'0');
                if(d>=(unsigned)radix){bad=1;continue;}
                if(v>(limit-d)/(unsigned)radix)overflow=1;
                v=v*(unsigned)radix+d;
            }
            tk.value=(long long)v;
            while(peek(lex,0)=='u'||peek(lex,0)=='U'||peek(lex,0)=='l'||peek(lex,0)=='L') {
                if(peek(lex,0)=='u'||peek(lex,0)=='U'){tk.lit_unsigned=1;take(lex);}
                else {take(lex);if(peek(lex,0)=='l'||peek(lex,0)=='L'){tk.lit_llong=1;take(lex);}else tk.lit_long=1;}
            }
            if(overflow)lex_error(lex,"integer literal exceeds 64-bit range");
        }
        if(is_ident_char(peek(lex,0))){bad=1;while(is_ident_char(peek(lex,0)))take(lex);}
        if(bad)lex_error(lex,floating?"invalid floating literal":"invalid integer literal");
        return tk;
    }
    if(c=='\'' || c=='"') {
        int quote=c,val=0;
        tk=make_token(quote=='"'?TOK_STR:TOK_NUM,line);tk.value=wide;
        for(;;) {
            advance(lex,1);
            while(peek(lex,0)&&peek(lex,0)!=quote) {
                int ch;
                if(peek(lex,0)=='\\') {
                    if(quote=='"'&&(peek(lex,1)=='u'||peek(lex,1)=='U')){ucn(lex);continue;}
                    advance(lex,1);ch=escape(lex);
                } else {ch=peek(lex,0);advance(lex,1);}
                if(quote=='"')append(lex,ch);else {val=ch;break;}
            }
            if(peek(lex,0)==quote)advance(lex,1);else lex_error(lex,"unterminated literal");
            if(quote!='"'||wide)break;
            lex->token_active=0;
            skip_ws(lex);
            lex->token_active=1;
            if(peek(lex,0)!='"')break;
        }
        if(quote=='"')tk.str=mem_strndup(lex->scratch,(int)lex->token_length);
        else tk.value=val;
        return tk;
    }
    advance(lex, 1);
    switch (c) {
        case '(': return make_token(TOK_LPAREN, line);
        case ')': return make_token(TOK_RPAREN, line);
        case '{': return make_token(TOK_LBRACE, line);
        case '}': return make_token(TOK_RBRACE, line);
        case '[': return make_token(TOK_LBRACKET, line);
        case ']': return make_token(TOK_RBRACKET, line);
        case ';': return make_token(TOK_SEMICOLON, line);
        case ',': return make_token(TOK_COMMA, line);
        case '?': return make_token(TOK_QUESTION, line);
        case ':': return make_token(TOK_COLON, line);
        case '#': return make_token(TOK_HASH, line);
        case '~': return make_token(TOK_BNOT, line);
        case '.':
            if (peek(lex, 0) == '.' && peek(lex, 1) == '.') {
                advance(lex, 2);
                return make_token(TOK_ELLIPSIS, line);
            }
            return make_token(TOK_DOT, line);
        case '^':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_XOR_ASSIGN, line); }
            return make_token(TOK_BXOR, line);
        case '+':
            if (peek(lex, 0) == '+') { advance(lex, 1); return make_token(TOK_INC, line); }
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_ADD_ASSIGN, line); }
            return make_token(TOK_PLUS, line);
        case '-':
            if (peek(lex, 0) == '-') { advance(lex, 1); return make_token(TOK_DEC, line); }
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_SUB_ASSIGN, line); }
            if (peek(lex, 0) == '>') { advance(lex, 1); return make_token(TOK_ARROW, line); }
            return make_token(TOK_MINUS, line);
        case '*':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_MUL_ASSIGN, line); }
            return make_token(TOK_MUL, line);
        case '/':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_DIV_ASSIGN, line); }
            return make_token(TOK_DIV, line);
        case '%':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_MOD_ASSIGN, line); }
            return make_token(TOK_MOD, line);
        case '=':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_EQ, line); }
            return make_token(TOK_ASSIGN, line);
        case '!':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_NE, line); }
            return make_token(TOK_NOT, line);
        case '<':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_LE, line); }
            if (peek(lex, 0) == '<') {
                advance(lex, 1);
                if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_SHL_ASSIGN, line); }
                return make_token(TOK_SHL, line);
            }
            return make_token(TOK_LT, line);
        case '>':
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_GE, line); }
            if (peek(lex, 0) == '>') {
                advance(lex, 1);
                if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_SHR_ASSIGN, line); }
                return make_token(TOK_SHR, line);
            }
            return make_token(TOK_GT, line);
        case '&':
            if (peek(lex, 0) == '&') { advance(lex, 1); return make_token(TOK_AND, line); }
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_AND_ASSIGN, line); }
            return make_token(TOK_BAND, line);
        case '|':
            if (peek(lex, 0) == '|') { advance(lex, 1); return make_token(TOK_OR, line); }
            if (peek(lex, 0) == '=') { advance(lex, 1); return make_token(TOK_OR_ASSIGN, line); }
            return make_token(TOK_BOR, line);
        default:
            /* armcc reports this as #7 "unrecognized token" with the caret on
             * the offending character and no extra detail, so we do the same
             * instead of leaking a hex code. */
            {
                DiagLoc loc;
                loc.file = 0;
                loc.line = line;
                loc.col = lex->column;
                diag_report(7, DIAG_SEV_ERROR, &loc, 0, 0, 0);
                lex->error = ARK_ERR_LEX;
            }
            return make_token(TOK_EOF, line);
    }
}

static Token lexer_read_token_with_pos(Lexer *lex) {
    Token t=lexer_read_token(lex);
    lex->token_active=0;
    t.src_offset=lex->token_start;
    t.col=lex->token_column;
    /* Compute token's position from a small saved position, never a line rescan. */
    if(lex->memory_input)t.src_start=lex->memory_input+t.src_offset;
    else if(t.str)t.src_start=t.str;
    else t.src_start="";
    return t;
}
static Token positioned(Lexer *lex) {
    return lexer_read_token_with_pos(lex);
}
int lexer_init_source(Lexer *lex,XhSourceReader *source) {
    if(!lex)return ARK_ERR_IO;
    if(!source||!source->range||source->error) {
        lex->source=source;lex->memory_input=0;lex->error=ARK_ERR_IO;
        lex->offset=0;lex->line=1;lex->column=1;lex->last_line=0;
        lex->current=make_token(TOK_EOF,1);lex->ahead=lex->current;
        lex->lookahead_tokens=2;
        return ARK_ERR_IO;
    }
    lex->source=source;lex->memory_input=0;
    lex->offset=0;lex->line_start=0;lex->line=1;lex->column=1;
    lex->line_blank=1;lex->previous_cr=0;lex->utf8_remaining=0;
    lex->error=0;lex->last_line=0;lex->token_length=0;lex->peak_token_bytes=0;
    lex->token_active=0;lex->raw_token_bytes=0;
    lex->token_start=0;lex->lookahead_tokens=2;
    lex->current=positioned(lex);lex->ahead=positioned(lex);
    return lex->error?lex->error:ARK_OK;
}
static int memory_range(void *ctx,uint32_t off,unsigned char *buf,uint32_t count) {
    const unsigned char *src=(const unsigned char *)ctx;uint32_t i;
    for(i=0;i<count;i++)buf[i]=src[off+i];return (int)count;
}
void lexer_init(Lexer *lex,const char *input) {
    uint32_t size=0;while(input[size])size++;
    xh_source_init(memory_range,(void *)input,size,lex->memory_window,sizeof(lex->memory_window),&lex->memory_source);
    lexer_init_source(lex,&lex->memory_source);
    lex->memory_input=input;
    lex->current.src_start=input+lex->current.src_offset;
    lex->ahead.src_start=input+lex->ahead.src_offset;
}
char *lexer_copy_source_span(Lexer *lex,uint32_t start,uint32_t length) {
    char *out;uint32_t i;unsigned char c;
    if(length>XH_LEX_ASM_MAX||start>lex->source->size||length>lex->source->size-start){lex_error(lex,"asm body exceeds bounded span limit");return 0;}
    out=(char *)mem_pool_alloc((int)length+1);
    for(i=0;i<length;i++){
        if(xh_source_read_byte(lex->source,start+i,&c)!=XH_SOURCE_OK){lex_error(lex,"source reader I/O failure");lex->error=ARK_ERR_IO;return 0;}
        out[i]=(char)c;
    }
    out[length]=0;return out;
}
Token lexer_peek(Lexer *lex){return lex->current;}
Token lexer_peek2(Lexer *lex){return lex->ahead;}
Token lexer_next(Lexer *lex){Token t=lex->current;lex->current=lex->ahead;lex->ahead=positioned(lex);if(t.kind!=TOK_EOF)lex->last_line=t.line;return t;}
void lexer_advance(Lexer *lex){(void)lexer_next(lex);}
/* Spelling of the token a caller expected, so the message reads like
 * "expected a \";\"" instead of exposing the TokenKind enum. */
static const char *expect_name(TokenKind k) {
    switch (k) {
        case TOK_SEMICOLON: return "a \";\"";
        case TOK_RPAREN:    return "a \")\"";
        case TOK_RBRACE:    return "a \"}\"";
        case TOK_RBRACKET:  return "a \"]\"";
        case TOK_LBRACE:    return "a \"{\"";
        case TOK_LPAREN:    return "a \"(\"";
        case TOK_IDENT:     return "an identifier";
        default:            return "a token";
    }
}

/* armcc codes for the "expected a X" family, measured one by one:
 *   int a[2;        -> #17 expected a "]"
 *   f(1, 2;         -> #18 expected a ")"
 *   int a = ;       -> #29 expected an expression
 *   missing ;       -> #65 expected a ";"
 *   unclosed {      -> #67 expected a "}"  (positioned at the EOF token)
 * `{`/`(` and identifiers have no measured code, so they keep the arkc
 * wording. */
static int expect_code(TokenKind k) {
    switch (k) {
        case TOK_SEMICOLON: return 65;
        case TOK_RPAREN:    return 18;
        case TOK_RBRACE:    return 67;
        case TOK_RBRACKET:  return 17;
        default:            return DIAG_NO_CODE;
    }
}

int lexer_expect(Lexer *lex, TokenKind kind) {
    if (lex->current.kind != kind) {
        DiagLoc loc;
        int code;
        loc.file = 0;
        loc.line = lex->current.line;
        loc.col  = lex->current.col;
        code = expect_code(kind);
        if (lex->current.kind == TOK_EOF) {
            /* Nothing left to point at: armcc anchors these at the line of the
             * last real token and adds "At end of source" instead of echoing
             * the line. */
            /* armcc anchors the message on the line the final newline
             * terminates, which is one before the EOF token (measured on
             * 1-, 3- and 5-line files). */
            if (loc.line > 1) {
                loc.line--;
            } else if (lex->last_line > 0) {
                loc.line = lex->last_line;
            }
            loc.col = 0;
            if (code >= 0) diag_report_at_eof(code, DIAG_SEV_ERROR, &loc);
            else diag_report_msg(DIAG_SEV_ERROR, &loc, "expected %s",
                                 expect_name(kind), 0, 0);
            return 0;
        }
        if (code >= 0) {
            diag_report(code, DIAG_SEV_ERROR, &loc, 0, 0, 0);
        } else {
            diag_report_msg(DIAG_SEV_ERROR, &loc, "expected %s",
                            expect_name(kind), 0, 0);
        }
        return 0;
    }
    lexer_advance(lex);
    return 1;
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "ident";
        case TOK_NUM: return "num";
        case TOK_STR: return "str";
        case TOK_FCONST: return "fconst";
        default: return "tok";
    }
}
