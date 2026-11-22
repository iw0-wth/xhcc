/* RV32 adapter for the shared external finite template VM. */
#include "xh_rv_rules.h"
#include "xh_template.h"
#include <stdio.h>
void xh_rv_rule_four(AsmBuffer*b,const char*key,int a,int c,int d,int e){char words[4][32];const char*args[4];int values[4]={a,c,d,e},i;for(i=0;i<4;i++){snprintf(words[i],sizeof(words[i]),"%d",values[i]);args[i]=words[i];}xh_rv_rule_args(b,key,args,4);}
static int failed;
void xh_rv_rules_reset(void){failed=0;}
const char*xh_rv_rule_text(const char*key){const char*s=xh_template_get("riscv32",key);if(!s){failed=1;return "";}return s;}
int xh_rv_rules_failed(void){return failed;}
void xh_rv_rule_args(AsmBuffer*b,const char*key,const char*const*args,int count){char text[4096];int n;if(failed)return;n=xh_template_emit("riscv32",key,args,count,text,sizeof(text));if(n<0){failed=1;return;}asmbuf_puts(b,text);}
void xh_rv_rule(AsmBuffer*b,const char*key){xh_rv_rule_args(b,key,0,0);}
void xh_rv_rule_int(AsmBuffer*b,const char*key,int value){char number[32];const char*args[1];snprintf(number,sizeof(number),"%d",value);args[0]=number;xh_rv_rule_args(b,key,args,1);}
