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
typedef struct {int n;Rule rules[XH_RULE_MAX_RULES];} Rules;
static Rules *cache;
static char selected[512];
static const char *default_path="packages/riscv32/encoding.rules";
static int fail(const char *message){diag_tool(DIAG_SEV_ERROR,xh_rules_path(),message,0,0,0,0,0,0);return -1;}
const char*xh_rules_path(void){return selected[0]?selected:default_path;}
void xh_rules_release(void){free(cache);cache=0;}
int xh_rules_set_path(const char*p){size_t n=p?strlen(p):0;if(n>=sizeof(selected))return fail("encoding rules path is too long");if(strcmp(p?p:"",selected)){xh_rules_release();if(n)memcpy(selected,p,n);selected[n]=0;}return 0;}
static char*token(char**cursor){char*p=*cursor,*start;for(;;){while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;if(*p!='#')break;while(*p&&*p!='\n')p++;}if(!*p){*cursor=p;return 0;}start=p;while(*p&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'&&*p!='#')p++;if(*p=='#'){*p++=0;while(*p&&*p!='\n')p++;}else if(*p)*p++=0;*cursor=p;return start;}
static int number(char**p,unsigned*out){char*t=token(p),*q;unsigned long long v;if(!t||t[0]=='-')return -1;v=strtoull(t,&q,0);if(!*t||*q||v>0xffffffffULL)return -1;*out=(unsigned)v;return 0;}
int xh_rules_load(void){int size,n,i;char*text,*cur,*t;Rules*next;Rule*r=0;unsigned version;if(cache)return 0;size=fs_file_size(xh_rules_path());if(size<=0||size>XH_RULE_MAX_BYTES)return fail("encoding rules file is missing or exceeds 32768 bytes");text=malloc((size_t)size+1);next=calloc(1,sizeof(*next));if(!text||!next){free(text);free(next);return fail("not enough memory to load encoding rules");}n=fs_read_file(xh_rules_path(),text,size+1);if(n!=size)goto bad;cur=text;t=token(&cur);if(!t||strcmp(t,"XHENC")||number(&cur,&version)||version!=1)goto bad;
 while((t=token(&cur))!=0){
  if(!strcmp(t,"rule")){unsigned argc,base;char*name;if(r||next->n==XH_RULE_MAX_RULES)goto bad;name=token(&cur);if(!name||!*name||strlen(name)>=32||number(&cur,&argc)||argc>XH_RULE_MAX_ARGS||number(&cur,&base))goto bad;for(i=0;i<next->n;i++)if(!strcmp(next->rules[i].name,name))goto bad;r=&next->rules[next->n++];strcpy(r->name,name);r->argc=(int)argc;r->base=base;}
  else if(!strcmp(t,"arg")){unsigned index,bits,sign,align;if(!r||number(&cur,&index)||index>=(unsigned)r->argc||number(&cur,&bits)||bits<1||bits>32||number(&cur,&sign)||sign>1||number(&cur,&align)||!align||(align&(align-1))||r->args[index].seen)goto bad;r->args[index].bits=(unsigned char)bits;r->args[index].sign=(unsigned char)sign;r->args[index].align=align;r->args[index].seen=1;}
  else if(!strcmp(t,"bits")){unsigned arg,src,width,dst,mask;Field*f;if(!r||r->nfields==XH_RULE_MAX_FIELDS||number(&cur,&arg)||arg>=(unsigned)r->argc||number(&cur,&src)||number(&cur,&width)||width<1||width>32||number(&cur,&dst)||src>31||dst>31||src+width>32||dst+width>32)goto bad;mask=width==32?0xffffffffu:((1u<<width)-1u)<<dst;if((r->mask&mask)||(r->base&mask))goto bad;r->mask|=mask;f=&r->fields[r->nfields++];f->arg=(unsigned char)arg;f->src=(unsigned char)src;f->width=(unsigned char)width;f->dst=(unsigned char)dst;}
  else if(!strcmp(t,"end")){if(!r||!r->nfields)goto bad;for(i=0;i<r->argc;i++)if(!r->args[i].seen)goto bad;for(i=0;i<r->nfields;i++)if(r->fields[i].src+r->fields[i].width>r->args[r->fields[i].arg].bits)goto bad;r=0;}
  else goto bad;
 }
 if(r||!next->n)goto bad;free(text);cache=next;return 0;
 bad:free(text);free(next);return fail("invalid encoding rules: version, syntax, bounds or overlapping fields");
}
int xh_rules_encode(const char*name,const int*args,int count,unsigned*word){Rule*r=0;unsigned w;int i;if(!name||!args||!word)return fail("invalid encoding rule call");if(xh_rules_load())return -1;for(i=0;i<cache->n;i++)if(!strcmp(cache->rules[i].name,name)){r=&cache->rules[i];break;}if(!r||count!=r->argc)return fail("unknown encoding rule or wrong argument count");for(i=0;i<count;i++){Arg*a=&r->args[i];long long v=args[i],min=a->sign?-(1LL<<(a->bits-1)):0,max=a->sign?(1LL<<(a->bits-1))-1:(a->bits==32?4294967295LL:(1LL<<a->bits)-1);if(v<min||v>max)return fail("instruction operand is outside encoding rule range");if(((unsigned)args[i]&(a->align-1))!=0)return fail("instruction operand violates encoding rule alignment");}w=r->base;for(i=0;i<r->nfields;i++){Field*f=&r->fields[i];unsigned mask=f->width==32?0xffffffffu:(1u<<f->width)-1;w|=(((unsigned)args[f->arg]>>f->src)&mask)<<f->dst;}*word=w;return 0;}
