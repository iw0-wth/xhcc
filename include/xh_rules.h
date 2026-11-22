#ifndef XH_RULES_H
#define XH_RULES_H
/* Bounded 32-bit field encoding VM v1. Rules select no C/IR operations and
 * execute no native code. Host/package supplies one external rules file. */
#define XH_RULE_MAX_ARGS 8
#define XH_RULE_MAX_RULES 32
#define XH_RULE_MAX_FIELDS 32
#define XH_RULE_MAX_BYTES 32768
/* A new path invalidates the single cached rule set. NULL selects the default
 * RV32 package path. Strings are copied; callers retain no lifetime obligation. */
int xh_rules_set_path(const char *path);
const char *xh_rules_path(void);
void xh_rules_release(void);
/* Load/validate lazily. Input order and constraints are defined by the file.
 * Returns 0 on success, -1 with a diagnostic on malformed/unknown rules,
 * missing files, overflow, invalid arguments, range or alignment errors. */
int xh_rules_encode(const char *rule,const int *args,int count,unsigned *word);
/* Validate without encoding (used before an assembler pass). */
int xh_rules_load(void);
/* Scalar32 IR selection VM. A matched record emits one constrained three-
 * register mnemonic. 1=selected, 0=unmatched (caller retains C fallback), -1=error. */
int xh_rules_set_selection_path(const char *path);
int xh_rules_select_binary(const char *operation,char *output,int capacity);
#endif
