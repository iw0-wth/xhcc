/* XHRELOC 1: finite straight-line relocation arithmetic, atomic <=8byte patch.
 * Values are uint64 bit patterns. Arithmetic wraps modulo 2^64 explicitly;
 * signed range/arithmetic right shift use two's-complement definitions.
 * No branches, loops, indirect memory, target-native evaluation or host calls. */
#include "xh_relocation.h"
#include "flash_fs.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define MAX_RULES 32
#define MAX_OPS 32
#define MAX_BYTES 16384
typedef struct{unsigned char op,a,b,c;uint64_t imm;}Op;
typedef struct{char name[32];int span,n;Op ops[MAX_OPS];}Rule;
typedef struct{int n;Rule rules[MAX_RULES];}Table;
typedef struct{char target[32],path[512];}Path;
static Path paths[8];static int npaths;static Table*cache;static char active[32];
static const char*pathof(const char*t){int i;for(i=0;i<npaths;i++)if(!strcmp(paths[i].target,t)&&paths[i].path[0])return paths[i].path;if(!strcmp(t,"arm"))return "packages/arm-cortex-m3/relocation.rules";if(!strcmp(t,"riscv32"))return "packages/riscv32/relocation.rules";if(!strcmp(t,"x86"))return "packages/x86/relocation.rules";if(!strcmp(t,"x86_64"))return "packages/x86_64/relocation.rules";return "<relocation.rules>";}
static int error(const char*t,const char*m){diag_tool(DIAG_SEV_ERROR,pathof(t?t:""),m,0,0,0,0,0,0);return -1;}
void xh_relocation_release(void){free(cache);cache=0;active[0]=0;}
int xh_relocation_set_path(const char*t,const char*p){int i;size_t n=p?strlen(p):0;if(!t||!*t||strlen(t)>=32||n>=512)return -1;for(i=0;i<npaths;i++)if(!strcmp(paths[i].target,t))break;if(i==npaths){if(npaths==8)return error(t,"too many relocation targets");strcpy(paths[i].target,t);paths[i].path[0]=0;npaths++;}if(strcmp(paths[i].path,p?p:"")){if(!strcmp(active,t))xh_relocation_release();if(n)memcpy(paths[i].path,p,n);paths[i].path[n]=0;}return 0;}
static char*tok(char**p){char*s=*p,*start;while(*s==' '||*s=='\t'||*s=='\r')s++;if(!*s)return 0;start=s;while(*s&&*s!=' '&&*s!='\t'&&*s!='\r')s++;if(*s)*s++=0;*p=s;return start;}
static int num(const char*s,uint64_t*out){char*e;unsigned long long v;if(!s||!*s||*s=='-')return -1;errno=0;v=strtoull(s,&e,0);if(*e||errno==ERANGE)return -1;*out=(uint64_t)v;return 0;}
static int reg(const char*s){uint64_t n;if(!s||*s!='r'||num(s+1,&n)||n>=16)return -1;return(int)n;}
enum{SET=1,CONST,READ,ADD,SUB,AND,OR,XOR,SHL,ASR,RANGE,ALIGN,CHECKMASK,WRITE};
int xh_relocation_load(const char*t){int size,n,header=0;unsigned initialized=0;char*text,*line;Table*next;Rule*r=0;if(!t||!*t||strlen(t)>=32)return -1;if(cache&&!strcmp(active,t))return 0;xh_relocation_release();size=fs_file_size(pathof(t));if(size<1||size>MAX_BYTES)return error(t,"relocation rules missing or oversized");text=malloc((size_t)size+1);next=calloc(1,sizeof(*next));if(!text||!next){free(text);free(next);return error(t,"not enough memory for relocation rules");}n=fs_read_file(pathof(t),text,size+1);if(n!=size)goto bad;line=text;
 while(*line){char*end=strchr(line,'\n'),*comment,*p,*w[7];int count=0,i,a,b,c;uint64_t v;Op*o;if(end)*end=0;comment=strchr(line,'#');if(comment)*comment=0;p=line;while(count<7&&(w[count]=tok(&p))!=0)count++;if(!count)goto advance;
 if(!header){if(count!=2||strcmp(w[0],"XHRELOC")||strcmp(w[1],"1"))goto bad;header=1;goto advance;}
 if(!strcmp(w[0],"rule")){if(r||count!=3||strlen(w[1])>=32||num(w[2],&v)||v<1||v>8||next->n==MAX_RULES)goto bad;for(i=0;i<next->n;i++)if(!strcmp(next->rules[i].name,w[1]))goto bad;r=&next->rules[next->n++];strcpy(r->name,w[1]);r->span=(int)v;initialized=0;goto advance;}
 if(!strcmp(w[0],"end")){if(!r||count!=1||!r->n)goto bad;r=0;goto advance;}
 if(!r||r->n==MAX_OPS||count<2)goto bad;o=&r->ops[r->n];a=reg(w[1]);if(a<0)goto bad;o->a=(unsigned char)a;
 if(!strcmp(w[0],"set")){if(count!=3||strlen(w[2])!=1||!strchr("SAPB",w[2][0]))goto bad;o->op=SET;o->b=(unsigned char)w[2][0];}
 else if(!strcmp(w[0],"const")){if(count!=3||num(w[2],&o->imm))goto bad;o->op=CONST;}
 else if(!strcmp(w[0],"read")||!strcmp(w[0],"write")){uint64_t width,off;if(count!=4||num(w[2],&off)||num(w[3],&width)||(width!=1&&width!=2&&width!=4&&width!=8)||off+width>(unsigned)r->span||off>8)goto bad;o->op=!strcmp(w[0],"read")?READ:WRITE;o->b=(unsigned char)off;o->c=(unsigned char)width;}
 else if(!strcmp(w[0],"range")){if(count!=4||num(w[2],&v)||v<1||v>64||(strcmp(w[3],"signed")&&strcmp(w[3],"unsigned")))goto bad;o->op=RANGE;o->b=(unsigned char)v;o->c=(unsigned char)!strcmp(w[3],"signed");}
 else if(!strcmp(w[0],"align")){if(count!=3||num(w[2],&v)||!v||(v&(v-1)))goto bad;o->op=ALIGN;o->imm=v;}
 else if(!strcmp(w[0],"checkmask")){if(count!=4||num(w[2],&o->imm)||num(w[3],&v)||(v&~o->imm))goto bad;o->op=CHECKMASK;o->b=0;/* expected is held in second op to avoid raw pointers */if(r->n+1>=MAX_OPS)goto bad;r->n++;r->ops[r->n].op=CONST;r->ops[r->n].a=255;r->ops[r->n].imm=v;}
 else {if(count!=4||(b=reg(w[2]))<0)goto bad;o->b=(unsigned char)b;if(!strcmp(w[0],"shl")||!strcmp(w[0],"asr")){if(num(w[3],&v)||v>63)goto bad;o->op=!strcmp(w[0],"shl")?SHL:ASR;o->c=(unsigned char)v;}else{if((c=reg(w[3]))<0)goto bad;o->c=(unsigned char)c;o->op=!strcmp(w[0],"add")?ADD:!strcmp(w[0],"sub")?SUB:!strcmp(w[0],"and")?AND:!strcmp(w[0],"or")?OR:!strcmp(w[0],"xor")?XOR:0;if(!o->op)goto bad;}}
 if(o->op==SET||o->op==CONST||o->op==READ)initialized|=1u<<o->a;
 else if(o->op==WRITE||o->op==RANGE||o->op==ALIGN||o->op==CHECKMASK){if(!(initialized&(1u<<o->a)))goto bad;}
 else {if(!(initialized&(1u<<o->b))||((o->op!=SHL&&o->op!=ASR)&&!(initialized&(1u<<o->c))))goto bad;initialized|=1u<<o->a;}
 r->n++;
 advance:if(!end)break;line=end+1;
 }
 if(!header||r||!next->n)goto bad;free(text);cache=next;strcpy(active,t);return 0;
 bad:free(text);free(next);return error(t,"invalid relocation rules version, syntax or bounds");}
