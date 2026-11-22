#include "preproc.h"
#include "xh_package.h"
#include <stdio.h>
#include <string.h>
static int escaped(FILE *f, const char *s) {
  for (; *s; ++s) {
    if (*s == '\n' || *s == '\r')
      return -1;
    if (*s == '$') {
      if (fputc('$', f) == EOF)
        return -1;
    } else if (*s == ' ' || *s == '\t' || *s == '#' || *s == ':' ||
               *s == '\\') {
      if (fputc('\\', f) == EOF)
        return -1;
    }
    if (fputc(*s, f) == EOF)
      return -1;
  }
  return 0;
}
int xh_write_dependencies(const ArkOptions *o) {
  char path[512], target[512];
  const char *dest = o->depend_file, *obj = o->depend_target;
  const char *incs[ARKCC_MAX_INC_FILES];
  int n, i, rc = 0;
  FILE *f;
  if (!dest) {
    const char *base = o->output_path ? o->output_path : o->input_path;
    const char *dot = strrchr(base, '.'), *slash = strrchr(base, '/');
    size_t len =
        dot && (!slash || dot > slash) ? (size_t)(dot - base) : strlen(base);
    if (len + 3 > sizeof(path))
      return 1;
    memcpy(path, base, len);
    strcpy(path + len, ".d");
    dest = path;
  }
  if (!obj) {
    if (o->output_path)
      obj = o->output_path;
    else {
      const char *base = o->input_path, *slash = strrchr(base, '/'), *dot;
      size_t len;
      if (slash)
        base = slash + 1;
      dot = strrchr(base, '.');
      len = dot ? (size_t)(dot - base) : strlen(base);
      if (len + 3 > sizeof(target))
        return 1;
      memcpy(target, base, len);
      strcpy(target + len, o->emit_asm ? ".s" : ".o");
      obj = target;
    }
  }
  f = !strcmp(dest, "-") ? stdout : fopen(dest, "wb");
  if (!f) {
    fprintf(stderr, "xhcc: cannot write dependency file: %s\n", dest);
    return 1;
  }
  rc = o->depend_target && !o->dep_quote_target ? (fputs(obj, f) == EOF ? -1 : 0) : escaped(f, obj);
  if (fputs(": ", f) == EOF)
    rc = -1;
  if (escaped(f, o->input_path))
    rc = -1;
  n = preproc_get_included_files(incs, ARKCC_MAX_INC_FILES);
  for (i = 0; i < n; ++i) {
    if (!o->include_system_deps && preproc_dependency_is_system(i)) continue;
    if (fputc(' ', f) == EOF || escaped(f, incs[i]))
      rc = -1;
  }
  if (fputc('\n', f) == EOF)
    rc = -1;
  if (o->dep_phony) {
    for (i = 0; i < n; ++i) {
      int j;
      if (!o->include_system_deps && preproc_dependency_is_system(i)) continue;
      for (j = 0; j < i; ++j) if (!strcmp(incs[j], incs[i])) break;
      if (j < i) continue;
      if (escaped(f, incs[i]) || fputs(":\n", f) == EOF) rc = -1;
    }
  }
  if (f == stdout ? fflush(f) : fclose(f))
    rc = -1;
  if (rc) {
    if (strcmp(dest, "-")) remove(dest);
    fprintf(stderr, "xhcc: dependency output failed: %s\n", dest);
  }
  return rc ? 1 : 0;
}
