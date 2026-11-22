/* Versioned external target resources. Code module selection belongs to host.
 */
#include "xh_package.h"
#include "xh_rules.h"
#include "xh_template.h"
#ifndef XH_MODULE_TARGET
#define XH_MODULE_TARGET "arm"
#endif
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PATH_CAP 512
#define PACKAGE_CAP 32
typedef struct {
  char id[64], tools[128], target[32], cpu[32], abi[32], module[128],
      include[128];
  char dialect[16], options[128], engine[32], encoding[128], selection[128], templates[128];
  int version;
  char protocol[32], protocol_major[8], protocol_minor[8], min_host_minor[8];
  char requires[512], entry[32], adapter[128], backend[128];
  char request_protocol[32], request_major[8], request_minor[8];
  char ir_protocol[32], ir_major[8], ir_minor[8];
} Package;
static Package packages[PACKAGE_CAP];
static char include_path[PATH_CAP];
static char *trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t')
    ++s;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                     end[-1] == '\n'))
    --end;
  *end = 0;
  return s;
}
static int safe_relative(const char *s) {
  const char *p = s;
  if (!*s || *s == '/' || *s == '\\' || strchr(s, ':') || strchr(s, '\\'))
    return 0;
  while (*p) {
    const char *q = strchr(p, '/');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
      return 0;
    if (!q)
      break;
    p = q + 1;
  }
  return 1;
}
static int package_requirements(const Package *pkg) {
  static const char *supported[]={"codegen.templates.v1","package.options.v1","encoding.fields.v1","selection.scalar32.v1","selection.scalar32.v2","object.elf32","object.elf64",0};
  const char *p=pkg->requires;
  while(*p){const char *end=strchr(p,',');size_t n=end?(size_t)(end-p):strlen(p);int i,known=0;size_t k;
    if(!n||n>=96||p[0]<'a'||p[0]>'z')goto invalid;
    {const char *prior=pkg->requires;while(prior<p){const char *pe=strchr(prior,',');size_t pn=pe?(size_t)(pe-prior):strlen(prior);if(pn==n&&!strncmp(prior,p,n))goto invalid;if(!pe)break;prior=pe+1;}}
    for(k=0;k<n;k++) {
      int alpha=p[k]>='a'&&p[k]<='z',digit=p[k]>='0'&&p[k]<='9';
      if(!alpha&&!digit){if((p[k]!='.'&&p[k]!='_'&&p[k]!='-')||k==n-1||k==0||!((p[k-1]>='a'&&p[k-1]<='z')||(p[k-1]>='0'&&p[k-1]<='9')))goto invalid;}
    }
    for(i=0;supported[i];i++)if(strlen(supported[i])==n&&!strncmp(p,supported[i],n)){known=1;break;}
    if(!known){fprintf(stderr,"xhcc: package %s requires unsupported capability: %.*s\n",pkg->id,(int)n,p);return 1;}
    if(!end)break;p=end+1;if(!*p)goto invalid;
  }
  return 0;
invalid:fprintf(stderr,"xhcc: invalid package capability requirements: %s\n",pkg->id);return 1;
}
static int package_target(const Package *p) {
  static const struct {const char *target,*abi;} descriptors[]={
    {"arm","aapcs-soft"},{"riscv32","ilp32"},{"x86","sysv-ilp32"},{"x86_64","sysv-lp64"},{"c","host-c"}};
  unsigned i;
  for(i=0;i<sizeof(descriptors)/sizeof(descriptors[0]);i++)if(!strcmp(p->target,descriptors[i].target)){
    if(strcmp(p->abi,descriptors[i].abi)||!*p->cpu){fprintf(stderr,"xhcc: package ABI/CPU does not match target: %s\n",p->id);return 1;}
    if(*p->engine && (strcmp(p->engine,"rv32-rule-v1")||strcmp(p->target,"riscv32"))){fprintf(stderr,"xhcc: unsupported package engine/target: %s\n",p->id);return 1;}
    if((*p->encoding&&strcmp(p->target,"riscv32")&&strcmp(p->target,"arm"))||(*p->selection&&strcmp(p->target,"riscv32"))){fprintf(stderr,"xhcc: rules unavailable for package target: %s\n",p->id);return 1;}
    return 0;
  }
  fprintf(stderr,"xhcc: unsupported package target: %s\n",p->target);return 1;
}
static int read_package(const char *path, Package *pkg) {
  char line[512];
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  memset(pkg, 0, sizeof(*pkg));
  while (fgets(line, sizeof(line), f)) {
    char *key, *value, *dest = 0;
    size_t cap = 0;
    if (!strchr(line, '\n') && !feof(f))
      goto invalid;
    key = trim(line);
    if (!*key || *key == '#' || *key == ';')
      continue;
    value = strchr(key, '=');
    if (!value)
      goto invalid;
    *value++ = 0;
    key = trim(key);
    value = trim(value);
    if (!strcmp(key, "version")) {
      if (pkg->version || (strcmp(value, "1") && strcmp(value,"3")))
        goto invalid;
      pkg->version = value[0] - '0';
      continue;
    }
#define FIELD(k, fld)                                                          \
  if (!strcmp(key, k)) {                                                       \
    dest = pkg->fld;                                                           \
    cap = sizeof(pkg->fld);                                                    \
  }
    FIELD("templates",templates) else FIELD("selection",selection) else FIELD("engine",engine) else FIELD("encoding",encoding) else FIELD("dialect",dialect) else FIELD("options",options) else FIELD("id", id)
    else FIELD("tools", tools) else FIELD("target", target) else FIELD(
        "cpu",
        cpu) else FIELD("abi",
                        abi) else FIELD("module",
                                        module) else FIELD("include",
                                                           include)
    else FIELD("protocol", protocol) else FIELD("protocol_major", protocol_major)
    else FIELD("protocol_minor", protocol_minor) else FIELD("min_host_minor", min_host_minor)
    else FIELD("requires", requires) else FIELD("entry", entry)
    else FIELD("adapter", adapter) else FIELD("backend", backend)
    else FIELD("request_protocol", request_protocol) else FIELD("request_major", request_major)
    else FIELD("request_minor", request_minor) else FIELD("ir_protocol", ir_protocol)
    else FIELD("ir_major", ir_major) else FIELD("ir_minor", ir_minor) else goto invalid;
#undef FIELD
    if (*dest || strlen(value) >= cap)
      goto invalid;
    strcpy(dest, value);
  }
  if (ferror(f))
    goto invalid;
  fclose(f);
  if ((pkg->version != 1 && pkg->version != 3) || !safe_relative(pkg->id) || strchr(pkg->id, '/') ||
      !*pkg->tools || !safe_relative(pkg->include) ||
      (pkg->version != 3 && !safe_relative(pkg->module)) ||
      (*pkg->options && !safe_relative(pkg->options)) ||
      (*pkg->encoding && !safe_relative(pkg->encoding)) ||
      (*pkg->selection && !safe_relative(pkg->selection)) ||
      (*pkg->templates && !safe_relative(pkg->templates)))
    goto invalid_closed;
  return 1;
invalid:
  fclose(f);
invalid_closed:
  fprintf(stderr, "xhcc: invalid package manifest: %s\n", path);
  return -1;
}
static int has_tool(const char *list, const char *tool) {
  size_t n = strlen(tool);
  const char *p = list;
  while (*p) {
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !strncmp(p, tool, n))
      return 1;
    if (!end)
      break;
    p = end + 1;
  }
  return 0;
}
static int scan(const char *root) {
  DIR *d = opendir(root);
  struct dirent *e;
  int count = 0;
  char path[PATH_CAP];
  if (!d) {
    fprintf(stderr, "xhcc: cannot open package root: %s\n", root);
    return -1;
  }
  while ((e = readdir(d))) {
    int rc;
    if (e->d_name[0] == '.')
      continue;
    if (snprintf(path, sizeof(path), "%s/%s/package.ini", root, e->d_name) >=
        (int)sizeof(path)) {
      closedir(d);
      return -1;
    }
    if (count == PACKAGE_CAP) {
      fprintf(stderr, "xhcc: package capacity exceeded\n");
      closedir(d);
      return -1;
    }
    rc = read_package(path, &packages[count]);
    if (rc < 0) {
      closedir(d);
      return -1;
    }
    if (rc) {
      if (strcmp(packages[count].id, e->d_name)) {
        fprintf(stderr, "xhcc: package id must equal directory name\n");
        closedir(d);
        return -1;
      }
      ++count;
    }
  }
  closedir(d);
  {
    int i, j;
    for (i = 0; i < count; i++) {
      char copy[128], *p, *q;
      strcpy(copy, packages[i].tools);
      p = copy;
      do {
        q = strchr(p, ',');
        if (q)
          *q = 0;
        if (!*p) {
          fprintf(stderr, "xhcc: empty tool alias\n");
          return -1;
        }
        for (j = 0; j < i; j++)
          if (has_tool(packages[j].tools, p)) {
            fprintf(stderr, "xhcc: duplicate tool alias: %s\n", p);
            return -1;
          }
        if (q && has_tool(q + 1, p)) {
          fprintf(stderr, "xhcc: duplicate tool alias: %s\n", p);
          return -1;
        }
        p = q ? q + 1 : 0;
      } while (p);
    }
  }
  return count;
}
int xh_package_adapter_config(const char *root,const char *tool,char *path,size_t cap,char *dialect,size_t dialect_cap) {
  int n=scan(root),i;
  if(n<0)return 1;
  if(!cap||!dialect_cap)return 1;
  path[0]=0;dialect[0]=0;
  for(i=0;i<n;i++)if(has_tool(packages[i].tools,tool)) {
    Package *p=&packages[i];const char *d=*p->dialect?p->dialect:(!strcmp(tool,"armcc")?"armcc":"gcc");
    if(package_requirements(p)||package_target(p))return 1;
    if(strcmp(d,"gcc")&&strcmp(d,"armcc")){fprintf(stderr,"xhcc: unsupported argument dialect: %s\n",d);return 1;}
    if(strlen(d)>=dialect_cap)return 1;strcpy(dialect,d);
    if(*p->options && snprintf(path,cap,"%s/%s/%s",root,p->id,p->options)>=(int)cap)return 1;
    return 0;
  }
  fprintf(stderr,"xhcc: no package provides tool: %s\n",tool);return 1;
}
int xh_package_list(const char *root) {
  int n = scan(root), i;
  if (n < 0)
    return 1;
  for (i = 0; i < n; i++)
    printf("%s: %s target=%s cpu=%s abi=%s module=%s\n", packages[i].id,
           packages[i].tools, packages[i].target, packages[i].cpu,
           packages[i].abi, packages[i].module);
  return 0;
}
int xh_package_apply(const char *root, const char *tool, ArkOptions *opts) {
  int n = scan(root), i;
  struct stat st;
  if (n < 0)
    return 1;
  for (i = 0; i < n; i++)
    if (has_tool(packages[i].tools, tool)) {
      Package *p = &packages[i];
      if(package_requirements(p)||package_target(p))return 1;
      if (strcmp(XH_MODULE_TARGET, "all") && strcmp(p->target, XH_MODULE_TARGET)) {
        fprintf(
            stderr,
            "xhcc: package target is unavailable in this compiler module: %s\n",
            p->id);
        return 1;
      }
      if (!opts->no_stdinc && opts->system_include_dir_count >= ARK_PP_MAX_INC_DIRS) {
        fprintf(stderr, "xhcc: too many include paths\n");
        return 1;
      }
      if (snprintf(include_path, sizeof(include_path), "%s/%s/%s", root, p->id,
                   p->include) >= (int)sizeof(include_path) ||
          stat(include_path, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "xhcc: missing package include directory\n");
        return 1;
      }
      if (!opts->no_stdinc) opts->system_include_dirs[opts->system_include_dir_count++] = include_path;
      if (*p->engine && strcmp(p->engine,"rv32-rule-v1")) {
        fprintf(stderr,"xhcc: unsupported package rule engine: %s\n",p->engine);return 1;
      }
      if (*p->encoding) {
        char rules_path[PATH_CAP];
        if(snprintf(rules_path,sizeof(rules_path),"%s/%s/%s",root,p->id,p->encoding)>=(int)sizeof(rules_path)||xh_encoding_set_path(p->target,rules_path))return 1;
      } else if (xh_encoding_set_path(p->target,0)) return 1;
      if (*p->selection) {
        char rules_path[PATH_CAP];
        if(strcmp(p->target,"riscv32")||snprintf(rules_path,sizeof(rules_path),"%s/%s/%s",root,p->id,p->selection)>=(int)sizeof(rules_path)||xh_rules_set_selection_path(rules_path))return 1;
      } else if(xh_rules_set_selection_path(0)) return 1;
      if (*p->templates) {
        char template_path[PATH_CAP];
        if(snprintf(template_path,sizeof(template_path),"%s/%s/%s",root,p->id,p->templates)>=(int)sizeof(template_path)||xh_template_set_path(p->target,template_path))return 1;
      } else if(xh_template_set_path(p->target,0))return 1;
      opts->target = p->target;
      opts->cpu = strcmp(p->target, "arm") ? 0 : p->cpu;
      return 0;
    }
  fprintf(stderr, "xhcc: no package provides tool: %s\n", tool);
  return 1;
}
