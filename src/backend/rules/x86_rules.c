/* x86 adapter for the shared bounded package template VM. Only one target's
 * rules and a small result ring are resident. No instruction rules live here. */
#include "xh_x86_rules.h"
#include "xh_template.h"
#include "xh_semantic.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
static const char *target = "x86";
static int failed, cursor;
static int wide_mode;
static char (*results)[4096];
int xh_x86_rules_begin(int mode64) {
    wide_mode=mode64;
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
        /* These values match xhcc IR argument packing and package stdarg.
         * Aggregate sizes describe xhcc's legacy private wire, not the full
         * native SysV eightbyte classification used by external compilers.
         * Reject a package/IR mismatch before any object can be emitted. */
        {
            static const char *keys[]={"abi.ir.word","abi.aggregate.direct_limit","abi.aggregate.sret_alignment","abi.frame.alignment","abi.va.gp_bytes","abi.va.fp_stride","abi.va.save_bytes","abi.frame.spill_slots","abi.aggregate.class.count","abi.aggregate.class.0.maximum","abi.aggregate.class.0.wire","abi.aggregate.class.1.maximum","abi.aggregate.class.1.wire","abi.aggregate.class.2.maximum"};
            static const int values[]={4,8,8,16,48,16,176,64,3,4,4,8,8,65535};
            int i;
            for(i=0;i<(int)(sizeof(values)/sizeof(values[0]));i++)
                if(xh_x86_number(keys[i])!=values[i])failed=1;
            if(xh_x86_number("abi.aggregate.class.2.wire")!=(mode64?8:4))failed=1;
            if(xh_x86_number("abi.stack.word")!=(mode64?8:4) ||
               xh_x86_number("abi.stack.argument_base")!=(mode64?16:8))failed=1;
            if(xh_x86_number("abi.frame.locals_gap")!=48 ||
               xh_x86_number("abi.frame.saved_bytes")!=(mode64?0:12) ||
               xh_x86_number("abi.frame.sret_pointer_offset")!=200 ||
               xh_x86_number("abi.frame.call_scratch_gap")!=16 ||
               xh_x86_number("abi.frame.div_scratch_offset")!=496 ||
               xh_x86_number("abi.frame.scratch_bytes")!=512 ||
               xh_x86_number("abi.frame.minimum_locals")!=16 ||
               xh_x86_number("abi.call.max_arguments")!=64 ||
               xh_x86_number("abi.call.max_words")!=116 ||
               xh_x86_number("abi.call.i386_argument_limit")!=120 ||
               xh_x86_number("abi.frame.multiword_scratch_base")!=24)failed=1;
            if(failed)diag_tool(DIAG_SEV_ERROR,target,"x86 ABI layout rules disagree with the supported IR and stdarg contract",0,0,0,0,0,0);
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
const char *xh_x86_semantic(const char *family,const char *operation,int width,const char *sign,const char *form,int pop,int push,const char *const *args,int count){
    XhSemanticInput input;XhSemanticResult result;
    input.family=family;input.operation=operation;input.width=width;input.sign=sign;input.form=form;
    if(xh_semantic_select(target,"semantic.fp",&input,&result)!=1 || result.pop!=pop || result.push!=push){failed=1;diag_tool(DIAG_SEV_ERROR,target,"missing or incompatible floating semantic rule",0,0,0,0,0,0);return "";}
    return xh_x86_expand(result.template_key,args,count);
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

int xh_x86_aggregate_wire(int bytes) {
    int count=xh_x86_number("abi.aggregate.class.count"),i;char key[80];
    if(bytes<=0)bytes=1;
    if(count<1 || count>8){failed=1;return 0;}
    for(i=0;i<count;i++){
        int limit;
        snprintf(key,sizeof(key),"abi.aggregate.class.%d.maximum",i);limit=xh_x86_number(key);
        if(bytes<=limit){snprintf(key,sizeof(key),"abi.aggregate.class.%d.wire",i);return xh_x86_number(key);}
    }
    failed=1;return wide_mode?8:4;
}

/* Finite first-match decision records. No expressions, jumps or executable
 * templates. Every lookup is bounded by 32 records and 16 numeric fields. */
int xh_x86_classify(int result,int kind,int size,int alignment,int variadic,int index,XhX86AbiClass *out){
    int count=xh_x86_number("abi.decision.count"),i,j,matched=0;char key[64];
    if(!out || count<1 || count>32 || kind<0 || kind>4 || size<0 || size>65535 || alignment<1 || alignment>4096 || index<0 || index>120)goto bad;
    for(i=0;i<count;i++){
        int v[16];const char *text;char *end;
        snprintf(key,sizeof(key),"abi.decision.%d",i);text=xh_x86_rule(key);
        for(j=0;j<16;j++){long n;while(*text==' ')text++;n=strtol(text,&end,10);if(end==text || n<0 || n>65535)goto bad;v[j]=(int)n;text=end;}
        while(*text==' ')text++;if(*text || v[0]>1 || v[1]>4 || v[2]>v[3] || v[4]>v[5] || v[6]>v[7] || v[8]>v[9] || v[10]>2 || v[11]>2 || v[12]>8 || v[13]>16 || v[14]>1 || v[15])goto bad;
        if(v[0]!=result || v[1]!=kind || size<v[2] || size>v[3] || alignment<v[4] || alignment>v[5] || variadic<v[6] || variadic>v[7] || index<v[8] || index>v[9])continue;
        if(!matched){out->bank=v[10];out->words=v[11];out->width=v[12];out->alignment=v[13];out->indirect=v[14];matched=1;}
    }
    if(!matched)goto bad;
    /* Validate the supported IR/private-wire contract independently of the
     * external selector; incompatible class changes must not produce objects. */
    {
        int indirect=kind==XH_ABI_AGGREGATE && size>8;
        int words=kind==XH_ABI_VOID?0:indirect || kind==XH_ABI_POINTER?1:size>4?2:1;
        int width=kind==XH_ABI_VOID?0:indirect || kind==XH_ABI_POINTER?(wide_mode?8:4):size>4?8:4;
        int bank=kind==XH_ABI_VOID?2:kind==XH_ABI_FLOAT && (wide_mode || result)?1:wide_mode || result?0:2;
        if(out->indirect!=indirect || out->words!=words || out->width!=width || out->bank!=bank || out->alignment!=(kind==XH_ABI_VOID?1:wide_mode?8:4))goto bad;
    }
    return 0;
bad:failed=1;diag_tool(DIAG_SEV_ERROR,target,"ABI decision table is invalid, unmatched or incompatible with the supported private wire",0,0,0,0,0,0);if(out)memset(out,0,sizeof(*out));return -1;
}
int xh_x86_allocate(const XhX86AbiClass *value,int *integer_used,int *vector_used,int *stack_used,int *where){
    if(value->bank==0 && *integer_used<xh_x86_number("abi.integer.count")){*where=(*integer_used)++;return 0;}
    if(value->bank==1 && *vector_used<xh_x86_number("abi.vector.count")){*where=(*vector_used)++;return 1;}
    *where=(*stack_used)++;return 2;
}
