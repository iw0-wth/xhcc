/* Versioned external target resources. Code module selection belongs to host.
 */
#include "xh_package.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PATH_CAP 512
#define PACKAGE_CAP 32
typedef struct {
  char id[64], tools[128], target[32], cpu[32], abi[32], module[128],
      include[128];
  int version;
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
      if (pkg->version || strcmp(value, "1"))
        goto invalid;
      pkg->version = 1;
      continue;
    }
#define FIELD(k, fld)                                                          \
  if (!strcmp(key, k)) {                                                       \
    dest = pkg->fld;                                                           \
    cap = sizeof(pkg->fld);                                                    \
  }
    FIELD("id", id)
    else FIELD("tools", tools) else FIELD("target", target) else FIELD(
        "cpu",
        cpu) else FIELD("abi",
                        abi) else FIELD("module",
                                        module) else FIELD("include",
                                                           include) else goto invalid;
#undef FIELD
    if (*dest || strlen(value) >= cap)
      goto invalid;
    strcpy(dest, value);
  }
  if (ferror(f))
    goto invalid;
  fclose(f);
  if (pkg->version != 1 || !safe_relative(pkg->id) || strchr(pkg->id, '/') ||
      !*pkg->tools || !safe_relative(pkg->include) ||
      !safe_relative(pkg->module))
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
      if (strcmp(p->target, "arm") || strcmp(p->cpu, "cortex-m3") ||
          strcmp(p->abi, "aapcs-soft")) {
        fprintf(
            stderr,
            "xhcc: package target is unavailable in this compiler module: %s\n",
            p->id);
        return 1;
      }
      if (opts->include_dir_count >= ARK_PP_MAX_INC_DIRS) {
        fprintf(stderr, "xhcc: too many include paths\n");
        return 1;
      }
      if (snprintf(include_path, sizeof(include_path), "%s/%s/%s", root, p->id,
                   p->include) >= (int)sizeof(include_path) ||
          stat(include_path, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "xhcc: missing package include directory\n");
        return 1;
      }
      opts->include_dirs[opts->include_dir_count++] = include_path;
      opts->target = "arm";
      return 0;
    }
  fprintf(stderr, "xhcc: no package provides tool: %s\n", tool);
  return 1;
}
