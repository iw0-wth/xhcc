/* ARM adapter for the shared external text-template VM; no built-in rules. */
#include "xh_arm_rules.h"
#include "xh_template.h"
#include <stdio.h>
static int failed;
void xh_arm_rules_reset(void){failed=0;}
int xh_arm_rules_failed(void){return failed;}
void xh_arm_rule_args(AsmBuffer*b,const char*key,const char*const*args,int count){
    char out[4096];int n;
    if(failed)return;
    n=xh_template_emit("arm",key,args,count,out,sizeof(out));
    if(n<0){failed=1;return;}
    asmbuf_puts(b,out);
}
void xh_arm_rule(AsmBuffer*b,const char*key){xh_arm_rule_args(b,key,0,0);}
void xh_arm_rule_ints(AsmBuffer*b,const char*key,int a,int c,int d){
    char aa[24],cc[24],dd[24];const char*args[3];
    snprintf(aa,sizeof aa,"%d",a);snprintf(cc,sizeof cc,"%d",c);snprintf(dd,sizeof dd,"%d",d);
    args[0]=aa;args[1]=cc;args[2]=dd;xh_arm_rule_args(b,key,args,3);
}
