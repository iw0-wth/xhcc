/* CompileRequest v1 wire format: magic[8]="XHREQ1\r\n", u16 major,
 * u16 minor, u32 payload bytes. Each record: u16 key bytes, u8 kind
 * (1=i64,2=string,3=string array), u8 reserved=0, u32 value bytes,
 * key then value. Strings are raw UTF-8/non-NUL bytes; null=0xffffffff.
 * Arrays: u32 count followed by u32 byte length + bytes per string.
 * All integers are little-endian, independent of host layout. */
#include "xh_request.h"
#include "diag.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *key; size_t offset; int kind; size_t count_offset; unsigned cap; } Field;
#define I(T,n) {#n,offsetof(T,n),1,0,0}
#define L(T,n) {#n,offsetof(T,n),4,0,0}
#define S(T,n) {#n,offsetof(T,n),2,0,0}
#define A(T,n,c,cap) {#n,offsetof(T,n),3,offsetof(T,c),cap}
/* Tables below enumerate the protocol schema, not the in-memory representation. */
static const Field option_fields[] = {
 I(ArkOptions,emit_asm), I(ArkOptions,verbose), I(ArkOptions,list_backends),
 I(ArkOptions,compile_only), I(ArkOptions,preprocess_only), I(ArkOptions,link_mode),
 I(ArkOptions,vector_table), I(ArkOptions,opt_level), I(ArkOptions,debug_info),
 I(ArkOptions,no_unused_warn), I(ArkOptions,gen_depend), I(ArkOptions,split_sections),
 I(ArkOptions,print_pipeline), I(ArkOptions,ipo_mode), I(ArkOptions,obj_ir),
 I(ArkOptions,lto_link), I(ArkOptions,partial_link), L(ArkOptions,build_epoch),
 I(ArkOptions,private_object), I(ArkOptions,no_stdinc), I(ArkOptions,dump_macros),
 I(ArkOptions,no_line_markers), I(ArkOptions,hosted_mode), I(ArkOptions,undef_predefines),
 I(ArkOptions,no_builtin), I(ArkOptions,language_c11), I(ArkOptions,emulate_gcc),
 I(ArkOptions,use_sysv_abi_plan), I(ArkOptions,record_sysv_abi), I(ArkOptions,memory_report), I(ArkOptions,syntax_only), I(ArkOptions,deps_only), I(ArkOptions,dep_phony),
 I(ArkOptions,dep_quote_target), I(ArkOptions,include_system_deps),
 S(ArkOptions,input_path), S(ArkOptions,output_path), S(ArkOptions,target), S(ArkOptions,cpu),
 S(ArkOptions,depend_file), S(ArkOptions,scatter_file), S(ArkOptions,linker_script),
 S(ArkOptions,uvprojx), S(ArkOptions,build_date), S(ArkOptions,build_time), S(ArkOptions,depend_target),
 A(ArkOptions,include_dirs,include_dir_count,ARK_PP_MAX_INC_DIRS),
 A(ArkOptions,link_inputs,link_input_count,ARK_LINK_MAX_OBJS),
 A(ArkOptions,ipo_inputs,ipo_input_count,ARK_LINK_MAX_OBJS),
 A(ArkOptions,pre_defines,pre_define_count,ARK_MAX_PRE_DEFINES),
 A(ArkOptions,pre_undefines,pre_undefine_count,ARK_MAX_PRE_DEFINES),
 A(ArkOptions,macro_actions,macro_action_count,ARK_MAX_PRE_DEFINES),
 A(ArkOptions,forced_includes,forced_include_count,ARK_PP_MAX_INC_DIRS),
 A(ArkOptions,quote_include_dirs,quote_include_dir_count,ARK_PP_MAX_INC_DIRS),
 A(ArkOptions,system_include_dirs,system_include_dir_count,ARK_PP_MAX_INC_DIRS)
};
static const Field config_fields[] = {
 I(ArkccConfig,enable_vla), I(ArkccConfig,enable_complex), I(ArkccConfig,enable_wchar),
 I(ArkccConfig,enable_trigraph), I(ArkccConfig,enable_fpu_hw), I(ArkccConfig,enable_dwarf),
 I(ArkccConfig,enable_opt_constfold), I(ArkccConfig,enable_opt_dce),
 I(ArkccConfig,enable_opt_regalloc), I(ArkccConfig,enable_opt_regalloc_near),
 I(ArkccConfig,enable_opt_vstack), I(ArkccConfig,enable_opt_addr), I(ArkccConfig,enable_opt_strength),
 I(ArkccConfig,enable_opt_cbz), I(ArkccConfig,enable_opt_branch), I(ArkccConfig,enable_opt_imm),
 I(ArkccConfig,enable_opt_peep), I(ArkccConfig,enable_opt_cbranch), I(ArkccConfig,enable_opt_copyprop),
 I(ArkccConfig,enable_opt_live), I(ArkccConfig,enable_opt_inline), I(ArkccConfig,enable_opt_vectorize),
 I(ArkccConfig,opt_level), I(ArkccConfig,debug_info), I(ArkccConfig,split_sections),
 I(ArkccConfig,enum_is_int), I(ArkccConfig,dollar_in_identifiers), I(ArkccConfig,signed_chars),
 I(ArkccConfig,unsigned_chars), I(ArkccConfig,wchar16), I(ArkccConfig,wchar32), I(ArkccConfig,dwarf_version),
 I(ArkccConfig,enable_dce_cfg), I(ArkccConfig,enable_reloc_data), I(ArkccConfig,enable_reloc_text),
 I(ArkccConfig,enable_reg_top), I(ArkccConfig,enable_rt_lazy), I(ArkccConfig,enable_asm_extra),
 I(ArkccConfig,enable_memlint), I(ArkccConfig,enable_enumcheck), I(ArkccConfig,enable_unused_result),
 I(ArkccConfig,enable_warn_unreachable), I(ArkccConfig,enable_warn_unused_static),
 I(ArkccConfig,enable_warn_recursion), I(ArkccConfig,enable_licm), I(ArkccConfig,enable_unroll),
 I(ArkccConfig,enable_tailmerge), I(ArkccConfig,enable_xblock), I(ArkccConfig,enable_tailcall),
 I(ArkccConfig,enable_divconst), I(ArkccConfig,enable_const_global),
#if ARKCC_FEATURE_SIZEWEIGHT
 I(ArkccConfig,size_weight),
#endif
 I(ArkccConfig,pipeline_profile), S(ArkccConfig,config_file), S(ArkccConfig,scatter_file),
 S(ArkccConfig,linker_script), S(ArkccConfig,fpu_type)
};
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
static unsigned char wire[XH_REQUEST_MAX_BYTES];
static char strings[XH_REQUEST_MAX_BYTES];
static size_t used, cursor, strings_used;
static const char *request_tool;
static uint32_t fingerprint;
uint32_t xh_request_fingerprint(void){return fingerprint;}
static uint32_t hash_payload(size_t n){uint32_t h=2166136261u;size_t i;for(i=16;i<n;i++){h^=wire[i];h*=16777619u;}return h;}
typedef struct {const char *args[512];int count;} DiagnosticArgs;
static DiagnosticArgs diagnostic_args;
int xh_request_add_diagnostic_arg(const char *arg) {
 if(!arg){diagnostic_args.count=0;return 0;}
 if(diagnostic_args.count==512)return -1;
 diagnostic_args.args[diagnostic_args.count++]=arg;return 0;
}
int xh_request_diagnostic_arg_count(void){return diagnostic_args.count;}
const char *xh_request_diagnostic_arg(int index){return index>=0&&index<diagnostic_args.count?diagnostic_args.args[index]:0;}
void xh_request_set_tool(const char *tool) { request_tool = tool; }
const char *xh_request_tool(void) { return request_tool; }
/* Runtime warning switches already live in the transported g_config. Only
 * diagnostic presentation/severity/sinks need replay; do not re-enable passes. */
