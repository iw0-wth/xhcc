/* Independent bounded ARM ELF linker command. No frontend/backend registry. */
#include "xh_elf_link.h"
#include "xh_relocation.h"
#include "platform.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
void ark_log(const char*format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);}
void diag_tool(DiagSeverity severity,const char*where,const char*message,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){(void)severity;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;fprintf(stderr,"%s: %s\n",where,message);}
static int number(const char*s,uint32_t*out){char*end;unsigned long long value;if(!s||!*s||*s=='-')return -1;errno=0;value=strtoull(s,&end,0);if(*end||errno||value>UINT32_MAX)return -1;*out=(uint32_t)value;return 0;}
static void usage(void){puts("xhlinker - bounded ARM Thumb ELF32 linker\nUsage: xhlinker [-o image.elf] [-e entry] [--text ADDRESS] [--data ADDRESS]\n                [--data-lma ADDRESS] [--oformat elf|bin] [--max-memory BYTES]\n                [-T linker.ld] [--package-root PATH] [--relocation-rules PATH] object.o ...\nAliases: --text-address, --data-address, --data-load-address.\nExternal archives, host runtimes and non-ARM objects are unsupported.");}
int main(int argc,char**argv){const char*inputs[XH_ELF_LINK_MAX_OBJECTS],*root=getenv("XHCC_PACKAGE_ROOT"),*rules=0;unsigned count=0;XhElfLinkOptions options;char error[512],path[512];int i;memset(&options,0,sizeof(options));options.output_path="a.elf";
 for(i=1;i<argc;i++){const char*a=argv[i],*value=0;uint32_t n;
  if(!strncmp(a,"--oformat=",10)){value=a+10;a="--oformat";}
  if(!strcmp(a,"--script"))a="-T";
  if(!strcmp(a,"--text-address"))a="--text";else if(!strcmp(a,"--data-address"))a="--data";else if(!strcmp(a,"--data-load-address"))a="--data-lma";
  if(!strcmp(a,"--help")||!strcmp(a,"-h")){usage();return 0;}if(!strcmp(a,"--version")){puts("xhlinker 1.0 (ARM ELF32 bounded linker)");return 0;}
  if(!strcmp(a,"-o")||!strcmp(a,"-e")||!strcmp(a,"--entry")||!strcmp(a,"--text")||!strcmp(a,"--data")||!strcmp(a,"--data-lma")||!strcmp(a,"--oformat")||!strcmp(a,"--max-memory")||!strcmp(a,"--package-root")||!strcmp(a,"--relocation-rules")||!strcmp(a,"-T")){if(!value){if(++i>=argc){fprintf(stderr,"xhlinker: missing value for %s\n",a);return 2;}value=argv[i];}}
  if(!strcmp(a,"-o"))options.output_path=value;else if(!strcmp(a,"-e")||!strcmp(a,"--entry"))options.entry_name=value;else if(!strcmp(a,"--package-root"))root=value;else if(!strcmp(a,"--relocation-rules"))rules=value;else if(!strcmp(a,"-T"))options.script_path=value;
  else if(!strcmp(a,"--oformat")){if(!strcmp(value,"elf"))options.output_format=0;else if(!strcmp(value,"bin")||!strcmp(value,"binary"))options.output_format=1;else{fprintf(stderr,"xhlinker: invalid output format\n");return 2;}}
  else if(value){if(number(value,&n)){fprintf(stderr,"xhlinker: invalid numeric value for %s\n",a);return 2;}if(!strcmp(a,"--text"))options.text_addr=n;else if(!strcmp(a,"--data"))options.data_addr=n;else if(!strcmp(a,"--data-lma"))options.data_lma=n;else options.max_memory=n;}
  else if(a[0]=='-'){fprintf(stderr,"xhlinker: unsupported option %s\n",a);return 2;}else{if(count==XH_ELF_LINK_MAX_OBJECTS){fprintf(stderr,"xhlinker: too many objects\n");return 2;}inputs[count++]=a;}
 }
 if(!count){usage();return 2;}if(!rules){int n=snprintf(path,sizeof(path),"%s/arm/relocation.rules",root&&*root?root:"packages");if(n<0||n>=(int)sizeof(path))return 2;rules=path;}if(xh_relocation_set_path("arm",rules))return 2;error[0]=0;if(xh_elf_link_arm(inputs,count,&options,error,sizeof(error))){fprintf(stderr,"xhlinker: %s\n",error);return 1;}return 0;}
