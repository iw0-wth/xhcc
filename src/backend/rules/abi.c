/* Shared finite ABI decision relation. Rules retain source order because
 * overlapping ranges deliberately use first-match semantics. */
#include "xh_abi.h"
#include "xh_template.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef struct {unsigned short field[XH_ABI_MAX_FIELDS];} Record;
static Record *records;
static int record_count;
static unsigned generation;
static char active_target[32],active_prefix[80];
void xh_abi_release(void){free(records);records=0;record_count=0;generation=0;active_target[0]=active_prefix[0]=0;}
static int error(void){diag_tool(DIAG_SEV_ERROR,"ABI selector","invalid ABI decision table or input",0,0,0,0,0,0);return -1;}
static int ident(const char *s,size_t capacity){size_t n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'||*s=='-'))return 0;return n<capacity;}
static int space(char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
static int number(const char **text,unsigned *out){const char *p=*text;char *end;unsigned long value;while(space(*p))p++;if(*p<'0'||*p>'9')return -1;errno=0;value=strtoul(p,&end,10);if(errno||value>65535||end==p||(*end&&!space(*end)))return -1;*out=(unsigned)value;*text=end;return 0;}
static int done(const char *p){while(space(*p))p++;return !*p;}
static int load(const char *target,const char *prefix){char key[96];const char *text;unsigned count;int i,j;Record *next;
 snprintf(key,sizeof(key),"%s.count",prefix);text=xh_template_get(target,key);
 /* Lookup first: it may switch the template cache and change its generation. */
 if(!text)return -1;
 if(records&&!strcmp(target,active_target)&&!strcmp(prefix,active_prefix)&&generation==xh_template_generation())return 0;
 xh_abi_release();if(number(&text,&count)||!done(text)||count<1||count>XH_ABI_MAX_RECORDS)return error();
 next=calloc((size_t)count,sizeof(*next));if(!next)return error();
 for(i=0;i<(int)count;i++){
  unsigned short *v=next[i].field;
  snprintf(key,sizeof(key),"%s.%d",prefix,i);text=xh_template_get(target,key);if(!text)goto bad;
  for(j=0;j<XH_ABI_MAX_FIELDS;j++){unsigned value;if(number(&text,&value))goto bad;v[j]=(unsigned short)value;}
  if(!done(text)||v[0]>1||v[1]>4||v[2]>v[3]||v[4]>v[5]||v[6]>v[7]||v[8]>v[9]||v[10]>2||v[11]>2||v[12]>8||v[13]>16||v[14]>1||v[15])goto bad;
 }
 records=next;record_count=(int)count;generation=xh_template_generation();strcpy(active_target,target);strcpy(active_prefix,prefix);return 0;
 bad:free(next);return error();
}
int xh_abi_select(const char *target,const char *prefix,const XhAbiInput *in,XhAbiResult *out){int i;
 if(out)memset(out,0,sizeof(*out));
 if(!out||!in||!ident(target,sizeof(active_target))||!ident(prefix,sizeof(active_prefix))||in->role<0||in->role>1||in->kind<0||in->kind>4||in->size<0||in->size>65535||in->alignment<1||in->alignment>4096||in->variadic<0||in->variadic>1||in->index<0||in->index>65535)return error();
 if(load(target,prefix))return -1;
 for(i=0;i<record_count;i++){const unsigned short *v=records[i].field;
  if(in->role!=v[0]||in->kind!=v[1]||in->size<v[2]||in->size>v[3]||in->alignment<v[4]||in->alignment>v[5]||in->variadic<v[6]||in->variadic>v[7]||in->index<v[8]||in->index>v[9])continue;
  out->bank=v[10];out->words=v[11];out->width=v[12];out->alignment=v[13];out->indirect=v[14];return 1;
 }
 return 0;
}