void xh_request_apply_diagnostics(void) {
 int i;
 diag_reset_policy();
 for(i=0;i<diagnostic_args.count;i++) {
  const char *a=diagnostic_args.args[i];
  if(!strcmp(a,"-Werror"))diag_set_warnings_as_errors(1);
  else if(!strcmp(a,"-Wno-error"))diag_set_warnings_as_errors(0);
  else if(!strcmp(a,"-w"))diag_set_warnings_disabled(1);
  else if(!strcmp(a,"-Wcpp"))diag_default(1215);
  else if(!strcmp(a,"-Wno-cpp"))diag_suppress(1215);
  else if(!strcmp(a,"--diag_style")||!strncmp(a,"--diag_style=",13)) {
   const char *v=a[12]=='='?a+13:(i+1<diagnostic_args.count?diagnostic_args.args[++i]:0);
   if(!v||diag_set_style_name(v))diag_report_msg(DIAG_SEV_ERROR,0,"invalid diagnostic style in CompileRequest",0,0,0);
  } else if(!strcmp(a,"--errors")||!strncmp(a,"--errors=",9)) {
   const char *v=a[8]=='='?a+9:(i+1<diagnostic_args.count?diagnostic_args.args[++i]:0);
   if(!v||diag_open_sink(v))diag_report_msg(DIAG_SEV_ERROR,0,"cannot open diagnostic sink in CompileRequest",0,0,0);
  } else {
   static const char *policies[]={"--diag_suppress=","--diag_remark=","--diag_error=",0};
   int j;
   for(j=0;policies[j];j++)if(!strncmp(a,policies[j],strlen(policies[j]))) {
    const char *p=a+strlen(policies[j]);
    while(*p) {
     int code=0;
     if(!isdigit((unsigned char)*p)) {diag_report_msg(DIAG_SEV_ERROR,0,"invalid diagnostic policy in CompileRequest",0,0,0);break;}
     while(isdigit((unsigned char)*p)) {if(code>100000)break;code=code*10+(*p++-'0');}
     if(code>100000) {diag_report_msg(DIAG_SEV_ERROR,0,"diagnostic code out of range in CompileRequest",0,0,0);break;}
     if(j==2)diag_force_error(code);else diag_suppress(code);
     if(*p&&(*p++!=','||!*p)) {diag_report_msg(DIAG_SEV_ERROR,0,"invalid diagnostic policy in CompileRequest",0,0,0);break;}
    }
    break;
   }
  }
 }
}

