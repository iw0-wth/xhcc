/* RV32 adapter for the shared external finite template VM. */
#include "xh_rv_rules.h"
#include "xh_template.h"
#include "xh_semantic.h"
#include "xh_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
void xh_rv_rule_four(AsmBuffer*b,const char*key,int a,int c,int d,int e){char words[4][32];const char*args[4];int values[4]={a,c,d,e},i;for(i=0;i<4;i++){snprintf(words[i],sizeof(words[i]),"%d",values[i]);args[i]=words[i];}xh_rv_rule_args(b,key,args,4);}
static int failed;
static XhRvAbi policy;
static int scalar_words,wide_words,indirect_words;
static int number_rule(const char*key,int*out);
static int register_token(const char*s);
typedef struct{char name[24],rule[32],shape[16];} RInstruction;
static RInstruction *r_instructions;static int r_instruction_count;
const char*xh_rv_r_encoding(const char*name){int i;for(i=0;i<r_instruction_count;i++)if(!strcmp(r_instructions[i].name,name)&&!strcmp(r_instructions[i].shape,"rr_r"))return r_instructions[i].rule;return 0;}
const char*xh_rv_i_encoding(const char*name,int*shift_amount){int i;for(i=0;i<r_instruction_count;i++)if(!strcmp(r_instructions[i].name,name)&&(!strcmp(r_instructions[i].shape,"rr_i")||!strcmp(r_instructions[i].shape,"rr_shamt"))){*shift_amount=!strcmp(r_instructions[i].shape,"rr_shamt");return r_instructions[i].rule;}return 0;}
const char*xh_rv_encoding_shape(const char*name,const char*shape){int i;for(i=0;i<r_instruction_count;i++)if(!strcmp(r_instructions[i].name,name)&&!strcmp(r_instructions[i].shape,shape))return r_instructions[i].rule;return 0;}
static int reserved_mnemonic(const char*name){static const char*reserved[]={"li","la","call","mv","ret","nop","j","jr","beqz","bnez","blez","bgez","bltz","bgtz","bgt","ble","bgtu","bleu","seqz","snez","not","neg","ecall","ebreak","fence"};unsigned i;for(i=0;i<sizeof(reserved)/sizeof(reserved[0]);i++)if(!strcmp(name,reserved[i]))return 1;return 0;}
int xh_rv_mnemonics_load(void){const char*source=xh_rv_rule_text("isa.mnemonics.r");char*copy,*line;int capacity=0;free(r_instructions);r_instructions=0;r_instruction_count=0;if(!source||!*source||strlen(source)>16384)goto bad;for(line=(char*)source;*line;){capacity++;line=strchr(line,'\n');if(!line)break;line++;}if(capacity>64)goto bad;r_instructions=calloc((size_t)capacity,sizeof(*r_instructions));copy=malloc(strlen(source)+1);if(!r_instructions||!copy){free(copy);goto bad;}strcpy(copy,source);line=copy;while(*line){char*end=strchr(line,'\n'),name[24],rule[32],shape[16],extra;int i,args[3]={0,0,0};unsigned word;if(end)*end=0;if(*line){if(sscanf(line,"%23s %31s %15s %c",name,rule,shape,&extra)!=3||!register_token(name)||reserved_mnemonic(name)||(strcmp(shape,"rr_r")&&strcmp(shape,"rr_i")&&strcmp(shape,"rr_shamt")&&strcmp(shape,"rd_mem")&&strcmp(shape,"rs_mem")&&strcmp(shape,"rs_rs_label")&&strcmp(shape,"rd_imm20")&&strcmp(shape,"rd_label")&&strcmp(shape,"rd_jumpmem"))||r_instruction_count==capacity){free(copy);goto bad;}for(i=0;i<r_instruction_count;i++)if(!strcmp(r_instructions[i].name,name)){free(copy);goto bad;}if(xh_rules_encode(rule,args,3,&word)){free(copy);goto bad;}strcpy(r_instructions[r_instruction_count].name,name);strcpy(r_instructions[r_instruction_count].rule,rule);strcpy(r_instructions[r_instruction_count++].shape,shape);}if(!end)break;line=end+1;}free(copy);if(!r_instruction_count)goto bad;return 0;
 bad:free(r_instructions);r_instructions=0;r_instruction_count=0;failed=1;diag_tool(DIAG_SEV_ERROR,"riscv32 mnemonics","invalid instruction descriptor, reserved mnemonic or encoding contract",0,0,0,0,0,0);return -1;}
static struct {char name[24];int number;} aliases[64];
static int alias_count,register_count;static char argument_registers[8][24];
static int register_token(const char*s){int n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'))return 0;return n<24;}
int xh_rv_register_number(const char*name){int i;for(i=0;i<alias_count;i++)if(!strcmp(aliases[i].name,name))return aliases[i].number;return -1;}
int xh_rv_register_count(void){return register_count;}
const char*xh_rv_argument_register(int i){if(i<0||i>=8){failed=1;return "";}return argument_registers[i];}
int xh_rv_registers_load(void){const char*source=xh_rv_rule_text("isa.register.aliases");char*copy,*line;int i,count;if(number_rule("isa.register_count",&count)||count!=32||!source||strlen(source)>4096)goto bad;register_count=count;alias_count=0;copy=malloc(strlen(source)+1);if(!copy)goto bad;strcpy(copy,source);line=copy;while(*line){char*end=strchr(line,'\n'),name[24],extra;int number;if(end)*end=0;if(*line){if(sscanf(line,"%23s %d %c",name,&number,&extra)!=2||!register_token(name)||number<0||number>=register_count||alias_count==64||xh_rv_register_number(name)>=0){free(copy);goto bad;}strcpy(aliases[alias_count].name,name);aliases[alias_count++].number=number;}if(!end)break;line=end+1;}free(copy);
 {static const char*required[]={"zero","ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2","a3","a4","a5","a6","a7","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6"};for(i=0;i<32;i++)if(xh_rv_register_number(required[i])!=i)goto bad;if(xh_rv_register_number("fp")!=8)goto bad;}
 for(i=0;i<8;i++){char key[48];const char*name;snprintf(key,sizeof(key),"abi.arg_register.%d",i);name=xh_rv_rule_text(key);if(!register_token(name)||xh_rv_register_number(name)!=10+i)goto bad;strcpy(argument_registers[i],name);}return 0;
 bad:failed=1;diag_tool(DIAG_SEV_ERROR,"riscv32 registers","invalid aliases or incompatible register ABI contract",0,0,0,0,0,0);return -1;}
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
