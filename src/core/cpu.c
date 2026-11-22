/* Ark C Compiler - cpu
 * Target CPU selection: look up CPU info via --cpu/-mcpu and apply predefined macros.
 *
 * Covers common Cortex-M cores from armcc 5: M0/M0+/M1/M3/M4/M7/M23/M33.
 * The chosen CPU sets values of __TARGET_ARCH_*, __TARGET_FEATURE_DSP,
 * __TARGET_FPU_VFP etc., and affects runtime switches like g_config.enable_fpu_hw.
 */
#include "cpu.h"
#include "preproc.h"
#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#define CPU_CAP 64
#define CPU_STORAGE 16384
static CpuInfo cpu_table[CPU_CAP];
static const char *cpu_aliases[CPU_CAP];
static unsigned cpu_seen[CPU_CAP];
static int cpu_count, cpu_loaded;
static char cpu_path[512];
static char cpu_storage[CPU_STORAGE];
static size_t cpu_used;
static int cpu_strcaseeq(const char *a,const char *b) {
 while(*a&&*b) {if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;}return !*a&&!*b;
}
static char *cpu_trim(char *s){char *e;while(isspace((unsigned char)*s))s++;e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;}
static const char *cpu_save(const char *s){size_t n=strlen(s)+1;char *p;if(n>sizeof(cpu_storage)-cpu_used)return 0;p=cpu_storage+cpu_used;memcpy(p,s,n);cpu_used+=n;return p;}
int cpu_set_description_path(const char *path) {
 if(path&&(!*path||strlen(path)>=sizeof(cpu_path)))return 1;
 if(path)strcpy(cpu_path,path);else cpu_path[0]=0;
 cpu_loaded=0;cpu_count=0;cpu_used=0;return 0;
}
static int cpu_alias_match(const char *list,const char *name){
 while(list&&*list){const char *e=strchr(list,',');size_t n=e?(size_t)(e-list):strlen(list);size_t i;if(strlen(name)==n){for(i=0;i<n;i++)if(tolower((unsigned char)list[i])!=tolower((unsigned char)name[i]))break;if(i==n)return 1;}list=e?e+1:0;}return 0;
}
static int cpu_validate_names(void) {
 int i,j;
 for(i=0;i<cpu_count;i++) {
  const char *p=cpu_aliases[i];
  if(strchr(cpu_table[i].name,',')||strpbrk(cpu_table[i].name," \t\r\n"))return 1;
  for(j=0;j<i;j++)if(cpu_strcaseeq(cpu_table[i].name,cpu_table[j].name)||cpu_alias_match(cpu_aliases[j],cpu_table[i].name))return 1;
  while(p&&*p) {
   const char *end=strchr(p,',');size_t n=end?(size_t)(end-p):strlen(p);char alias[128];
   if(!n||n>=sizeof(alias))return 1;memcpy(alias,p,n);alias[n]=0;
   if(strpbrk(alias," \t\r\n")||cpu_strcaseeq(alias,cpu_table[i].name))return 1;
   for(j=0;j<cpu_count;j++)if(j!=i&&(cpu_strcaseeq(alias,cpu_table[j].name)||cpu_alias_match(cpu_aliases[j],alias)))return 1;
   if(end&&(!end[1]||cpu_alias_match(end+1,alias)))return 1;
   p=end?end+1:0;
  }
 }
 return 0;
}
static int cpu_finish(int index) {
 CpuInfo *c=&cpu_table[index];unsigned required=(1u<<11)-1u;
 if(cpu_seen[index]!=required||!c->name||!*c->name||c->arch_m<6||c->arch_m>8||
    (c->has_fpu_dp&&!c->has_fpu)||(!c->has_fpu&&c->fpu_version)||
    (c->has_fpu&&((c->fpu_version!=4&&c->fpu_version!=5)||!c->fpu_name||!*c->fpu_name))||
    (c->thumb2&&c->arch_m==6))return 1;
 return 0;
}
static int cpu_load(void) {
 FILE *f;char line[1024];int current=-1,version=0;const char *path=*cpu_path?cpu_path:"packages/arm/cpus.rules";
 cpu_loaded=-1;cpu_count=0;cpu_used=0;memset(cpu_table,0,sizeof(cpu_table));memset(cpu_seen,0,sizeof(cpu_seen));memset(cpu_aliases,0,sizeof(cpu_aliases));
 f=fopen(path,"rb");if(!f){fprintf(stderr,"xhcc: cannot open CPU descriptions: %s\n",path);return 1;}
 while(fgets(line,sizeof(line),f)) {
  char *s,*eq,*v;int field=-1,number=0;size_t len;
  if(!strchr(line,'\n')&&!feof(f))goto invalid;
  s=cpu_trim(line);len=strlen(s);
  if(!*s||*s=='#'||*s==';')continue;
  if(*s=='['){if(!version||len<3||s[len-1]!=']'||cpu_count==CPU_CAP)goto invalid;if(current>=0&&cpu_finish(current))goto invalid;current=cpu_count++;continue;}
  eq=strchr(s,'=');if(!eq)goto invalid;*eq=0;v=cpu_trim(eq+1);s=cpu_trim(s);
  if(current<0){if(version||strcmp(s,"version")||strcmp(v,"1"))goto invalid;version=1;continue;}
  if(!strcmp(s,"name"))field=0;else if(!strcmp(s,"aliases"))field=1;
  else if(!strcmp(s,"arch_m"))field=2;else if(!strcmp(s,"arch_em"))field=3;
  else if(!strcmp(s,"has_dsp"))field=4;else if(!strcmp(s,"has_fpu"))field=5;
  else if(!strcmp(s,"has_fpu_dp"))field=6;else if(!strcmp(s,"thumb_only"))field=7;
  else if(!strcmp(s,"thumb2"))field=8;else if(!strcmp(s,"fpu_version"))field=9;
  else if(!strcmp(s,"fpu_name"))field=10;
  else goto invalid;
  if(cpu_seen[current]&(1u<<field))goto invalid;cpu_seen[current]|=1u<<field;
  if(field==0||field==1||field==10){const char *copy=cpu_save(v);if(!copy)goto invalid;
   if(field==0)cpu_table[current].name=copy;else if(field==1)cpu_aliases[current]=copy;else cpu_table[current].fpu_name=*copy?copy:0;
  } else {
   if(!*v||v[1]||*v<'0'||*v>'8')goto invalid;number=*v-'0';
   if(field!=2&&field!=9&&number>1)goto invalid;
   switch(field){case 2:cpu_table[current].arch_m=number;break;case 3:cpu_table[current].arch_em=number;break;
    case 4:cpu_table[current].has_dsp=number;break;case 5:cpu_table[current].has_fpu=number;break;
    case 6:cpu_table[current].has_fpu_dp=number;break;case 7:cpu_table[current].thumb_only=number;break;
    case 8:cpu_table[current].thumb2=number;break;case 9:cpu_table[current].fpu_version=number;break;}
  }
 }
 if(ferror(f)||!version||current<0||cpu_finish(current)||cpu_validate_names())goto invalid;
 fclose(f);cpu_loaded=1;return 0;
invalid:
 fclose(f);cpu_count=0;fprintf(stderr,"xhcc: invalid CPU descriptions: %s\n",path);return 1;
}
const CpuInfo *cpu_lookup(const char *name) {
 int i;if(!name||!*name)return 0;
 if(!cpu_loaded&&cpu_load())return 0;if(cpu_loaded<0)return 0;
 for(i=0;i<cpu_count;i++)if(cpu_strcaseeq(name,cpu_table[i].name)||cpu_alias_match(cpu_aliases[i],name))return &cpu_table[i];
 return 0;
}