static int put(uint64_t v, unsigned n) {
 unsigned i; if (n > sizeof(wire)-used) return -1;
 for(i=0;i<n;i++) { wire[used++]=(unsigned char)v; v>>=8; } return 0;
}
static int bytes(const void *p,size_t n) {
 if(n>sizeof(wire)-used) return -1;
 if(n) memcpy(wire+used,p,n); used+=n; return 0;
}
static int string_write(const char *p) {
 size_t n; if(!p) return put(UINT32_MAX,4);
 n=strlen(p); if(n>65535) return -1;
 return put(n,4)||bytes(p,n) ? -1:0;
}
static int record(const char *prefix,const Field *f,const void *base) {
 const unsigned char *b=(const unsigned char *)base;
 size_t keylen=strlen(prefix)+strlen(f->key), length_at, start;
 int kind=f->kind==4?1:f->kind;
 if(put(keylen,2)||put(kind,1)||put(0,1)) return -1;
 length_at=used; if(put(0,4)||bytes(prefix,strlen(prefix))||bytes(f->key,strlen(f->key)))return -1;
 start=used;
 if(kind==1) {
  int64_t v; if(f->kind==4) {long long x; memcpy(&x,b+f->offset,sizeof(x));v=x;}
  else {int x;memcpy(&x,b+f->offset,sizeof(x));v=x;}
  if(put((uint64_t)v,8))return -1;
 } else if(kind==2) {
  const char *p;memcpy(&p,b+f->offset,sizeof(p)); if(string_write(p))return -1;
 } else {
  int count,i; const char *const *array=(const char *const *)(b+f->offset);
  memcpy(&count,b+f->count_offset,sizeof(count));
  if(count<0||(unsigned)count>f->cap||put((unsigned)count,4))return -1;
  for(i=0;i<count;i++)if(!array[i]||string_write(array[i]))return -1;
 }
 {size_t end=used;used=length_at;if(put(end-start,4))return -1;used=end;}
 return 0;
}
int xh_request_write(const ArkOptions *opts,const char *path) {
 FILE *fp;size_t i;Field tool={"tool",0,2,0,0};
 if(!opts||!path||!*path)return -1;
 used=0;if(bytes("XHREQ1\r\n",8)||put(XH_REQUEST_MAJOR,2)||put(XH_REQUEST_MINOR,2)||put(0,4))return -1;
 if(record("",&tool,&request_tool))return -1;
 {Field d={"diagnostic_args",offsetof(DiagnosticArgs,args),3,offsetof(DiagnosticArgs,count),512};if(record("",&d,&diagnostic_args))return -1;}
 for(i=0;i<COUNT(option_fields);i++)if(record("o.",&option_fields[i],opts))return -1;
 for(i=0;i<COUNT(config_fields);i++)if(record("c.",&config_fields[i],&g_config))return -1;
 {size_t end=used;used=12;if(put(end-16,4))return -1;used=end;}
 fp=fopen(path,"wb");if(!fp)return -1;
 i=fwrite(wire,1,used,fp); {int rc=fclose(fp);if(i!=used||rc)return -1;fingerprint=hash_payload(used);return 0;}
}
static int get(uint64_t *v,unsigned n,size_t end) {
 unsigned i;if(cursor>end||n>end-cursor)return -1;*v=0;
 for(i=0;i<n;i++)*v|=(uint64_t)wire[cursor++]<<(8*i);return 0;
}
static int string_read(const char **p,size_t end) {
 uint64_t n;if(get(&n,4,end))return -1;
 if(n==UINT32_MAX){*p=0;return 0;}
 if(n>65535||n>end-cursor||n+1>sizeof(strings)-strings_used||memchr(wire+cursor,0,(size_t)n))return -1;
 *p=strings+strings_used;memcpy(strings+strings_used,wire+cursor,(size_t)n);
 strings_used+=(size_t)n;strings[strings_used++]=0;cursor+=(size_t)n;return 0;
}
static int field_read(const Field *f,void *base,unsigned kind,size_t end) {
 unsigned char *b=(unsigned char *)base;uint64_t v;
 if(kind!=(unsigned)(f->kind==4?1:f->kind))return -1;
 if(kind==1){if(get(&v,8,end))return -1;
  if(f->kind==4){long long x=(long long)v;memcpy(b+f->offset,&x,sizeof(x));}
  else {int64_t s=(int64_t)v;int x;if(s<INT32_MIN||s>INT32_MAX)return -1;x=(int)s;memcpy(b+f->offset,&x,sizeof(x));}
 }else if(kind==2){const char *p;if(string_read(&p,end))return -1;memcpy(b+f->offset,&p,sizeof(p));}
 else {unsigned i;int count;const char **array=(const char **)(b+f->offset);
  if(get(&v,4,end)||v>f->cap)return -1;count=(int)v;memcpy(b+f->count_offset,&count,sizeof(count));
  for(i=0;i<(unsigned)count;i++)if(string_read(&array[i],end)||!array[i])return -1;
 }return cursor==end?0:-1;
}
int xh_request_read(const char *path,ArkOptions *opts) {
 FILE *fp;size_t n,i;uint64_t major,minor,length;ArkOptions next;ArkccConfig cfg;
 unsigned char seen_o[COUNT(option_fields)]={0},seen_c[COUNT(config_fields)]={0};int seen_tool=0;
 const char *tool_value=0;DiagnosticArgs next_diagnostics;int seen_diagnostics=0;
 memset(&next_diagnostics,0,sizeof(next_diagnostics));
 if(!path||!opts)return -1;fp=fopen(path,"rb");if(!fp)return -1;
 n=fread(wire,1,sizeof(wire),fp);if(ferror(fp)||fgetc(fp)!=EOF){fclose(fp);return -1;}fclose(fp);
 if(n<16||memcmp(wire,"XHREQ1\r\n",8))return -1;
 cursor=8;if(get(&major,2,n)||get(&minor,2,n)||get(&length,4,n)||major!=XH_REQUEST_MAJOR||minor>XH_REQUEST_MINOR||length!=n-16)return -1;
 memset(&next,0,sizeof(next));memset(&cfg,0,sizeof(cfg));strings_used=0;
 while(cursor<n){uint64_t kl,kind,reserved,vl;char key[128];size_t end;const Field *f=0;void *base=0;unsigned char *seen=0;
  if(get(&kl,2,n)||get(&kind,1,n)||get(&reserved,1,n)||get(&vl,4,n)||reserved||!kl||kl>=sizeof(key)||kl>n-cursor)return -1;
  memcpy(key,wire+cursor,(size_t)kl);key[kl]=0;cursor+=(size_t)kl;
  if(memchr(key,0,(size_t)kl)||vl>n-cursor)return -1;end=cursor+(size_t)vl;
  if(!strcmp(key,"tool")){if(seen_tool++||kind!=2||string_read(&tool_value,end)||cursor!=end)return -1;continue;}
  if(!strcmp(key,"diagnostic_args")){Field d={"diagnostic_args",offsetof(DiagnosticArgs,args),3,offsetof(DiagnosticArgs,count),512};if(seen_diagnostics++||field_read(&d,&next_diagnostics,(unsigned)kind,end))return -1;continue;}
  if(!strncmp(key,"o.",2))for(i=0;i<COUNT(option_fields);i++)if(!strcmp(key+2,option_fields[i].key)){f=&option_fields[i];base=&next;seen=&seen_o[i];break;}
  if(!strncmp(key,"c.",2))for(i=0;i<COUNT(config_fields);i++)if(!strcmp(key+2,config_fields[i].key)){f=&config_fields[i];base=&cfg;seen=&seen_c[i];break;}
  if(!f||(*seen)++||field_read(f,base,(unsigned)kind,end))return -1;
 }
 for(i=0;i<COUNT(option_fields);i++)if(!seen_o[i])return -1;
 for(i=0;i<COUNT(config_fields);i++)if(!seen_c[i])return -1;
 if(!seen_tool||!seen_diagnostics||!tool_value||!next.input_path||!next.target)return -1;
 *opts=next;g_config=cfg;request_tool=tool_value;diagnostic_args=next_diagnostics;fingerprint=hash_payload(n);return 0;
}
