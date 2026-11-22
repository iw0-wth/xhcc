/* Bounded external target template engine. One table resident at a time. */
#include "xh_template.h"
#include "diag.h"
#include "flash_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct {char *key,*text;} Entry;
typedef struct {char target[32],path[512];} Location;
static Location locations[8];static int location_count;
static Entry *entries;static int entry_count;
static char *storage;static char loaded_target[32],loaded_path[512];
static unsigned generation=1;
unsigned xh_template_generation(void){return generation;}
static int error(const char *path,const char *msg){diag_tool(DIAG_SEV_ERROR,path?path:"xhcc templates",msg,0,0,0,0,0,0);return -1;}
void xh_template_release(void){generation++;if(!generation)generation=1;free(entries);free(storage);entries=0;storage=0;entry_count=0;loaded_target[0]=0;loaded_path[0]=0;}
static int identifier(const char *s,int max){int n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'||*s=='-'))return 0;return n<max;}
int xh_template_set_path(const char *target,const char *path){int i;size_t n=path?strlen(path):0;if(!identifier(target,32)||n>=512)return error("xhcc templates","invalid target or template path");for(i=0;i<location_count;i++)if(!strcmp(target,locations[i].target))break;if(i==location_count){if(i==8)return error("xhcc templates","too many target template locations");strcpy(locations[i].target,target);location_count++;}if(strcmp(locations[i].path,path?path:"")&&!strcmp(loaded_target,target))xh_template_release();if(n)memcpy(locations[i].path,path,n);locations[i].path[n]=0;return 0;}
static const char *path_for(const char *target,char *fallback,size_t cap){int i;for(i=0;i<location_count;i++)if(!strcmp(target,locations[i].target)&&locations[i].path[0])return locations[i].path;if(!strcmp(target,"arm"))return "packages/arm/arm.rules";if(!strcmp(target,"x86")||!strcmp(target,"x86_64")){snprintf(fallback,cap,"packages/%s/x86.rules",target);return fallback;}if(!strcmp(target,"c"))return "packages/c/c.rules";if(!strcmp(target,"riscv32"))return "packages/riscv32/rv.rules";return 0;}
static int hex(int c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int decode(const char *src,char **dest,char *end){while(*src){unsigned c=(unsigned char)*src++;if(c=='\\'){c=(unsigned char)*src++;if(!c)return -1;if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='\\')c='\\';else if(c=='"')c='"';else if(c=='x'){int a=hex((unsigned char)src[0]),b=src[0]?hex((unsigned char)src[1]):-1;if(a<0||b<0)return -1;c=(unsigned)(a*16+b);src+=2;if(!c)return -1;}else return -1;}if(*dest>=end)return -1;*(*dest)++=(char)c;}return 0;}
static int placeholders(const char *s){while(*s){if(*s++!='$')continue;if(*s=='$'){s++;continue;}if(*s++!='{')return 0;{unsigned n=0;int digits=0;while(*s>='0'&&*s<='9'){n=n*10u+(unsigned)(*s++-'0');if(n>=XH_TEMPLATE_MAX_ARGS)return 0;digits++;}if(!digits||*s++!='}')return 0;}}return 1;}
static int entry_compare(const void *a,const void *b){return strcmp(((const Entry*)a)->key,((const Entry*)b)->key);}
static int load(const char *target){char fallback[512];const char *path;char *source,*p,*next,*write,*end;int size,n,version=0,open=0;Entry *table;char *data;int count=0;
 if(!identifier(target,32))return error("xhcc templates","invalid template target");path=path_for(target,fallback,sizeof(fallback));if(!path)return error("xhcc templates","no templates configured for target");if(entries&&!strcmp(loaded_target,target)&&!strcmp(loaded_path,path))return 0;
 size=fs_file_size(path);if(size<=0||(unsigned)size>XH_TEMPLATE_MAX_BYTES)return error(path,"missing template file or file exceeds 512 KiB");
 /* A target switch invalidates pointers by contract. Release before loading its
  * successor, rather than retaining two target tables during the parse. */
 xh_template_release();
 source=malloc((size_t)size+1);data=malloc((size_t)size+1);table=0;
 if(!source||!data){free(source);free(data);return error(path,"not enough memory for target templates");}
 n=fs_read_file(path,source,size+1);if(n!=size||memchr(source,0,(size_t)size))goto invalid;
 /* Allocate entries for actual rule records, not the maximum package capacity. */
 {const char *scan=source;unsigned records=0;while(*scan){if(!strncmp(scan,"rule ",5))records++;scan=strchr(scan,'\n');if(!scan)break;scan++;}if(!records||records>XH_TEMPLATE_MAX_RULES)goto invalid;table=calloc(records,sizeof(*table));if(!table){free(source);free(data);return error(path,"not enough memory for target template index");}}
 p=source;write=data;end=data+size;
 while(*p){size_t len;next=strchr(p,'\n');if(next)*next++=0;len=strlen(p);if(len&&p[len-1]=='\r')p[--len]=0;
  if(!version){if(!*p||*p=='#')goto line_done;if(strcmp(p,"XHTEMPLATE 1"))goto invalid;version=1;goto line_done;}
  if(!open){if(!*p||*p=='#')goto line_done;if(strncmp(p,"rule ",5)||!identifier(p+5,96)||count>=XH_TEMPLATE_MAX_RULES)goto invalid;{int i;for(i=0;i<count;i++)if(!strcmp(table[i].key,p+5))goto invalid;}table[count].key=write;if((size_t)(end-write)<strlen(p+5)+2)goto invalid;strcpy(write,p+5);write+=strlen(write)+1;table[count].text=write;open=1;}
  else if(!strcmp(p,"end")){if(write>=end)goto invalid;*write++=0;if(!placeholders(table[count].text))goto invalid;count++;open=0;}
  else if(!strncmp(p,"text ",5)){if(decode(p+5,&write,end))goto invalid;}
  else if(!strcmp(p,"text")){}else goto invalid;
line_done:if(!next)break;p=next;
 }
 if(!version||open||!count)goto invalid;qsort(table,(size_t)count,sizeof(*table),entry_compare);free(source);xh_template_release();entries=table;storage=data;entry_count=count;strcpy(loaded_target,target);strcpy(loaded_path,path);return 0;
invalid:free(source);free(data);free(table);return error(path,"invalid template version, syntax, escape, placeholder, duplicate key or capacity");
}
const char *xh_template_get(const char *target,const char *key){int lo=0,hi;if(!key||load(target))return 0;hi=entry_count;while(lo<hi){int mid=lo+(hi-lo)/2;int order=strcmp(entries[mid].key,key);if(!order)return entries[mid].text;if(order<0)lo=mid+1;else hi=mid;}error(loaded_path,"required target template is missing");return 0;}
int xh_template_emit(const char *target,const char *key,const char *const *args,int count,char *out,int cap){const char *p;int n=0;if(count<0||count>XH_TEMPLATE_MAX_ARGS||!out||cap<1)return error("xhcc templates","invalid template expansion arguments");p=xh_template_get(target,key);if(!p)return -1;while(*p){if(*p!='$'){if(n>=cap-1)goto overflow;out[n++]=*p++;continue;}p++;if(*p=='$'){if(n>=cap-1)goto overflow;out[n++]='$';p++;continue;}{unsigned index=0;const char *s;p++;while(*p>='0'&&*p<='9')index=index*10u+(unsigned)(*p++-'0');p++;if(index>=(unsigned)count||!args||!args[index])return error(loaded_path,"missing template substitution argument");s=args[index];while(*s){if(n>=cap-1)goto overflow;out[n++]=*s++;}}}out[n]=0;return n;
overflow:out[0]=0;return error(loaded_path,"target template expansion exceeds output capacity");}