/* Convert integer to string into a static buffer (used for macro bodies, immediate values only) */
static void int_to_str(int v, char *out) {
    int n = 0;
    char tmp[16];
    int i;
    unsigned u;
    if (v < 0) { out[0] = '-'; out++; u = (unsigned)(-v); }
    else u = (unsigned)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    for (i = n - 1; i >= 0; i--) out[n - 1 - i] = tmp[i];
    out[n] = 0;
}

void cpu_apply_predefines(const CpuInfo *cpu) {
    char buf[16];
    if (!cpu) return;

    /* Clear all old __TARGET_ARCH_* / __TARGET_FEATURE_* / __TARGET_FPU_* macros,
     * then redefine per the current CPU. This overrides the hardcoded M7 defaults in preproc_process. */
    preproc_undef("__TARGET_ARCH_6_M");
    preproc_undef("__TARGET_ARCH_7_M");
    preproc_undef("__TARGET_ARCH_7E_M");
    preproc_undef("__TARGET_ARCH_8_M");
    preproc_undef("__TARGET_ARCH_8_M_BASE");
    preproc_undef("__TARGET_ARCH_8_M_MAIN");
    preproc_undef("__TARGET_FEATURE_DSP");
    preproc_undef("__TARGET_FPU_VFP");
    preproc_undef("__TARGET_FPU_FPV5");
    preproc_undef("__TARGET_FPU_DP");

    /* Define architecture macro based on arch_m value */
    switch (cpu->arch_m) {
        case 6:
            preproc_define("__TARGET_ARCH_6_M", "1");
            break;
        case 7:
            if (cpu->arch_em) preproc_define("__TARGET_ARCH_7E_M", "1");
            else              preproc_define("__TARGET_ARCH_7_M", "1");
            break;
        case 8:
            if (cpu->arch_em) preproc_define("__TARGET_ARCH_8_M_MAIN", "1");
            else              preproc_define("__TARGET_ARCH_8_M_BASE", "1");
            preproc_define("__TARGET_ARCH_8_M", "1");
            break;
        default:
            break;
    }

    /* DSP extension */
    if (cpu->has_dsp) {
        preproc_define("__TARGET_FEATURE_DSP", "1");
    }

    /* FPU flags: single/double precision */
    if (cpu->has_fpu) {
        preproc_define("__TARGET_FPU_VFP", "1");
        /* M7 defaults to VFPv5-D16; M4 is VFPv4-SP; M33 is VFPv5-SP.
         * Simplification: if fpu_name is non-null, define an extra FPU unit macro. */
        if (cpu->fpu_version == 5) preproc_define("__TARGET_FPU_FPV5", "1");
        if (cpu->has_fpu_dp) {
            preproc_define("__TARGET_FPU_DP", "1");
        }
    }

    /* Define a __TARGET_CPU reflection macro for user-code diagnostics (non-standard, arkcc extension) */
    preproc_define("__TARGET_CPU_NAME", cpu->name);

    /* Define __TARGET_ARCH_M (major version number) as an integer string for #if comparisons */
    int_to_str(cpu->arch_m, buf);
    preproc_define("__TARGET_ARCH_M", buf);
}
