/* Bounded generic instruction sequence profiles. XHSELECT1/2 are supported
 * solely through the explicit historical RV32 compatibility profile. Version3
 * carries register bindings and instruction operand shapes as external data. */
#include "xh_rules.h"
#include "diag.h"
#include "flash_fs.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define MAX_RULES 32
#define MAX_STEPS 8
#define MAX_ALLOWED 64
#define MAX_BYTES 16384
typedef struct{char mnemonic[24],shape[4];}Allowed;
typedef struct{unsigned char allowed,operand[3],count;}Step;
typedef struct{char op[32];int count;Step steps[MAX_STEPS];}Select;
typedef struct{int count,allowed_count,version;char registers[3][24];Allowed allowed[MAX_ALLOWED];Select rule[1];}Selection;
typedef struct{char target[32],path[512];}Location;
static Selection*cache;static Location paths[8];static int path_count;static char active[32];static char diagnostic[32]="riscv32";
static void set_diagnostic(const char*t){if(t&&strlen(t)<32)strcpy(diagnostic,t);}
static const char*pathof(const char*t){int i;for(i=0;i<path_count;i++)if(!strcmp(paths[i].target,t)&&paths[i].path[0])return paths[i].path;return !strcmp(t,"riscv32")?"packages/riscv32/selection.rules":"<selection.rules>";}
static int error(const char*m){diag_tool(DIAG_SEV_ERROR,pathof(diagnostic),m,0,0,0,0,0,0);return -1;}
void xh_sequence_release(void){free(cache);cache=0;active[0]=0;}
static int ident(const char*s,size_t cap){size_t n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'))return 0;return n<cap;}
int xh_sequence_set_path(const char*t,const char*p){int i;size_t n=p?strlen(p):0;if(!ident(t,32)||n>=512)return -1;set_diagnostic(t);for(i=0;i<path_count;i++)if(!strcmp(paths[i].target,t))break;if(i==path_count){if(i==8)return error("too many sequence profile targets");strcpy(paths[i].target,t);paths[i].path[0]=0;path_count++;}if(strcmp(p?p:"",paths[i].path)){if(!strcmp(active,t))xh_sequence_release();if(n)memcpy(paths[i].path,p,n);paths[i].path[n]=0;}return 0;}
static char*token(char**p){char*s=*p,*t;while(*s==' '||*s=='\t'||*s=='\r')s++;if(!*s)return 0;t=s;while(*s&&*s!=' '&&*s!='\t'&&*s!='\r')s++;if(*s)*s++=0;*p=s;return t;}
static int operand(const char*s){if(!strcmp(s,"dst"))return 0;if(!strcmp(s,"lhs"))return 1;if(!strcmp(s,"rhs"))return 2;if(!strcmp(s,"0"))return 3;if(!strcmp(s,"1"))return 4;return -1;}
static int allow(Selection*s,const char*name,const char*shape){Allowed*a;int i;if(!ident(name,24)||s->allowed_count==MAX_ALLOWED||(strcmp(shape,"rr")&&strcmp(shape,"rrr")&&strcmp(shape,"rri")))return -1;for(i=0;i<s->allowed_count;i++)if(!strcmp(s->allowed[i].mnemonic,name))return -1;a=&s->allowed[s->allowed_count++];strcpy(a->mnemonic,name);strcpy(a->shape,shape);return 0;}
static int legacy(Selection*s,const char*target){static const char*names[]={"add","sub","and","or","xor","mul","sll","srl","sra","slt","sltu","div","divu","rem","remu"};int i;if(strcmp(target,"riscv32"))return -1;strcpy(s->registers[0],"t0");strcpy(s->registers[1],"t0");strcpy(s->registers[2],"t1");for(i=0;i<15;i++)if(allow(s,names[i],"rrr"))return -1;return allow(s,"seqz","rr")||allow(s,"snez","rr")||allow(s,"xori","rri")?-1:0;}
static int step(Selection*s,Select*r,char**words,int count){int i,k,which=-1;Step*out;if(r->count==MAX_STEPS||count<3)return -1;for(i=0;i<s->allowed_count;i++)if(!strcmp(s->allowed[i].mnemonic,words[0])){which=i;break;}if(which<0||count!=(int)strlen(s->allowed[which].shape)+1)return -1;out=&r->steps[r->count];out->allowed=(unsigned char)which;out->count=(unsigned char)(count-1);for(k=1;k<count;k++){int value=operand(words[k]);if(value<0||(s->allowed[which].shape[k-1]=='r'?value>2:value<3))return -1;out->operand[k-1]=(unsigned char)value;}if(out->operand[0]!=0)return -1;r->count++;return 0;}
static Select*addrule(Selection*s,int capacity,const char*name){int i;Select*r;if(!ident(name,32)||s->count==capacity)return 0;for(i=0;i<s->count;i++)if(!strcmp(s->rule[i].op,name))return 0;r=&s->rule[s->count++];strcpy(r->op,name);return r;}
static int load(const char*target){int size,n,version=0,capacity=0,register_mask=0;char*text,*p;Selection*next=0;Select*current=0;set_diagnostic(target);if(cache&&!strcmp(active,target))return 0;xh_sequence_release();size=fs_file_size(pathof(target));if(size<1||size>MAX_BYTES)return error("selection rules file missing or exceeds 16384 bytes");text=malloc((size_t)size+1);if(!text)return error("not enough memory for sequence profile");n=fs_read_file(pathof(target),text,size+1);if(n!=size||memchr(text,0,(size_t)size))goto bad;
 {char*line=text;while(*line){char*end=strchr(line,'\n'),*q=line;while(*q==' '||*q=='\t')q++;if(!strncmp(q,"select ",7)||!strncmp(q,"select\t",7))capacity++;if(!end)break;line=end+1;}}if(capacity>MAX_RULES)goto bad;next=calloc(1,sizeof(*next)+(size_t)(capacity>0?capacity-1:0)*sizeof(Select));if(!next){free(text);return error("not enough memory for sequence profile");}p=text;
 while(*p){char*words[9],*end=strchr(p,'\n'),*comment,*cur;int count=0;if(end)*end=0;comment=strchr(p,'#');if(comment)*comment=0;cur=p;while(count<9&&(words[count]=token(&cur))!=0)count++;if(count){if(!version){if(count!=2||strcmp(words[0],"XHSELECT"))goto bad;version=!strcmp(words[1],"1")?1:!strcmp(words[1],"2")?2:!strcmp(words[1],"3")?3:0;if(!version)goto bad;next->version=version;if(version<3&&legacy(next,target))goto bad;}
 else if(version==3&&!strcmp(words[0],"register")){int role;if(current||next->count||count!=3||(role=operand(words[1]))<0||role>2||!ident(words[2],24)||(register_mask&(1<<role)))goto bad;strcpy(next->registers[role],words[2]);register_mask|=1<<role;}
 else if(version==3&&!strcmp(words[0],"allow")){if(current||next->count||count!=3||allow(next,words[1],words[2]))goto bad;}
 else if(version==1){Select*r;if(count!=7||strcmp(words[0],"select")||strcmp(words[2],"scalar32"))goto bad;r=addrule(next,capacity,words[1]);if(!r||step(next,r,words+3,4))goto bad;}
 else if(!strcmp(words[0],"select")){if(current||count!=3||strcmp(words[2],"scalar32")||(version==3&&(register_mask!=7||!next->allowed_count)))goto bad;current=addrule(next,capacity,words[1]);if(!current)goto bad;}
 else if(!strcmp(words[0],"emit")){if(!current||step(next,current,words+1,count-1))goto bad;}
 else if(!strcmp(words[0],"end")){if(!current||count!=1||!current->count)goto bad;current=0;}else goto bad;}
 if(!end)break;p=end+1;}
 if(!version||current||(version==3&&(register_mask!=7||!next->allowed_count)))goto bad;free(text);cache=next;strcpy(active,target);return 0;
 bad:free(text);free(next);return error("invalid selection rules version, syntax, duplicate, profile or operand shape");}
int xh_sequence_select(const char*target,const char*op,const XhSequenceBindings*bindings,char*out,int cap){int i,j,k,n,used=0;const char*values[5];set_diagnostic(target?target:"riscv32");if(!ident(target,32)||!ident(op,32)||!out||cap<1)return error("invalid sequence call");out[0]=0;if(load(target))return -1;for(i=0;i<3;i++)values[i]=cache->registers[i];values[3]="0";values[4]="1";if(bindings){const char*b[3]={bindings->dst,bindings->lhs,bindings->rhs};for(i=0;i<3;i++)if(!ident(b[i],24)||strcmp(b[i],values[i]))return error("sequence register binding disagrees with profile");}
 for(i=0;i<cache->count;i++){Select*r=&cache->rule[i];if(strcmp(r->op,op))continue;for(j=0;j<r->count;j++){Step*s=&r->steps[j];n=snprintf(out+used,(size_t)(cap-used),"    %s ",cache->allowed[s->allowed].mnemonic);if(n<0||n>=cap-used)goto overflow;used+=n;for(k=0;k<s->count;k++){n=snprintf(out+used,(size_t)(cap-used),"%s%s",k?", ":"",values[s->operand[k]]);if(n<0||n>=cap-used)goto overflow;used+=n;}if(used+1>=cap)goto overflow;out[used++]='\n';out[used]=0;}return 1;}return 0;
 overflow:out[0]=0;return error("selection rule output exceeds buffer");}
int xh_rules_set_selection_path(const char*p){return xh_sequence_set_path("riscv32",p);}
int xh_rules_select_binary(const char*op,char*out,int cap){const XhSequenceBindings bindings={"t0","t0","t1"};return xh_sequence_select("riscv32",op,&bindings,out,cap);}
