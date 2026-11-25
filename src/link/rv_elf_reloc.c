/* RV32 psABI ELF RELA adapter. Target formulas are external ELF_* rules. */
#include "xh_rv_elf_reloc.h"
#include "xh_relocation.h"
static uint32_t word(const unsigned char*p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int xh_rv_elf_rela_width(unsigned type,unsigned*width){if(!width)return -1;switch(type){case 0:case 51:*width=0;return 0;case 18:case 19:*width=8;return 0;case 1:case 16:case 17:case 23:case 24:case 25:case 26:case 27:case 28:case 35:case 39:*width=4;return 0;default:return -1;}}
int xh_rv_elf_rela_apply(unsigned type,unsigned char*code,size_t length,size_t site,const XhRvElfRelContext*c){XhRelocContext context={0};const char*key=0;unsigned width;uint32_t first,second=0;if(!c||!code||xh_rv_elf_rela_width(type,&width)||site>length||width>length-site)return -1;if(!width)return 0;first=word(code+site);if(width==8)second=word(code+site+4);
 switch(type){
 case 1:key="ELF_ABS32";break;
 case 16:if((site&3)||(first&0x7f)!=0x63)return -1;key="ELF_BRANCH";break;
 case 17:if((site&3)||(first&0x7f)!=0x6f)return -1;key="ELF_JAL";break;
 case 18:case 19:if((site&3)||(first&0x7f)!=0x17||(second&0x707f)!=0x67||((first>>7)&31)!=((second>>15)&31))return -1;key="ELF_CALL";break;
 case 23:if((site&3)||(first&0x7f)!=0x17)return -1;key="ELF_PCREL_HI20";break;
 case 26:if((site&3)||(first&0x7f)!=0x37)return -1;key="ELF_HI20";break;
 case 24:case 27:if((site&3)||((first&0x7f)!=0x03&&(first&0x7f)!=0x13&&(first&0x7f)!=0x67))return -1;key=type==24?"ELF_PCREL_LO12_I":"ELF_LO12_I";break;
 case 25:case 28:if((site&3)||(first&0x7f)!=0x23)return -1;key=type==25?"ELF_PCREL_LO12_S":"ELF_LO12_S";break;
 case 35:key="ELF_ADD32";break;
 case 39:key="ELF_SUB32";break;
 default:return -1;
 }
 if(type==24||type==25){if(c->has_pair!=1||c->addend!=0)return -1;context.symbol=c->pair_symbol;context.addend=(uint64_t)(int64_t)c->pair_addend;context.place=c->pair_place;}else{context.symbol=c->symbol;context.addend=(uint64_t)(int64_t)c->addend;context.place=c->place;}
 return xh_relocation_apply("riscv32",key,code,length,site,&context);
}
