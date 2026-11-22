/* Finite IR selection v1/v2: bounded straight-line instruction sequences.
 * No code evaluation, branches, raw template interpolation or labels. */
#include "xh_rules.h"
#include "diag.h"
#include "flash_fs.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define SELECT_MAX_RULES 32
#define SELECT_MAX_STEPS 8
typedef struct{char mnemonic[8];unsigned char operand[3],count;}Step;
typedef struct{char op[16];int count;Step steps[SELECT_MAX_STEPS];}Select;
typedef struct{int count;Select rule[SELECT_MAX_RULES];}Selection;
static Selection*cache;
static char path[512];
static const char*selected_path(void){return path[0]?path:"packages/riscv32/selection.rules";}
static int error(const char*m){diag_tool(DIAG_SEV_ERROR,selected_path(),m,0,0,0,0,0,0);return -1;}
int xh_rules_set_selection_path(const char*p){size_t n=p?strlen(p):0;if(n>=sizeof(path))return error("selection rules path is too long");if(strcmp(p?p:"",path)){free(cache);cache=0;if(n)memcpy(path,p,n);path[n]=0;}return 0;}
static char*tok(char**p){char*s=*p,*t;while(*s==' '||*s=='\t'||*s=='\r')s++;if(!*s)return 0;t=s;while(*s&&*s!=' '&&*s!='\t'&&*s!='\r')s++;if(*s)*s++=0;*p=s;return t;}
static int ident(const char*s){int n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'))return 0;return n<16;}
/* -1 invalid; 2 unary pseudo, 3 register instruction, 4 immediate instruction. */
static int mnemonic(const char*s){static const char*allowed[]={"add","sub","and","or","xor","mul","sll","srl","sra","slt","sltu","div","divu","rem","remu"};int i;for(i=0;i<15;i++)if(!strcmp(s,allowed[i]))return 3;if(!strcmp(s,"seqz")||!strcmp(s,"snez"))return 2;if(!strcmp(s,"xori"))return 4;return -1;}
static int operand(const char*s){if(!strcmp(s,"dst"))return 0;if(!strcmp(s,"lhs"))return 1;if(!strcmp(s,"rhs"))return 2;if(!strcmp(s,"0"))return 3;if(!strcmp(s,"1"))return 4;return -1;}
static int addstep(Select*r,char**words,int count){int k,kind;Step*s;if(r->count==SELECT_MAX_STEPS||count<3||strlen(words[0])>=8)return -1;kind=mnemonic(words[0]);if(kind<0||count!=(kind==2?3:4))return -1;s=&r->steps[r->count];strcpy(s->mnemonic,words[0]);s->count=(unsigned char)(count-1);for(k=1;k<count;k++){int v=operand(words[k]);if(v<0)return -1;s->operand[k-1]=(unsigned char)v;}if(s->operand[0]!=0)return -1;/* Every step writes the result register, never frame/stack state. */
 if(s->operand[1]>2)return -1;if(kind==3&&s->operand[2]>2)return -1;if(kind==4&&s->operand[2]<3)return -1;r->count++;return 0;}
static Select*addrule(Selection*s,const char*name){int i;Select*r;if(!ident(name)||s->count==SELECT_MAX_RULES)return 0;for(i=0;i<s->count;i++)if(!strcmp(s->rule[i].op,name))return 0;r=&s->rule[s->count++];strcpy(r->op,name);return r;}
static int load(void){int size,n,version=0;char*text,*p,*end;Selection*next;Select*active=0;if(cache)return 0;size=fs_file_size(selected_path());if(size<1||size>8192)return error("selection rules file is missing or exceeds 8192 bytes");text=malloc((size_t)size+1);next=calloc(1,sizeof(*next));if(!text||!next){free(text);free(next);return error("not enough memory for selection rules");}n=fs_read_file(selected_path(),text,size+1);if(n!=size)goto bad;p=text;
 while(*p){char*words[9],*cur,*comment;int count=0;end=strchr(p,'\n');if(end)*end=0;comment=strchr(p,'#');if(comment)*comment=0;cur=p;while(count<9&&(words[count]=tok(&cur))!=0)count++;if(count){if(!version){if(count!=2||strcmp(words[0],"XHSELECT"))goto bad;if(!strcmp(words[1],"1"))version=1;else if(!strcmp(words[1],"2"))version=2;else goto bad;}else if(version==1){Select*r;if(count!=7||strcmp(words[0],"select")||strcmp(words[2],"scalar32"))goto bad;r=addrule(next,words[1]);if(!r||addstep(r,words+3,4))goto bad;}else if(!strcmp(words[0],"select")){if(active||count!=3||strcmp(words[2],"scalar32"))goto bad;active=addrule(next,words[1]);if(!active)goto bad;}else if(!strcmp(words[0],"emit")){if(!active||addstep(active,words+1,count-1))goto bad;}else if(!strcmp(words[0],"end")){if(!active||count!=1||!active->count)goto bad;active=0;}else goto bad;}if(!end)break;p=end+1;}
 if(!version||active)goto bad;free(text);cache=next;return 0;
 bad:free(text);free(next);return error("invalid selection rules version, syntax, duplicate, sequence or register selector");
}
int xh_rules_select_binary(const char*operation,char*out,int cap){static const char*operands[]={"t0","t0","t1","0","1"};int i,j,k,used=0,n;if(!operation||!out||cap<1)return error("invalid selection rule call");if(load())return -1;for(i=0;i<cache->count;i++){Select*r=&cache->rule[i];if(strcmp(r->op,operation))continue;for(j=0;j<r->count;j++){Step*s=&r->steps[j];n=snprintf(out+used,(size_t)(cap-used),"    %s ",s->mnemonic);if(n<0||n>=cap-used)goto overflow;used+=n;for(k=0;k<s->count;k++){n=snprintf(out+used,(size_t)(cap-used),"%s%s",k?", ":"",operands[s->operand[k]]);if(n<0||n>=cap-used)goto overflow;used+=n;}if(used+1>=cap)goto overflow;out[used++]='\n';out[used]=0;}return 1;}return 0;
 overflow:return error("selection rule output exceeds buffer");}
