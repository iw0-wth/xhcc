/* x86 adapter for the shared bounded package template VM. Only one target's
 * rules and a small result ring are resident. No instruction rules live here. */
#include "xh_x86_rules.h"
#include "xh_template.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
static const char *target = "x86";
static int failed, cursor;
static char (*results)[4096];
int xh_x86_rules_begin(int mode64) {
    target = mode64 ? "x86_64" : "x86";
    failed = 0;
    if (!results) results = malloc(8 * sizeof(*results));
    if (!results) { failed=1; return -1; }
    {
        int version=xh_x86_number("format.version");
        int integers=xh_x86_number("abi.integer.count");
        int vectors=xh_x86_number("abi.vector.count");
        /* Physical scratch arrays are bounded independently of package data. */
        if(version!=1 || integers!=(mode64?6:0) || vectors!=(mode64?8:0)) {
            failed=1;
            diag_tool(DIAG_SEV_ERROR,target,"x86 package register counts disagree with the supported frontend ABI",0,0,0,0,0,0);
        }
        return failed ? -1 : 0;
    }
}
const char *xh_x86_rule(const char *key) {
    char *out;
    const char *raw=xh_template_get(target,key);
    if (!raw) { failed=1; return ""; }
    /* Stable table pointers are needed by ABI register arrays. Expand only
     * templates containing dollar syntax; ordinary fragments need no copy. */
    if (!strchr(raw,'$')) return raw;
    if (!results) { failed=1; return ""; }
    out=results[(cursor++) & 7];
    if (xh_template_emit(target,key,0,0,out,4096)<0) {failed=1;out[0]=0;}
    return out;
}
const char *xh_x86_expand(const char *key,const char *const *args,int count) {
    char *out;
    if(!results){failed=1;return "";}
    out=results[(cursor++) & 7];
    if(xh_template_emit(target,key,args,count,out,4096)<0){failed=1;out[0]=0;}
    return out;
}
int xh_x86_number(const char *key) {
    const char *s=xh_x86_rule(key); char *end; long v=strtol(s,&end,0);
    if (!*s || *end || v<INT_MIN || v>INT_MAX) {
        failed=1;diag_tool(DIAG_SEV_ERROR,target,"invalid integer in x86 package rule %s",key,0,0,0,0,0);return 0;
    }
    return (int)v;
}
int xh_x86_register(const char *name,int *width) {
    char key[64],*end; const char *s;long code,w;
    if (strlen(name)>16) return -1;
    /* All legal names are enumerated externally. A list avoids missing-rule
     * diagnostics for identifiers and labels that are not registers. */
    s=xh_x86_rule("register.names");
    while (*s) {
        const char *start=s; size_t n;
        while(*s && *s!=' ')s++;n=(size_t)(s-start);
        if(strlen(name)==n && !strncmp(name,start,n))break;
        while(*s==' ')s++;
    }
    if(!*s)return -1;
    snprintf(key,sizeof(key),"register.%s",name);s=xh_x86_rule(key);
    code=strtol(s,&end,10);if(end==s || *end!=' ')goto bad;
    s=end+1;w=strtol(s,&end,10);
    if(*end || code<0 || code>15 || (w!=1&&w!=2&&w!=4&&w!=8&&w!=16))goto bad;
    *width=(int)w;return (int)code;
bad: failed=1;return -1;
}
int xh_x86_rules_failed(void){return failed;}
