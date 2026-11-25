/* Restricted ARMv6-M scalar legalization, independently implemented.
 * All target sequences are package templates; this module matches bounded
 * instruction shapes and rejects everything outside its documented subset. */
#include "xh_arm_thumb1.h"
#include "xh_arm_rules.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int error(const char*s){diag_tool(DIAG_SEV_ERROR,"Thumb-1 legalizer","unsupported instruction or operand in initial scalar subset: %s",s,0,0,0,0,0);return -1;}
static void line(AsmBuffer*b,const char*s){asmbuf_puts(b,"    ");asmbuf_puts(b,s);asmbuf_putc(b,'\n');}
static void number(AsmBuffer*b,int r,unsigned v){char rr[16],bb[4][16];const char*args[5];int i;snprintf(rr,sizeof rr,"%d",r);args[0]=rr;for(i=0;i<4;i++){snprintf(bb[i],sizeof bb[i],"%u",(v>>(24-8*i))&255u);args[i+1]=bb[i];}xh_arm_rule_args(b,"t1.constant",args,5);}
int xh_arm_thumb1_legalize(const char*input,AsmBuffer*out){
 const char*p=input;char s[512],next[512],op[32];int id=0;
 asmbuf_init(out);xh_arm_rule(out,"t1.header");
 while(*p){int n=0,r,a,b,v,used=0;const char*q;while(*p&&*p!='\n'){if(n==511)return error("line exceeds 511 bytes");s[n++]=*p++;}if(*p)p++;s[n]=0;q=s;while(*q==' '||*q=='\t')q++;
  if(!*q||*q=='@'){asmbuf_puts(out,s);asmbuf_putc(out,'\n');continue;}
  if(*q=='.'){if(!strncmp(q,".arch ",6)){asmbuf_puts(out,".cpu cortex-m0\n");continue;}if(!strncmp(q,".fpu",4)||!strncmp(q,".L_flt",6)||!strncmp(q,".L_dbl",6)||!strncmp(q,".L_cplx",7))return error(q);asmbuf_puts(out,s);asmbuf_putc(out,'\n');continue;}
  if(strchr(q,':')){asmbuf_puts(out,s);asmbuf_putc(out,'\n');continue;}
  if(sscanf(q,"MOVW r%d, #%d%n",&r,&v,&used)==2&&q[used]==0){
   unsigned value=(unsigned)v&65535u;const char*save=p;int nn=0;while(*save&&*save!='\n'&&nn<511)next[nn++]=*save++;next[nn]=0;
   if(sscanf(next," MOVT r%d, #%d",&a,&b)==2&&a==r){value|=((unsigned)b&65535u)<<16;p=*save?save+1:save;}
   if(r<0||r>7)return error(q);number(out,r,value);continue;
  }
  if(sscanf(q,"ITE %31s",op)==1){char yes[32],no[32],label[48];const char*save=p;int nn=0;while(*save&&*save!='\n'&&nn<511)next[nn++]=*save++;next[nn]=0;if(sscanf(next," MOV%31s",yes)!=1)return error(q);p=*save?save+1:save;nn=0;save=p;while(*save&&*save!='\n'&&nn<511)next[nn++]=*save++;next[nn]=0;if(sscanf(next," MOV%31s",no)!=1)return error(q);p=*save?save+1:save;
   snprintf(label,sizeof label,".L_t1cmp_%d",id++);{const char*args[2]={op,label};xh_arm_rule_args(out,"t1.compare",args,2);}continue;
  }
  if(sscanf(q,"%31s",op)!=1)return error(q);
  if(!strcmp(op,"BL")||!strcmp(op,"B")||!strcmp(op,"BEQ")||!strcmp(op,"BNE")||!strcmp(op,"BLT")||!strcmp(op,"BGT")||!strcmp(op,"BLE")||!strcmp(op,"BGE")||!strcmp(op,"BLO")||!strcmp(op,"BHI")||!strcmp(op,"BLS")||!strcmp(op,"BHS")){line(out,q);continue;}
  if(!strcmp(op,"PUSH")||!strcmp(op,"POP")){if(strstr(q,"lr")&&!strcmp(op,"POP"))return error(q);if(strstr(q,"r8")||strstr(q,"r9")||strstr(q,"r10")||strstr(q,"r11")||strstr(q,"r12"))return error(q);line(out,q);continue;}
  if(!strcmp(op,"BX")){if(strcmp(q,"BX lr"))return error(q);line(out,q);continue;}
  if(!strcmp(op,"NOP")){line(out,q);continue;}
  if(sscanf(q,"%31s sp, sp, #%d",op,&v)==2&&(!strcmp(op,"ADD")||!strcmp(op,"SUB"))){if(v<0||v>4096||(v&3))return error(q);while(v){int piece=v>508?508:v;char x[64];snprintf(x,sizeof x,"%s sp, sp, #%d",op,piece);line(out,x);v-=piece;}continue;}
  if(!strcmp(op,"MOV")){if(sscanf(q,"MOV r%d, #%d",&r,&v)==2){if(r>7||r<0)return error(q);if(v<0||v>255)number(out,r,(unsigned)v);else{char x[64];snprintf(x,sizeof x,"MOVS r%d, #%d",r,v);line(out,x);}continue;}if(!strcmp(q,"MOV r6, sp")||!strcmp(q,"MOV r4, sp")||!strcmp(q,"MOV r5, sp")||!strcmp(q,"MOV sp, r6")||!strcmp(q,"MOV sp, r4")||!strcmp(q,"MOV sp, r5")){line(out,q);continue;}if(sscanf(q,"MOV r%d, r%d",&r,&a)==2&&r>=0&&r<=7&&a>=0&&a<=7){line(out,q);continue;}return error(q);}
  if(!strcmp(op,"LDR")||!strcmp(op,"STR")||!strcmp(op,"LDRB")||!strcmp(op,"STRB")||!strcmp(op,"LDRH")||!strcmp(op,"STRH")){
   char base[16];int count=sscanf(q,"%31s r%d, [%15[^],], #%d]",op,&r,base,&v);if(count<3||r<0||r>7||strchr(q,'=')||strchr(q,'!'))return error(q);if(count==3)v=0;
   if(!strcmp(base,"sp")){if(strcmp(op,"LDR")&&strcmp(op,"STR"))return error(q);if(v<0||v>1020||(v&3))return error(q);}
   else {if(sscanf(base,"r%d",&a)!=1||a<0||a>7)return error(q);int scale=!strcmp(op,"LDR")||!strcmp(op,"STR")?4:(!strcmp(op,"LDRH")||!strcmp(op,"STRH")?2:1);if(v<0||v>31*scale||(v&(scale-1)))return error(q);}
   line(out,q);continue;
  }
  if(!strcmp(op,"CMP")){if(sscanf(q,"CMP r%d, #%d",&r,&v)==2){if(r<0||r>7||v<0||v>255)return error(q);}else if(sscanf(q,"CMP r%d, r%d",&r,&a)!=2||r<0||r>7||a<0||a>7)return error(q);line(out,q);continue;}
  if(!strcmp(op,"ADD")||!strcmp(op,"SUB")||!strcmp(op,"ADDS")||!strcmp(op,"SUBS")){
   char x[96];if(sscanf(q,"%31s r%d, r%d, #%d",op,&r,&a,&v)==4){if(r<0||r>7||a<0||a>7||v<0||v>255||(r!=a&&v>7))return error(q);if(r==a)snprintf(x,sizeof x,"%s r%d, #%d",op[0]=='A'?"ADDS":"SUBS",r,v);else snprintf(x,sizeof x,"%s r%d, r%d, #%d",op[0]=='A'?"ADDS":"SUBS",r,a,v);}
   else if(sscanf(q,"%31s r%d, r%d, r%d",op,&r,&a,&b)==4){if(r<0||r>7||a<0||a>7||b<0||b>7)return error(q);snprintf(x,sizeof x,"%s r%d, r%d, r%d",op[0]=='A'?"ADDS":"SUBS",r,a,b);}else if(sscanf(q,"%31s r%d, #%d",op,&r,&v)==3&&r>=0&&r<=7&&v>=0&&v<=255)snprintf(x,sizeof x,"%s r%d, #%d",op[0]=='A'?"ADDS":"SUBS",r,v);else return error(q);line(out,x);continue;
  }
  if(!strcmp(op,"LSLS")||!strcmp(op,"LSRS")||!strcmp(op,"ASRS")){if(sscanf(q,"%31s r%d, r%d, #%d",op,&r,&a,&v)!=4||r<0||r>7||a<0||a>7||v<0||v>31)return error(q);line(out,q);continue;}
  if(!strcmp(op,"RSBS")){if(sscanf(q,"RSBS r%d, r%d, #%d",&r,&a,&v)!=3||r<0||r>7||a<0||a>7||v!=0)return error(q);line(out,q);continue;}
  if(!strcmp(op,"MOVS")){if(sscanf(q,"MOVS r%d, #%d",&r,&v)!=2||r<0||r>7||v<0||v>255)return error(q);line(out,q);continue;}
  if(!strcmp(op,"AND")||!strcmp(op,"ORR")||!strcmp(op,"EOR")||!strcmp(op,"MUL")||!strcmp(op,"LSL")||!strcmp(op,"LSR")||!strcmp(op,"ASR")||!strcmp(op,"MVN")||!strcmp(op,"NEG")){
   char x[96];if(sscanf(q,"%31s r%d, r%d",op,&r,&a)!=3||r<0||r>7||a<0||a>7)return error(q);if(strchr(q,'#'))return error(q);
   snprintf(x,sizeof x,"%s%s r%d, r%d",!strcmp(op,"NEG")?"RSB":op,!strcmp(op,"NEG")?"S":"S",r,a);if(!strcmp(op,"NEG"))snprintf(x,sizeof x,"RSBS r%d, r%d, #0",r,a);line(out,x);continue;
  }
  return error(q);
 }return xh_arm_rules_failed()?-1:0;
}
