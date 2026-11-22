/* Finite IR selector v1. No native evaluation or template interpolation.
 * XHSELECT 1 ; select OP scalar32 mnemonic dst|lhs|rhs dst|lhs|rhs dst|lhs|rhs
 * Newlines separate records; # comments allowed. One matched rule emits one
 * register instruction. Unmatched IR remains explicitly the caller's C path. */
#include "xh_rules.h"
#include "diag.h"
#include "flash_fs.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct{char op[16],mnemonic[16];unsigned char reg[3];}Select;
typedef struct{int count;Select rule[32];}Selection;
static Selection*cache;
static char path[512];
static const char*selected_path(void){return path[0]?path:"packages/riscv32/selection.rules";}
static int error(const char*m){diag_tool(DIAG_SEV_ERROR,selected_path(),m,0,0,0,0,0,0);return -1;}
int xh_rules_set_selection_path(const char*p){size_t n=p?strlen(p):0;if(n>=sizeof(path))return error("selection rules path is too long");if(strcmp(p?p:"",path)){free(cache);cache=0;if(n)memcpy(path,p,n);path[n]=0;}return 0;}
static char*tok(char**p){char*s=*p,*t;while(*s==' '||*s=='\t'||*s=='\r')s++;if(!*s||*s=='#'){*p=s;return 0;}t=s;while(*s&&*s!=' '&&*s!='\t'&&*s!='\r'&&*s!='#')s++;if(*s)*s++=0;*p=s;return t;}
static int ident(const char*s){int n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'))return 0;return n<16;}
static int mnemonic(const char*s){static const char*allowed[]={"add","sub","and","or","xor","mul","sll","srl","sra","slt","sltu","div","divu","rem","remu"};int i;for(i=0;i<15;i++)if(!strcmp(s,allowed[i]))return 1;return 0;}
static int load(void){int size,n,line=0;char*text,*p,*end;Selection*next;if(cache)return 0;size=fs_file_size(selected_path());if(size<1||size>8192)return error("selection rules file is missing or exceeds 8192 bytes");text=malloc((size_t)size+1);next=calloc(1,sizeof(*next));if(!text||!next){free(text);free(next);return error("not enough memory for selection rules");}n=fs_read_file(selected_path(),text,size+1);if(n!=size)goto bad;p=text;
 while(*p){char*words[8],*cur;int count=0,i;end=strchr(p,'\n');if(end)*end=0;{char*comment=strchr(p,'#');if(comment)*comment=0;}cur=p;while(count<8&&(words[count]=tok(&cur))!=0)count++;if(count){if(!line){if(count!=2||strcmp(words[0],"XHSELECT")||strcmp(words[1],"1"))goto bad;line=1;}else{Select*r;if(count!=7||strcmp(words[0],"select")||strcmp(words[2],"scalar32")||!ident(words[1])||!ident(words[3])||!mnemonic(words[3])||next->count==32)goto bad;for(i=0;i<next->count;i++)if(!strcmp(next->rule[i].op,words[1]))goto bad;r=&next->rule[next->count++];strcpy(r->op,words[1]);strcpy(r->mnemonic,words[3]);for(i=0;i<3;i++){if(!strcmp(words[i+4],"dst"))r->reg[i]=0;else if(!strcmp(words[i+4],"lhs"))r->reg[i]=1;else if(!strcmp(words[i+4],"rhs"))r->reg[i]=2;else goto bad;}/* result must be written to the stack-result register */if(r->reg[0]!=0)goto bad;}}if(!end)break;p=end+1;}
 if(!line)goto bad;free(text);cache=next;return 0;
 bad:free(text);free(next);return error("invalid selection rules version, syntax, duplicate or register selector");
}
int xh_rules_select_binary(const char*operation,char*out,int cap){static const char*regs[]={"t0","t0","t1"};int i,n;if(!operation||!out||cap<1)return error("invalid selection rule call");if(load())return -1;for(i=0;i<cache->count;i++){Select*r=&cache->rule[i];if(strcmp(r->op,operation))continue;n=snprintf(out,(size_t)cap,"    %s %s, %s, %s\n",r->mnemonic,regs[r->reg[0]],regs[r->reg[1]],regs[r->reg[2]]);if(n<0||n>=cap)return error("selection rule output exceeds buffer");return 1;}return 0;}
