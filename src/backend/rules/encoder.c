/* XH encoding rules v1: finite records, no jumps/loops/expressions or plugins.
 * Cache holds at most 32 rules * 32 fields; file scratch is freed after load.
 * Grammar (decimal/0x numbers; comments begin #):
 * XHENC 1
 * rule NAME ARGCOUNT BASE
 * arg INDEX BITS SIGNED ALIGN       (SIGNED 0/1, ALIGN power of two)
 * bits ARG SOURCE_LSB WIDTH DEST_LSB
 * end
 * Encoding checks complete argument constraints before scattering bit slices.
 */
#include "xh_rules.h"
#include "flash_fs.h"
#include "diag.h"
#include "memory.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
typedef struct {unsigned char arg,src,width,dst;} Field;
typedef struct {unsigned char bits,sign;unsigned align;int seen;} Arg;
typedef struct {char name[32];unsigned base,mask;int argc,nfields;Arg args[XH_RULE_MAX_ARGS];Field fields[XH_RULE_MAX_FIELDS];} Rule;
typedef struct {int n;Rule rules[1];} Rules;
typedef struct {char target[32],path[512];} Target;
static Target paths[XH_ENCODING_MAX_TARGETS];
static int path_count;
static Rules *cache;
static char cached_target[32];
static char diagnostic_target[32]="riscv32";
static void diagnostic(const char*t){if(t&&*t&&strlen(t)<sizeof(diagnostic_target))strcpy(diagnostic_target,t);else strcpy(diagnostic_target,"riscv32");}
const char*xh_encoding_path(const char*target){int i;if(!target)return "<encoding>";for(i=0;i<path_count;i++)if(!strcmp(paths[i].target,target)&&paths[i].path[0])return paths[i].path;if(!strcmp(target,"riscv32"))return "packages/riscv32/encoding.rules";if(!strcmp(target,"arm"))return "packages/arm-cortex-m3/arm.encoding.rules";if(!strcmp(target,"x86"))return "packages/x86/x86.encoding.rules";if(!strcmp(target,"x86_64"))return "packages/x86_64/x86.encoding.rules";return "<encoding>";}
static int fail(const char *message){diag_tool(DIAG_SEV_ERROR,xh_encoding_path(diagnostic_target),message,0,0,0,0,0,0);return -1;}
void xh_encoding_release(void){free(cache);cache=0;cached_target[0]=0;}
int xh_encoding_set_path(const char*target,const char*p){size_t n=p?strlen(p):0;int i;diagnostic(target);if(!target||!*target||strlen(target)>=32||n>=512)return fail("invalid encoding target or path");for(i=0;i<path_count;i++)if(!strcmp(paths[i].target,target))break;if(i==path_count){if(path_count==XH_ENCODING_MAX_TARGETS)return fail("too many encoding targets");strcpy(paths[i].target,target);paths[i].path[0]=0;path_count++;}if(strcmp(p?p:"",paths[i].path)){if(!strcmp(cached_target,target))xh_encoding_release();if(n)memcpy(paths[i].path,p,n);paths[i].path[n]=0;}return 0;}
const char*xh_rules_path(void){return xh_encoding_path("riscv32");}
void xh_rules_release(void){xh_encoding_release();}
int xh_rules_set_path(const char*p){return xh_encoding_set_path("riscv32",p);}
static char*token(char**cursor){char*p=*cursor,*start;for(;;){while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;if(*p!='#')break;while(*p&&*p!='\n')p++;}if(!*p){*cursor=p;return 0;}start=p;while(*p&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'&&*p!='#')p++;if(*p=='#'){*p++=0;while(*p&&*p!='\n')p++;}else if(*p)*p++=0;*cursor=p;return start;}
static int number(char**p,unsigned*out){char*t=token(p),*q;unsigned long long v;if(!t||t[0]=='-')return -1;v=strtoull(t,&q,0);if(!*t||*q||v>0xffffffffULL)return -1;*out=(unsigned)v;return 0;}
int xh_encoding_load(const char*target){int size,n,i,rule_capacity=0;char*text,*cur,*t;Rules*next;Rule*r=0;unsigned version;diagnostic(target);if(!target||!*target||strlen(target)>=32)return fail("invalid encoding target");if(cache&&!strcmp(cached_target,target))return 0;xh_encoding_release();size=fs_file_size(xh_encoding_path(target));if(size<=0||size>XH_RULE_MAX_BYTES)return fail("encoding rules file is missing or exceeds 65536 bytes");text=malloc((size_t)size+1);next=0;if(!text)return fail("not enough memory to load encoding rules");n=fs_read_file(xh_encoding_path(target),text,size+1);if(n!=size)goto bad;
 /* Count rule tokens with no table allocation. File scratch remains exact size. */
 {char*scan=text;int start=1;while(*scan){if(*scan=='#'){while(*scan&&*scan!='\n')scan++;continue;}if(*scan==' '||*scan=='\t'||*scan=='\r'||*scan=='\n'){start=1;scan++;continue;}if(start&&scan+4<text+size&&scan[0]=='r'&&scan[1]=='u'&&scan[2]=='l'&&scan[3]=='e'&&(scan[4]==' '||scan[4]=='\t'||scan[4]=='\r'||scan[4]=='\n'))rule_capacity++;start=0;scan++;}}
 if(rule_capacity<1||rule_capacity>XH_RULE_MAX_RULES)goto bad;next=calloc(1,sizeof(*next)+(size_t)(rule_capacity-1)*sizeof(Rule));if(!next){free(text);return fail("not enough memory to load encoding rules");}cur=text;t=token(&cur);if(!t||strcmp(t,"XHENC")||number(&cur,&version)||version!=1)goto bad;
 while((t=token(&cur))!=0){
  if(!strcmp(t,"rule")){unsigned argc,base;char*name;if(r||next->n==rule_capacity)goto bad;name=token(&cur);if(!name||!*name||strlen(name)>=32||number(&cur,&argc)||argc>XH_RULE_MAX_ARGS||number(&cur,&base))goto bad;for(i=0;i<next->n;i++)if(!strcmp(next->rules[i].name,name))goto bad;r=&next->rules[next->n++];strcpy(r->name,name);r->argc=(int)argc;r->base=base;}
  else if(!strcmp(t,"arg")){unsigned index,bits,sign,align;if(!r||number(&cur,&index)||index>=(unsigned)r->argc||number(&cur,&bits)||bits<1||bits>32||number(&cur,&sign)||sign>1||number(&cur,&align)||!align||(align&(align-1))||r->args[index].seen)goto bad;r->args[index].bits=(unsigned char)bits;r->args[index].sign=(unsigned char)sign;r->args[index].align=align;r->args[index].seen=1;}
  else if(!strcmp(t,"bits")){unsigned arg,src,width,dst,mask;Field*f;if(!r||r->nfields==XH_RULE_MAX_FIELDS||number(&cur,&arg)||arg>=(unsigned)r->argc||number(&cur,&src)||number(&cur,&width)||width<1||width>32||number(&cur,&dst)||src>31||dst>31||src+width>32||dst+width>32)goto bad;mask=width==32?0xffffffffu:((1u<<width)-1u)<<dst;if((r->mask&mask)||(r->base&mask))goto bad;r->mask|=mask;f=&r->fields[r->nfields++];f->arg=(unsigned char)arg;f->src=(unsigned char)src;f->width=(unsigned char)width;f->dst=(unsigned char)dst;}
  else if(!strcmp(t,"end")){if(!r||(!r->nfields&&r->argc))goto bad;for(i=0;i<r->argc;i++)if(!r->args[i].seen)goto bad;for(i=0;i<r->nfields;i++)if(r->fields[i].src+r->fields[i].width>r->args[r->fields[i].arg].bits)goto bad;r=0;}
  else goto bad;
 }
 if(r||!next->n)goto bad;free(text);cache=next;strcpy(cached_target,target);return 0;
 bad:free(text);free(next);return fail("invalid encoding rules: version, syntax, bounds or overlapping fields");
}
int xh_encoding_encode(const char*target,const char*name,const int*args,int count,unsigned*word){Rule*r=0;unsigned w;int i;diagnostic(target);if(!name||(!args&&count)||!word||count<0)return fail("invalid encoding rule call");if(xh_encoding_load(target))return -1;for(i=0;i<cache->n;i++)if(!strcmp(cache->rules[i].name,name)){r=&cache->rules[i];break;}if(!r||count!=r->argc)return fail("unknown encoding rule or wrong argument count");for(i=0;i<count;i++){Arg*a=&r->args[i];long long v=args[i],min=a->sign?-(1LL<<(a->bits-1)):0,max=a->sign?(1LL<<(a->bits-1))-1:(a->bits==32?4294967295LL:(1LL<<a->bits)-1);if(v<min||v>max)return fail("instruction operand is outside encoding rule range");if(((unsigned)args[i]&(a->align-1))!=0)return fail("instruction operand violates encoding rule alignment");}w=r->base;for(i=0;i<r->nfields;i++){Field*f=&r->fields[i];unsigned mask=f->width==32?0xffffffffu:(1u<<f->width)-1;w|=(((unsigned)args[f->arg]>>f->src)&mask)<<f->dst;}*word=w;return 0;}

int xh_rules_load(void){return xh_encoding_load("riscv32");}
int xh_rules_encode(const char*name,const int*args,int count,unsigned*word){return xh_encoding_encode("riscv32",name,args,count,word);}
