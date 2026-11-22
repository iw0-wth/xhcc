/* RV32 adapter for the shared external finite template VM. */
#include "xh_rv_rules.h"
#include "xh_template.h"
#include "xh_semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
void xh_rv_rule_four(AsmBuffer*b,const char*key,int a,int c,int d,int e){char words[4][32];const char*args[4];int values[4]={a,c,d,e},i;for(i=0;i<4;i++){snprintf(words[i],sizeof(words[i]),"%d",values[i]);args[i]=words[i];}xh_rv_rule_args(b,key,args,4);}
static int failed;
static XhRvAbi policy;
static int scalar_words,wide_words,indirect_words;
static int number_rule(const char*key,int*out){const char*s=xh_rv_rule_text(key);char*end;long n;if(!s||!*s)return -1;n=strtol(s,&end,10);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;if(*end||n<0||n>65536)return -1;*out=(int)n;return 0;}
int xh_rv_abi_load(void){int packed;if(number_rule("classify.scalar_words",&scalar_words)||number_rule("classify.wide_words",&wide_words)||number_rule("classify.indirect_words",&indirect_words)||scalar_words!=1||wide_words!=2||indirect_words!=1||number_rule("abi.word_bytes",&policy.word_bytes)||number_rule("abi.arg_registers",&policy.arg_registers)||number_rule("abi.stack_alignment",&policy.stack_alignment)||number_rule("abi.packed_words",&packed)||number_rule("frame.region_offset",&policy.region_offset)||number_rule("frame.saved_ra_offset",&policy.saved_ra_offset)||number_rule("frame.saved_s0_offset",&policy.saved_s0_offset)||number_rule("frame.local_base",&policy.local_base)||number_rule("frame.extra_padding",&policy.extra_padding)||number_rule("classify.indirect_threshold",&policy.indirect_threshold)||policy.word_bytes!=4||policy.arg_registers!=8||policy.stack_alignment!=16||packed!=1||policy.region_offset!=32||policy.saved_ra_offset!=36||policy.saved_s0_offset!=40||policy.local_base!=48||policy.indirect_threshold!=8||policy.extra_padding>256||(policy.extra_padding&15)){failed=1;diag_tool(DIAG_SEV_ERROR,"riscv32 ABI","external policy disagrees with supported packed ILP32 contract",0,0,0,0,0,0);return -1;}return 0;}
const XhRvAbi*xh_rv_abi(void){return &policy;}
int xh_rv_argument_words(int bytes,int aggregate){int words;if(bytes<1)bytes=policy.word_bytes;if(aggregate&&bytes>policy.indirect_threshold)return indirect_words;if(bytes<=policy.word_bytes)return scalar_words;if(bytes<=policy.indirect_threshold)return wide_words;words=(bytes+policy.word_bytes-1)/policy.word_bytes;return words;}
int xh_rv_align_stack(int bytes){return(bytes+policy.stack_alignment-1)&~(policy.stack_alignment-1);}
void xh_rv_rules_reset(void){failed=0;}
const char*xh_rv_rule_text(const char*key){const char*s=xh_template_get("riscv32",key);if(!s){failed=1;return "";}return s;}
int xh_rv_rules_failed(void){return failed;}
int xh_rv_semantic(AsmBuffer*b,const char*family,const char*op,int width,const char*sign,const char*form,int pop,int push,const char*const*args,int argc){XhSemanticInput input={family,op,sign,form,width};XhSemanticResult result;int n=xh_semantic_select("riscv32","semantic.records",&input,&result);if(n!=1||result.pop!=pop||result.push!=push){failed=1;diag_tool(DIAG_SEV_ERROR,"riscv32 semantic","missing record or incompatible stack effect",0,0,0,0,0,0);return -1;}xh_rv_rule_args(b,result.template_key,args,argc);return failed?-1:0;}
void xh_rv_rule_args(AsmBuffer*b,const char*key,const char*const*args,int count){char text[4096];int n;if(failed)return;n=xh_template_emit("riscv32",key,args,count,text,sizeof(text));if(n<0){failed=1;return;}asmbuf_puts(b,text);}
void xh_rv_rule(AsmBuffer*b,const char*key){xh_rv_rule_args(b,key,0,0);}
void xh_rv_rule_int(AsmBuffer*b,const char*key,int value){char number[32];const char*args[1];snprintf(number,sizeof(number),"%d",value);args[0]=number;xh_rv_rule_args(b,key,args,1);}
