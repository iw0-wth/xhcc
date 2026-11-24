/* Independently implemented, bounded GNU ld script subset; not a GNU ld parser. */
#ifndef XH_LD_SCRIPT_H
#define XH_LD_SCRIPT_H
#include <stddef.h>
#include <stdint.h>
#define XH_LD_MAX_REGIONS 8
#define XH_LD_MAX_SECTIONS 16
#define XH_LD_MAX_PATTERNS 64
#define XH_LD_MAX_SYMBOLS 64
#define XH_LD_MAX_EXPRS 128
#define XH_LD_MAX_EVENTS 144
#define XH_LD_NAME 64
#define XH_LD_NONE (-1)
enum { XH_LD_VALUE, XH_LD_DOT, XH_LD_SYMBOL, XH_LD_ADD, XH_LD_SUB,
       XH_LD_MUL, XH_LD_DIV, XH_LD_NEG, XH_LD_ALIGN, XH_LD_ORIGIN,
       XH_LD_LENGTH, XH_LD_ADDR, XH_LD_LOADADDR, XH_LD_SIZEOF };
enum { XH_LD_PATTERN, XH_LD_DEFINE, XH_LD_SET_DOT };
typedef struct { char name[XH_LD_NAME]; uint32_t origin,length; unsigned attrs; int origin_expr,length_expr; } XhLdRegion;
typedef struct { char name[XH_LD_NAME]; int region,load_region,noload,address_expr,align_expr; unsigned first_event,event_count; } XhLdSection;
typedef struct { char name[XH_LD_NAME]; unsigned keep,common; } XhLdPattern;
typedef struct { char name[XH_LD_NAME]; int expr,section; unsigned event; } XhLdSymbol;
typedef struct { int kind,left,right; uint32_t value; char name[XH_LD_NAME]; } XhLdExpr;
typedef struct { int kind,index,section; } XhLdEvent;
typedef struct {
    char entry[XH_LD_NAME];
    unsigned region_count,section_count,pattern_count,symbol_count,expr_count,event_count;
    XhLdRegion regions[XH_LD_MAX_REGIONS];
    XhLdSection sections[XH_LD_MAX_SECTIONS];
    XhLdPattern patterns[XH_LD_MAX_PATTERNS];
    XhLdSymbol symbols[XH_LD_MAX_SYMBOLS];
    XhLdExpr exprs[XH_LD_MAX_EXPRS];
    XhLdEvent events[XH_LD_MAX_EVENTS];
} XhLdScript;
/* Parse clears caller-owned out; no heap allocation or retained source pointers.
 * Returns 0 or -1 with a bounded diagnostic. All unknown syntax is rejected.
 * Names <64 bytes, text <=64KiB, expression nesting <=16, 128 AST nodes total.
 * Expressions are checked unsigned 32-bit arithmetic (including intermediate
 * results). Regions must resolve during parse, possibly using earlier regions.
 * Every section must select >region; omitted AT> uses its VMA region.
 * Events retain script order. Match patterns in order, each input at most once;
 * KEEP is retained for consumers with GC. COMMON is a separate input class.
 * DEFINE evaluates at its event's dot; deferred address expressions may require
 * a second symbol pass after layout. Never silently discard unmatched inputs. */
int xh_ld_script_parse(const char *text,size_t length,XhLdScript *out,char *err,size_t errcap);
/* lookup returns 0 on success, -1 for unresolved; kind is SYMBOL, ADDR,
 * LOADADDR or SIZEOF. It must not silently substitute zero. ALIGN(n) aligns dot.
 * expr==XH_LD_NONE is invalid. Evaluator does not mutate the IR. */
typedef int (*XhLdLookup)(void *user,int kind,const char *name,uint32_t *value);
int xh_ld_script_eval(const XhLdScript *script,int expr,uint32_t dot,
                     XhLdLookup lookup,void *user,uint32_t *value,char *err,size_t errcap);
int xh_ld_script_match(const XhLdPattern *pattern,const char *name,int common);
#endif