static uint64_t asr64(uint64_t v,unsigned n){if(!n)return v;return(v>>n)|((v>>63)?(~(uint64_t)0<<(64-n)):0);}
int xh_relocation_apply(const char*t,const char*name,unsigned char*code,size_t length,size_t site,const XhRelocContext*ctx){uint64_t temp[16]={0};unsigned char scratch[8];Rule*r=0;int i,j;if(!t||!name||!code||!ctx)return -1;if(xh_relocation_load(t))return -1;for(i=0;i<cache->n;i++)if(!strcmp(cache->rules[i].name,name)){r=&cache->rules[i];break;}if(!r)return error(t,"unknown relocation rule");if(site>length||(size_t)r->span>length-site)return error(t,"relocation site outside buffer");memcpy(scratch,code+site,(size_t)r->span);
 for(i=0;i<r->n;i++){Op*o=&r->ops[i];uint64_t v=0,mask;switch(o->op){case SET:temp[o->a]=o->b=='S'?ctx->symbol:o->b=='A'?ctx->addend:o->b=='P'?ctx->place:ctx->image_base;break;case CONST:temp[o->a]=o->imm;break;case READ:for(j=0;j<o->c;j++)v|=(uint64_t)scratch[o->b+j]<<(8*j);temp[o->a]=v;break;case WRITE:for(j=0;j<o->c;j++)scratch[o->b+j]=(unsigned char)(temp[o->a]>>(8*j));break;case ADD:temp[o->a]=temp[o->b]+temp[o->c];break;case SUB:temp[o->a]=temp[o->b]-temp[o->c];break;case AND:temp[o->a]=temp[o->b]&temp[o->c];break;case OR:temp[o->a]=temp[o->b]|temp[o->c];break;case XOR:temp[o->a]=temp[o->b]^temp[o->c];break;case SHL:temp[o->a]=temp[o->b]<<o->c;break;case ASR:temp[o->a]=asr64(temp[o->b],o->c);break;case RANGE:if(o->b<64){mask=((uint64_t)1<<o->b)-1;if(o->c){v=asr64(temp[o->a],o->b-1);if(v!=0&&v!=~(uint64_t)0)goto violation;}else if(temp[o->a]&~mask)goto violation;}break;case ALIGN:if(temp[o->a]&(o->imm-1))goto violation;break;case CHECKMASK:if((temp[o->a]&o->imm)!=r->ops[++i].imm)goto violation;break;default:goto violation;}}
 memcpy(code+site,scratch,(size_t)r->span);return 0;
 violation:return error(t,"relocation assertion failed; original bytes unchanged");}
