#ifndef XH_ABI_H
#define XH_ABI_H
/* Finite first-match ABI relation, stored as prefix.count and prefix.N
 * XHTEMPLATE values. Each record has sixteen unsigned decimal fields:
 * role kind sizeLo sizeHi alignLo alignHi varLo varHi indexLo indexHi
 * bank words width wireAlignment indirect reserved(0).
 * v1 limits: role 0..1, kind 0..4, numeric fields 0..65535;
 * bank 0..2, words 0..2, width 0..8, wireAlignment 0..16,
 * indirect 0..1, reserved zero; ranges must be ascending. Input alignment
 * is 1..4096 and variadic is 0..1. These are a bounded v1 relation schema,
 * not a claim that every platform ABI can be expressed without extension.
 * No architecture, type traversal, allocation policy or expression evaluation. */
#define XH_ABI_MAX_RECORDS 32
#define XH_ABI_MAX_FIELDS 16
typedef struct {int role,kind,size,alignment,variadic,index;} XhAbiInput;
typedef struct {int bank,words,width,alignment,indirect;} XhAbiResult;
/* 1 matched; 0 unmatched; -1 invalid input/table. Clears output on 0/-1.
 * One actual-count cache, invalidated by target/prefix/template generation. */
int xh_abi_select(const char *target,const char *prefix,const XhAbiInput *input,XhAbiResult *output);
/* Frees only the parsed ABI cache. To observe edits to an already loaded file
 * at the same path, call xh_template_release() then xh_abi_release(). Path
 * changes and target switches invalidate automatically through generation. */
void xh_abi_release(void);
#endif
