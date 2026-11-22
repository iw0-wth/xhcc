/* ARM adapter for the shared external text-template VM; no built-in rules. */
#include "xh_arm_rules.h"
#include "xh_template.h"
#include <stdio.h>
#include "diag.h"
static int failed;
static XhArmAbi abi;
static int abi_loaded;
static int abi_record(const char *key,int *values,int count){
    char text[192],*p;int n,i=0,v;
    n=xh_template_emit("arm",key,0,0,text,sizeof text);if(n<0)return -1;
    p=text;while(*p){while(*p==' '||*p=='\t')p++;if(!*p)break;v=0;if(*p<'0'||*p>'9'||i==count)return -1;while(*p>='0'&&*p<='9'){v=v*10+(*p++-'0');if(v>4096)return -1;}values[i++]=v;if(*p&&*p!=' '&&*p!='\t')return -1;}return i==count?0:-1;
}
const XhArmAbi *xh_arm_abi(void){
    int layout[7],small[2],wide[2],indirect[3];
    if(abi_loaded)return &abi;
    if(abi_record("abi.layout",layout,7)||abi_record("abi.scalar32",small,2)||abi_record("abi.scalar64",wide,2)||abi_record("abi.indirect",indirect,3)||
       layout[0]!=4||layout[1]!=4||layout[2]!=2||layout[3]!=8||layout[4]!=8||layout[5]!=16||layout[6]!=1||
       small[0]!=4||small[1]!=4||wide[0]!=8||wide[1]!=8||indirect[0]!=8||indirect[1]!=4||indirect[2]!=4){
        failed=1;diag_tool(DIAG_SEV_ERROR,"arm ABI rules","invalid or unsupported AAPCS base/softfp descriptor",0,0,0,0,0,0);
        /* Fill the supported contract only to keep error unwinding bounded;
         * sticky failure rejects the compilation and never emits fallback output. */
        abi.word_bytes=4;abi.core_regs=4;abi.pair_words=2;abi.pair_align=8;abi.stack_align=8;abi.saved_frame=16;abi.sret_words=1;
        abi.scalar32_size=4;abi.scalar32_align=4;abi.scalar64_size=8;abi.scalar64_align=8;abi.indirect_limit=8;abi.indirect_size=4;abi.indirect_align=4;return &abi;
    }
    abi.word_bytes=layout[0];abi.core_regs=layout[1];abi.pair_words=layout[2];abi.pair_align=layout[3];abi.stack_align=layout[4];abi.saved_frame=layout[5];abi.sret_words=layout[6];
    abi.scalar32_size=small[0];abi.scalar32_align=small[1];abi.scalar64_size=wide[0];abi.scalar64_align=wide[1];abi.indirect_limit=indirect[0];abi.indirect_size=indirect[1];abi.indirect_align=indirect[2];abi_loaded=1;return &abi;
}
/* Ordered records: form mode maxRt base index wb maxShift min max align.
 * form 1=REG16 2=SP16 3=PC16 4=IMM16 5=REG32 6=WB32 7=POS32 8=NEG32.
 * Base 0=low 1=SP 2=PC 3=any. Mode0=immediate1=register. */
int xh_arm_mem_form(int kind,int is_load,int rt,int rn,int rm,int imm,int wb,int shift){
    char key[64],text[2048],*p;int n,form,mode,maxrt,base,index,wantwb,maxshift,lo,hi,align,used;
    snprintf(key,sizeof key,"mem.policy.%s.%s",is_load?"L":"S",kind==0?"W":kind==1?"B":kind==2?"H":kind==3?"SB":"SH");
    n=xh_template_emit("arm",key,0,0,text,sizeof text);if(n<0){failed=1;return -1;}
    p=text;while(*p){used=0;
        if(sscanf(p,"%d %d %d %d %d %d %d %d %d %d %n",&form,&mode,&maxrt,&base,&index,&wantwb,&maxshift,&lo,&hi,&align,&used)!=10||used<1||form<1||form>8||mode<0||mode>1||maxrt<0||maxrt>15||base<0||base>3||index<0||index>1||wantwb<0||wantwb>1||maxshift<0||maxshift>3||lo>hi||align<1||(align&(align-1))){failed=1;return -1;}
        p+=used;
        if((rm>=0)!=mode||rt<0||rt>maxrt||rn<0||rn>15||wb!=wantwb||shift>maxshift)continue;
        if((base==0&&rn>7)||(base==1&&rn!=13)||(base==2&&rn!=15))continue;
        if(mode&&((index==0&&rm>7)||rm>15))continue;
        if(imm<lo||imm>hi||(imm&(align-1)))continue;
        /* Final architectural guards keep external policy variants semantic:
         * a table may prefer a wide form, never truncate an illegal narrow one. */
        if(form==1&&(!mode||rt>7||rn>7||rm>7||shift||wb))goto bad_form;
        if(form==2&&(kind!=0||mode||rt>7||rn!=13||wb||imm<0||imm>1020||(imm&3)))goto bad_form;
        if(form==3&&(kind!=0||!is_load||mode||rt>7||rn!=15||wb||imm<0||imm>1020||(imm&3)))goto bad_form;
        if(form==4&&(mode||wb||kind>2||rt>7||rn>7||imm<0||imm>(kind==0?124:kind==1?31:62)||(imm&((kind==0?4:kind==1?1:2)-1))))goto bad_form;
        if(form==5&&(!mode||wb||shift<0||shift>3))goto bad_form;
        if(form==6&&(mode||!wb||imm< -255||imm>255))goto bad_form;
        if(form==7&&(mode||wb||imm<0||imm>4095))goto bad_form;
        if(form==8&&(mode||wb||imm< -255||imm>=0))goto bad_form;
        return form;
        bad_form:failed=1;diag_tool(DIAG_SEV_ERROR,"arm memory policy","selected memory form violates architectural constraints",0,0,0,0,0,0);return -1;
    }failed=1;return -1;
}

void xh_arm_rules_reset(void){failed=0;abi_loaded=0;}
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
