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
#define XH_LD_MAX_EVENTS 176
#define XH_LD_MAX_ASSERTIONS 32
#define XH_LD_MESSAGE 128
#define XH_LD_NAME 64
#define XH_LD_NONE (-1)
enum { XH_LD_VALUE, XH_LD_DOT, XH_LD_SYMBOL, XH_LD_ADD, XH_LD_SUB,
       XH_LD_MUL, XH_LD_DIV, XH_LD_NEG, XH_LD_ALIGN, XH_LD_ORIGIN,
       XH_LD_LENGTH, XH_LD_ADDR, XH_LD_LOADADDR, XH_LD_SIZEOF,
       XH_LD_EQ, XH_LD_NE, XH_LD_LT, XH_LD_LE, XH_LD_GT, XH_LD_GE,
       XH_LD_AND, XH_LD_OR, XH_LD_NOT };
enum { XH_LD_PATTERN, XH_LD_DEFINE, XH_LD_SET_DOT, XH_LD_ASSERT };
typedef struct { char name[XH_LD_NAME]; uint32_t origin,length; unsigned attrs; int origin_expr,length_expr; } XhLdRegion;
typedef struct { char name[XH_LD_NAME]; int region,load_region,noload,address_expr,align_expr,load_address_expr; unsigned first_event,event_count; } XhLdSection;
typedef struct { char name[XH_LD_NAME]; unsigned keep,common; } XhLdPattern;
typedef struct { char name[XH_LD_NAME]; int expr,section; unsigned event; } XhLdSymbol;
typedef struct { int kind,left,right; uint32_t value; char name[XH_LD_NAME]; } XhLdExpr;
typedef struct { int kind,index,section; } XhLdEvent;
typedef struct { int expr; char message[XH_LD_MESSAGE]; int section; unsigned event; } XhLdAssertion;
typedef struct {
    char entry[XH_LD_NAME];
    unsigned region_count,section_count,pattern_count,symbol_count,expr_count,event_count,assertion_count;
    XhLdRegion regions[XH_LD_MAX_REGIONS];
    XhLdSection sections[XH_LD_MAX_SECTIONS];
    XhLdPattern patterns[XH_LD_MAX_PATTERNS];
    XhLdSymbol symbols[XH_LD_MAX_SYMBOLS];
    XhLdExpr exprs[XH_LD_MAX_EXPRS];
    XhLdEvent events[XH_LD_MAX_EVENTS];
    XhLdAssertion assertions[XH_LD_MAX_ASSERTIONS];
} XhLdScript;
/* Parse clears caller-owned out; no heap allocation or retained source pointers.
 * Returns 0 or -1 with a bounded diagnostic. All unknown syntax is rejected.
 * Names <64 bytes, text <=64KiB, expression nesting <=16, 128 AST nodes total.
 * Expressions are checked unsigned 32-bit arithmetic (including intermediate
 * results). Regions must resolve during parse, possibly using earlier regions.
 * Alloc sections select >region; omitted AT uses its VMA region. Header
 * AT(expr) selects an exact LMA (evaluated at aligned VMA start), mutually
 * exclusive with trailing AT>region. Consumers require its entire file span
 * inside one declared MEMORY region, regardless of region attributes.
 * region==XH_LD_NONE denotes a checked, ignored metadata declaration: explicit
 * constant address zero, pure patterns, no load/align/type modifiers. Consumers
 * MUST check every matching input is nonalloc SHT_ARM_ATTRIBUTES (0x70000003).
 * Such declarations do not advance dot/cursors or create load segments; input
 * attributes were already ignored and are NOT merged/preserved in output ELF.
 * Events retain script order. Match patterns in order, each input at most once;
 * KEEP is retained for consumers with GC. COMMON is a separate input class.
 * DEFINE evaluates at its event's dot; deferred address expressions may require
 * a second symbol pass after layout. ASSERT events capture their source dot;
 * consumers MUST evaluate all assertions after layout/symbol resolution, rejecting
 * false or unresolved expressions with the assertion message before output.
 * Comparisons yield 0/1; &&/|| short-circuit. Message text <128 bytes, quoted
 * with supported escapes \", \\, n, r, t. Never silently discard unmatched inputs. */
int xh_ld_script_parse(const char *text,size_t length,XhLdScript *out,char *err,size_t errcap);
/* lookup returns 0 on success, -1 for unresolved; kind is SYMBOL, ADDR,
 * LOADADDR or SIZEOF. It must not silently substitute zero. ALIGN(n) aligns dot.
 * expr==XH_LD_NONE is invalid. Evaluator does not mutate the IR. */
typedef int (*XhLdLookup)(void *user,int kind,const char *name,uint32_t *value);
int xh_ld_script_eval(const XhLdScript *script,int expr,uint32_t dot,
                     XhLdLookup lookup,void *user,uint32_t *value,char *err,size_t errcap);
int xh_ld_script_match(const XhLdPattern *pattern,const char *name,int common);
#endif
