/* Data-only package argument aliases, bounded and independent of compiler core. */
#include "xh_package_options.h"
#include "xh_driver.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#define MAX_RULES 128
#define RULE_BYTES 32768
typedef struct {const char *match,*emit;int mode;} Rule;
static Rule rules[MAX_RULES];
static int rule_count;
static char rule_storage[RULE_BYTES], mapped_storage[XH_DRIVER_ARG_BYTES];
static char *mapped[XH_DRIVER_MAX_ARGS];
static size_t rule_used,mapped_used;
static int error(const char *message){fprintf(stderr,"xhcc package options: %s\n",message);return 1;}
static char *trim(char *s){char *e;while(isspace((unsigned char)*s))s++;e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;}
static const char *save(const char *s){size_t n=strlen(s)+1;char *p;if(n>sizeof(rule_storage)-rule_used)return 0;p=rule_storage+rule_used;memcpy(p,s,n);rule_used+=n;return p;}
static int reserved_token(const char *p,size_t n) {
 static const char *names[]={"--package-root","--target","-target","-m32","-m64",0};
 int i;for(i=0;names[i];i++){size_t k=strlen(names[i]);if(n>=k&&!strncmp(p,names[i],k)&&(n==k||p[k]=='='))return 1;}return 0;
}
int xh_package_options_takes_value(const char *arg) {
 static const char *names[]={"-o","-I","-D","-U","-include","-MF","-MT","-MQ","--cpu","-mcpu","--fpu","-mfpu","-config","--package-root","-x","-target","--target","-march","-mabi","-isystem","-iquote","--diag_style","--errors",0};
 int i;for(i=0;names[i];i++)if(!strcmp(arg,names[i]))return 1;
 for(i=0;i<rule_count;i++)if(rules[i].mode==2&&!strcmp(arg,rules[i].match))return 1;
 return 0;
}
static int finish_rule(Rule *r){int i;if(!r->match||!r->emit||!r->mode)return error("each rule requires match, mode, emit");
 {const char *p=r->emit;while(*p){const char *start;while(isspace((unsigned char)*p))p++;start=p;while(*p&&!isspace((unsigned char)*p))p++;if(p>start&&reserved_token(start,(size_t)(p-start)))return error("rules cannot change selected package root or target/tool");}}
 if(r->match[0]!='-'||r->emit[0]!='-'||!strcmp(r->match,"--"))return error("match/emit must be option tokens");
 if(r->mode==3&&r->match[strlen(r->match)-1]!='=')return error("joined match must end in =");
 if(r->mode!=1&&(strchr(r->emit,' ')||strchr(r->emit,'\t')))return error("value/joined emit must be one token");
 if(r->mode==2&&r->emit[strlen(r->emit)-1]=='=')return error("value emit must be a separate option token");
 if(r->mode==3&&r->emit[strlen(r->emit)-1]!='=')return error("joined emit must end in =");
 for(i=0;i<rule_count-1;i++)if(!strcmp(r->match,rules[i].match))return error("duplicate match");
 return 0;}
int xh_package_options_load(const char *path){FILE *fp;char line[2048];Rule *r=0;int seen_section=0;
 rule_count=0;rule_used=0;if(!path||!*path)return 0;
 fp=fopen(path,"rb");if(!fp)return error("cannot open options file");
 while(fgets(line,sizeof(line),fp)){char *s,*eq,*v;size_t n=strlen(line);
  if(n==sizeof(line)-1&&line[n-1]!='\n'){fclose(fp);return error("line too long");}
  s=trim(line);if(!*s||*s==';'||*s=='#')continue;
  if(*s=='['){char *end=strchr(s,']');if(!end||end==s+1||end[1]){fclose(fp);return error("invalid rule section");}
   if(r&&finish_rule(r)){fclose(fp);return 1;}
   if(rule_count==MAX_RULES){fclose(fp);return error("rule capacity exceeded");}
   r=&rules[rule_count++];memset(r,0,sizeof(*r));seen_section=1;continue;
  }
  if(!r||(eq=strchr(s,'='))==0){fclose(fp);return error("expected key=value inside rule section");}
  *eq=0;v=trim(eq+1);s=trim(s);if(!*v){fclose(fp);return error("empty rule value");}
  if(!strcmp(s,"match")){if(r->match){fclose(fp);return error("duplicate match key");}r->match=save(v);if(!r->match)goto capacity;}
  else if(!strcmp(s,"emit")){if(r->emit){fclose(fp);return error("duplicate emit key");}r->emit=save(v);if(!r->emit)goto capacity;}
  else if(!strcmp(s,"mode")){if(r->mode){fclose(fp);return error("duplicate mode key");}r->mode=!strcmp(v,"flag")?1:!strcmp(v,"value")?2:!strcmp(v,"joined")?3:0;if(!r->mode){fclose(fp);return error("unknown rule mode");}}
  else {fclose(fp);return error("unknown rule key");}
 }
 if(ferror(fp)){fclose(fp);return error("cannot read options file");}fclose(fp);
 if(!seen_section)return error("empty options file");return finish_rule(r);
capacity:fclose(fp);return error("rule storage exceeded");}
static int append(const char *s,size_t n,int *count){char *p;if(*count==XH_DRIVER_MAX_ARGS||n+1>sizeof(mapped_storage)-mapped_used)return error("mapped argument limit exceeded");p=mapped_storage+mapped_used;memcpy(p,s,n);p[n]=0;mapped_used+=n+1;mapped[(*count)++]=p;return 0;}
int xh_package_options_map(int argc,char **argv,int *out_argc,char ***out_argv){int i,count=0,literal=0;mapped_used=0;
 if(argc<0||argc>XH_DRIVER_MAX_ARGS||!out_argc||!out_argv)return error("invalid argv");
 for(i=0;i<argc;i++){const char *a=argv[i];int j,best=-1;size_t length=0;
  if(!literal&&!strcmp(a,"--"))literal=1;
  if(!literal)for(j=0;j<rule_count;j++){size_t n=strlen(rules[j].match);if((rules[j].mode==3?!strncmp(a,rules[j].match,n):!strcmp(a,rules[j].match))&&n>length){best=j;length=n;}}
  if(best<0){if(append(a,strlen(a),&count))return 1;if(!literal&&xh_package_options_takes_value(a)&&i+1<argc){i++;if(append(argv[i],strlen(argv[i]),&count))return 1;}continue;}
  {Rule *r=&rules[best];
   if(r->mode==1){const char *p=r->emit;while(*p){const char *start;while(isspace((unsigned char)*p))p++;start=p;while(*p&&!isspace((unsigned char)*p))p++;if(p>start){if(*start=='@')return error("response expansion in rules is forbidden");if(append(start,(size_t)(p-start),&count))return 1;}}}
   else if(r->mode==2){if(i+1>=argc)return error("missing mapped option value");if(append(r->emit,strlen(r->emit),&count))return 1;i++;if(append(argv[i],strlen(argv[i]),&count))return 1;}
   else {char joined[XH_DRIVER_ARG_BYTES];int n;if(!a[length])return error("empty joined option value");n=snprintf(joined,sizeof(joined),"%s%s",r->emit,a+length);if(n<0||(size_t)n>=sizeof(joined))return error("joined argument too long");if(append(joined,(size_t)n,&count))return 1;}
  }
 }
 *out_argc=count;*out_argv=mapped;return 0;
}
