/* Independent ARM ELF32 REL byte decoding. Arithmetic, range assertions and
 * writes execute in the shared bounded relocation VM using ELF_* records. */
#include "xh_arm_elf_reloc.h"
#include "xh_relocation.h"
static unsigned u16(const unsigned char*p){return (unsigned)p[0]|((unsigned)p[1]<<8);}
static int64_t signed_bits(uint32_t v,unsigned bits){uint32_t sign=(uint32_t)1u<<(bits-1);return (v&sign)?(int64_t)v-((int64_t)1<<bits):(int64_t)v;}
int xh_arm_elf_rel_decode(unsigned type,const unsigned char*code,size_t length,size_t site,int64_t*addend,unsigned*width){
    unsigned hi,lo;uint32_t imm;int64_t a;
    if(!code||!addend||!width||site>length||length-site<4)return -1;
    hi=u16(code+site);lo=u16(code+site+2);
    switch(type){
    case 2:a=signed_bits((uint32_t)hi|((uint32_t)lo<<16),32);break;
    case 10:case 30:{
        unsigned sign,i1,i2;
        if((site&1)||(hi&0xf800)!=0xf000||(lo&0xd000)!=(type==10?0xd000:0x9000))return -1;
        sign=(hi>>10)&1;i1=(~(((lo>>13)&1)^sign))&1;i2=(~(((lo>>11)&1)^sign))&1;
        imm=(sign<<24)|(i1<<23)|(i2<<22)|((hi&1023)<<12)|((lo&2047)<<1);
        a=signed_bits(imm,25);break;
    }
    case 51:
        if((site&1)||(hi&0xf800)!=0xf000||(lo&0xd000)!=0x8000||((hi>>6)&15)>=14)return -1;
        imm=(((hi>>10)&1)<<20)|(((lo>>13)&1)<<19)|(((lo>>11)&1)<<18)|((hi&63)<<12)|((lo&2047)<<1);
        a=signed_bits(imm,21);break;
    case 47:case 48:
        if((site&1)||(hi&0xfbf0)!=(type==47?0xf240:0xf2c0)||(lo&0x8000)||((lo>>8)&15)==15)return -1;
        imm=((hi&15)<<12)|(((hi>>10)&1)<<11)|(((lo>>12)&7)<<8)|(lo&255);
        /* AAELF REL addend is the sign-extended encoded imm16 for BOTH
         * MOVW and MOVT. MOVT applies the high-half selection after S+A. */
        a=signed_bits(imm,16);break;
    default:return -1;
    }
    *addend=a;*width=4;return 0;
}
int xh_arm_elf_rel_apply(unsigned type,unsigned char*code,size_t length,size_t site,uint32_t symbol,uint32_t place,unsigned symbol_thumb){
    XhRelocContext ctx={0};int64_t addend;unsigned width;const char*key;
    if(symbol_thumb>1||xh_arm_elf_rel_decode(type,code,length,site,&addend,&width))return -1;
    switch(type){case 2:key="ELF_ABS32";break;case 10:key="ELF_THM_CALL";break;case 30:key="ELF_THM_JUMP24";break;case 51:key="ELF_THM_JUMP19";break;case 47:key="ELF_MOVW_ABS_NC";break;case 48:key="ELF_MOVT_ABS";break;default:return -1;}
    ctx.symbol=symbol_thumb?(symbol&~(uint32_t)1):symbol;
    ctx.addend=(uint64_t)addend;ctx.place=place;ctx.image_base=symbol_thumb;
    return xh_relocation_apply("arm",key,code,length,site,&ctx);
}
