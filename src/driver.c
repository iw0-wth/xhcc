/* xhcc: bounded command parsing followed by direct ark_compile calls. */
#include "compiler.h"
#include "config.h"
#include "diag.h"
#include "memory.h"
#include "xh_driver.h"
#include "xh_package.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static struct {
  char bytes[XH_DRIVER_ARG_BYTES];
  char *args[XH_DRIVER_MAX_ARGS];
  char response_paths[XH_DRIVER_RESPONSE_DEPTH][XH_DRIVER_PATH_BYTES];
  const char *sources[XH_DRIVER_MAX_SOURCES];
  ArkOptions opts;
  size_t used;
  unsigned long response_bytes;
  int argc;
  int source_count;
} driver;

static int fail(const char *message, const char *arg) {
  diag_report_msg(DIAG_SEV_ERROR, 0, message, arg, 0, 0);
  return 1;
}

static int append_byte(size_t *end, int c) {
  if (*end >= sizeof(driver.bytes))
    return fail("argument storage limit exceeded", 0);
  driver.bytes[(*end)++] = (char)c;
  return 0;
}

/* Lexical canonicalisation detects a/../a.rsp and ./a.rsp cycles without
 * platform-specific realpath APIs. Symlink aliases are bounded by depth too. */
static int response_key(const char *path, char *out) {
  size_t n = 0;
  const char *p = path;
  while (*p) {
    const char *start;
    size_t len;
    while (*p == '/' || *p == '\\') {
      if (!n)
        out[n++] = '/';
      p++;
    }
    if (!*p)
      break;
    start = p;
    while (*p && *p != '/' && *p != '\\')
      p++;
    len = (size_t)(p - start);
    if (len == 1 && start[0] == '.')
      continue;
    if (len == 2 && start[0] == '.' && start[1] == '.' && n &&
        !(n >= 2 && out[n - 1] == '.' && out[n - 2] == '.')) {
      if (n > 1 && out[n - 1] == '/')
        n--;
      while (n && out[n - 1] != '/')
        n--;
      continue;
    }
    if (n && out[n - 1] != '/') {
      if (n >= XH_DRIVER_PATH_BYTES - 1)
        return 1;
      out[n++] = '/';
    }
    if (n + len >= XH_DRIVER_PATH_BYTES)
      return 1;
    while (len--) {
      unsigned char c = (unsigned char)*start++;
#ifdef _WIN32
      c = (unsigned char)tolower(c);
#endif
      out[n++] = (char)c;
    }
  }
  if (!n)
    out[n++] = '.';
  out[n] = 0;
  return 0;
}

static int expand_argument(const char *arg, int depth);

static int expand_response(const char *path, int depth) {
  FILE *fp;
  int c, i, result = 0;
  if (!*path)
    return fail("empty response file path", 0);
  if (depth >= XH_DRIVER_RESPONSE_DEPTH)
    return fail("response nesting limit exceeded: %s", path);
  if (response_key(path, driver.response_paths[depth]))
    return fail("response path too long: %s", path);
  for (i = 0; i < depth; i++)
    if (!strcmp(driver.response_paths[i], driver.response_paths[depth]))
      return fail("response file cycle: %s", path);
  fp = fopen(path, "rb");
  if (!fp)
    return fail("cannot open response file: %s", path);
  c = fgetc(fp);
  for (;;) {
    size_t start, end;
    int quote = 0, started = 0;
    while (c != EOF && isspace((unsigned char)c)) {
      if (++driver.response_bytes > XH_DRIVER_RESPONSE_BYTES) {
        result = fail("response byte limit exceeded", 0);
        goto done;
      }
      c = fgetc(fp);
    }
    if (c == EOF)
      break;
    start = end = driver.used;
    while (c != EOF && (quote || !isspace((unsigned char)c))) {
      if (++driver.response_bytes > XH_DRIVER_RESPONSE_BYTES) {
        result = fail("response byte limit exceeded", 0);
        goto done;
      }
      if (!c) {
        result = fail("NUL byte in response file: %s", path);
        goto done;
      }
      started = 1;
      if (c == quote)
        quote = 0;
      else if (!quote && (c == '\'' || c == '"'))
        quote = c;
      else if (c == '\\' && quote != '\'') {
        int next = fgetc(fp);
        /* Preserve ordinary Windows path separators. */
        if (next == '\\' || next == '"' || next == '\'' ||
            (next != EOF && isspace((unsigned char)next))) {
          if (++driver.response_bytes > XH_DRIVER_RESPONSE_BYTES) {
            result = fail("response byte limit exceeded", 0);
            goto done;
          }
          if (append_byte(&end, next)) {
            result = 1;
            goto done;
          }
        } else {
          if (append_byte(&end, c)) {
            result = 1;
            goto done;
          }
          c = next;
          continue;
        }
      } else if (append_byte(&end, c)) {
        result = 1;
        goto done;
      }
      c = fgetc(fp);
    }
    if (quote) {
      result = fail("unterminated quote in response file: %s", path);
      goto done;
    }
    if (started) {
      if (append_byte(&end, 0)) {
        result = 1;
        goto done;
      }
      driver.used = end;
      if (expand_argument(driver.bytes + start, depth + 1)) {
        result = 1;
        goto done;
      }
    }
  }
  if (ferror(fp))
    result = fail("cannot read response file: %s", path);
done:
  fclose(fp);
  return result;
}

static int expand_argument(const char *arg, int depth) {
  if (arg[0] == '@')
    return expand_response(arg + 1, depth);
  if (driver.argc == XH_DRIVER_MAX_ARGS)
    return fail("argument count limit exceeded", 0);
  driver.args[driver.argc++] = (char *)arg;
  return 0;
}

static const char *value(int *index, const char *inline_value) {
  const char *s = inline_value;
  if (!s) {
    if (*index + 1 >= driver.argc) {
      fail("missing argument for %s", driver.args[*index]);
      return 0;
    }
    s = driver.args[++*index];
  }
  if (!*s) {
    fail("empty option argument", 0);
    return 0;
  }
  return s;
}

static int macro_valid(const char *s, int define) {
  if (!isalpha((unsigned char)*s) && *s != '_')
    return 0;
  do {
    s++;
  } while (isalnum((unsigned char)*s) || *s == '_');
  return !*s || (define && *s == '=');
}

static int add_macro(const char *s, int define) {
  ArkOptions *o = &driver.opts;
  size_t end = driver.used;
  const char *action = driver.bytes + end;
  if (!macro_valid(s, define))
    return fail("invalid macro argument: %s", s);
  if (o->macro_action_count == ARK_MAX_PRE_DEFINES)
    return fail("macro action limit exceeded", 0);
  if (append_byte(&end, '-') || append_byte(&end, define ? 'D' : 'U'))
    return 1;
  while (*s) {
    if (append_byte(&end, *s++))
      return 1;
  }
  if (append_byte(&end, 0))
    return 1;
  driver.used = end;
  o->macro_actions[o->macro_action_count++] = action;
  if (define)
    o->pre_defines[o->pre_define_count++] = action + 2;
  else
    o->pre_undefines[o->pre_undefine_count++] = action + 2;
  return 0;
}

static void usage(void) {
  puts("xhcc 0.1 - Cortex-M3 Thumb soft-float C99 compiler\n"
       "Usage: xhcc [arm-none-eabi-gcc|armcc] -E|-S|-c [options] file.c ...\n"
       "  -o FILE  -I DIR  -D NAME[=VALUE]  -U NAME  -include FILE\n"
       "  -std=c99 (armcc: --c99)  -O0|-O1  -mcpu=cortex-m3\n"
       "  -mthumb  -mfloat-abi=soft  (armcc: --cpu Cortex-M3 --thumb --fpu "
       "SoftVFP)\n"
       "  -MMD -MF FILE -MT TARGET  --private-object  --memory-report\n"
       "  --package-root DIR (default packages)  --list-tools  -v\n"
       "  @FILE: quoted arguments, cwd-relative nested paths; no expansion\n"
       "  Limits: 512 arguments, 64 sources, 32 KiB storage, 64 KiB response\n"
       "          input, 8 response levels, 511-byte derived output paths.\n"
       "  Multiple sources compile sequentially; explicit -o/-MF/-MT require\n"
       "  one source. No linking, assembly inputs, other CPUs/ABIs/options.");
}

static int same_path(const char *a, const char *b) {
  char ka[XH_DRIVER_PATH_BYTES], kb[XH_DRIVER_PATH_BYTES];
  return !response_key(a, ka) && !response_key(b, kb) && !strcmp(ka, kb);
}

static int derive_output(const char *source, const char *suffix, char *out) {
  const char *base = source, *p, *dot = 0;
  size_t n;
  for (p = source; *p; p++)
    if (*p == '/' || *p == '\\')
      base = p + 1;
  for (p = base; *p; p++)
    if (*p == '.')
      dot = p;
  n = dot ? (size_t)(dot - base) : strlen(base);
  if (n + strlen(suffix) >= XH_DRIVER_PATH_BYTES)
    return fail("output path too long: %s", source);
  memcpy(out, base, n);
  strcpy(out + n, suffix);
  return 0;
}

int xh_driver_main(int argc, char **argv) {
  ArkOptions *o = &driver.opts;
  const char *root = "packages", *tool = "arm-none-eabi-gcc";
  int i, literal = 0, list_tools = 0, memory_report = 0, result = 0;
  int seen_e = 0, seen_s = 0, seen_c = 0;
  memset(&driver, 0, sizeof(driver));
  config_init();
  diag_init(0, 0);
  for (i = 1; i < argc; i++)
    if (expand_argument(argv[i], 0)) {
      result = 1;
      goto done;
    }
  o->target = "arm";
  o->cpu = "Cortex-M3";
  for (i = 0; i < driver.argc; i++) {
    const char *a = driver.args[i], *v = 0;
    if (!literal && !strcmp(a, "--")) {
      literal = 1;
      continue;
    }
    if (!literal && i == 0 && a[0] != '-' &&
        (strlen(a) < 3 || strcmp(a + strlen(a) - 2, ".c"))) {
      tool = a;
      continue;
    }
    if (!literal && (!strcmp(a, "--help") || !strcmp(a, "-h"))) {
      usage();
      goto done;
    }
    if (!literal && !strcmp(a, "--version")) {
      puts("xhcc 0.1 (Ark C core), Cortex-M3 Thumb soft-float C99");
      goto done;
    }
    if (!literal && !strcmp(a, "--list-tools")) {
      list_tools = 1;
      continue;
    }
    if (!literal && !strcmp(a, "--memory-report")) {
      memory_report = 1;
      continue;
    }
    if (!literal && !strcmp(a, "--private-object")) {
      o->private_object = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-v")) {
      o->verbose = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-E")) {
      seen_e = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-S")) {
      seen_s = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-c")) {
      seen_c = 1;
      continue;
    }
    if (!literal && (!strcmp(a, "-std=c99") || !strcmp(a, "--c99") ||
                     !strcmp(a, "-mthumb") || !strcmp(a, "--thumb") ||
                     !strcmp(a, "-mfloat-abi=soft")))
      continue;
    if (!literal && (!strcmp(a, "-O0") || !strcmp(a, "-O1"))) {
      o->opt_level = a[2] - '0';
      continue;
    }
    if (!literal && !strcmp(a, "-MMD")) {
      o->gen_depend = 1;
      continue;
    }
    if (!literal &&
        (!strcmp(a, "--package-root") || !strncmp(a, "--package-root=", 15))) {
      root = value(&i, a[14] == '=' ? a + 15 : 0);
      if (!root) {
        result = 1;
        goto done;
      }
      continue;
    }
    if (!literal && (!strcmp(a, "--cpu") || !strncmp(a, "--cpu=", 6) ||
                     !strcmp(a, "-mcpu") || !strncmp(a, "-mcpu=", 6))) {
      v = value(&i, a[5] == '=' ? a + 6 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (strcmp(v, "cortex-m3") && strcmp(v, "Cortex-M3")) {
        result = fail("only Cortex-M3 is supported: %s", v);
        goto done;
      }
      continue;
    }
    if (!literal && (!strcmp(a, "--fpu") || !strncmp(a, "--fpu=", 6))) {
      v = value(&i, a[5] == '=' ? a + 6 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (strcmp(v, "SoftVFP") && strcmp(v, "softvfp")) {
        result = fail("only soft float is supported: %s", v);
        goto done;
      }
      continue;
    }
    if (!literal && !strncmp(a, "-include", 8) && (!a[8] || a[8] == '=')) {
      v = value(&i, a[8] == '=' ? a + 9 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (o->forced_include_count == ARK_PP_MAX_INC_DIRS) {
        result = fail("forced include limit exceeded", 0);
        goto done;
      }
      o->forced_includes[o->forced_include_count++] = v;
      continue;
    }
    if (!literal && (!strncmp(a, "-MF", 3) || !strncmp(a, "-MT", 3))) {
      v = value(&i, a[3] ? a + 3 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (a[2] == 'F')
        o->depend_file = v;
      else
        o->depend_target = v;
      continue;
    }
    if (!literal && a[0] == '-' &&
        (a[1] == 'I' || a[1] == 'D' || a[1] == 'U' || a[1] == 'o')) {
      v = value(&i, a[2] ? a + 2 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (a[1] == 'I') {
        if (o->include_dir_count == ARK_PP_MAX_INC_DIRS) {
          result = fail("include directory limit exceeded", 0);
          goto done;
        }
        o->include_dirs[o->include_dir_count++] = v;
      } else if (a[1] == 'o')
        o->output_path = v;
      else if (add_macro(v, a[1] == 'D')) {
        result = 1;
        goto done;
      }
      continue;
    }
    if (!literal && a[0] == '-') {
      result = fail("unsupported option: %s", a);
      goto done;
    }
    if (strlen(a) < 3 || strcmp(a + strlen(a) - 2, ".c")) {
      result = fail("only .c source inputs are supported: %s", a);
      goto done;
    }
    if (driver.source_count == XH_DRIVER_MAX_SOURCES) {
      result = fail("source count limit exceeded", 0);
      goto done;
    }
    driver.sources[driver.source_count++] = a;
  }
  if (list_tools) {
    result = xh_package_list(root) != 0;
    goto done;
  }
  if (!driver.source_count) {
    result = fail("no input source files", 0);
    goto done;
  }
  if (!seen_e && !seen_s && !seen_c) {
    result = fail("linking is unsupported; specify -E, -S or -c", 0);
    goto done;
  }
  if (driver.source_count > 1 &&
      (o->output_path || o->depend_file || o->depend_target)) {
    result = fail("-o, -MF and -MT require a single source", 0);
    goto done;
  }
  if (!o->gen_depend && (o->depend_file || o->depend_target)) {
    result = fail("-MF and -MT require -MMD", 0);
    goto done;
  }
  o->preprocess_only = seen_e;
  o->emit_asm = !seen_e && seen_s;
  o->compile_only = !seen_e && !seen_s;
  if (o->private_object && !o->compile_only) {
    result = fail("--private-object requires -c without -E/-S", 0);
    goto done;
  }
  g_config.enable_fpu_hw = 0;
  g_config.fpu_type = "SoftVFP";
  g_config.opt_level = o->opt_level;
  g_config.enable_opt_inline = 0;
  g_config.enable_opt_vectorize = 0;
  if (xh_package_apply(root, tool, o)) {
    result = 1;
    goto done;
  }
  {
    const char *explicit_output = o->output_path;
    const char *explicit_depend = o->depend_file;
    for (i = 0; i < driver.source_count; i++) {
      char output[XH_DRIVER_PATH_BYTES], depend[XH_DRIVER_PATH_BYTES];
      int rc, errors;
      o->input_path = driver.sources[i];
      o->output_path = explicit_output;
      o->depend_file = explicit_depend;
      if (!explicit_output && !o->preprocess_only) {
        if (derive_output(o->input_path,
                          o->emit_asm         ? ".s"
                          : o->private_object ? ".ark_obj"
                                              : ".o",
                          output)) {
          result = 1;
          break;
        }
        o->output_path = output;
      }
      {
        int j;
        for (j = 0; o->output_path && j < driver.source_count; j++)
          if (same_path(o->output_path, driver.sources[j])) {
            result =
                fail("output would overwrite input: %s", driver.sources[j]);
            break;
          }
        if (j < driver.source_count && o->output_path)
          break;
      }
      if (o->gen_depend && !explicit_depend) {
        const char *dep_base = o->output_path && strcmp(o->output_path, "-")
                                   ? o->output_path
                                   : o->input_path;
        const char *dot = strrchr(dep_base, '.');
        const char *slash = strrchr(dep_base, '/'),
                   *backslash = strrchr(dep_base, '\\');
        size_t n;
        if ((slash && dot && dot < slash) ||
            (backslash && dot && dot < backslash))
          dot = 0;
        n = dot ? (size_t)(dot - dep_base) : strlen(dep_base);
        if (n + 3 >= sizeof(depend)) {
          result = fail("dependency path too long: %s", dep_base);
          break;
        }
        memcpy(depend, dep_base, n);
        strcpy(depend + n, ".d");
        o->depend_file = depend;
      }
      if (o->gen_depend && o->depend_file) {
        int j;
        for (j = 0; j < driver.source_count; j++)
          if (same_path(o->depend_file, driver.sources[j]))
            break;
        if (j < driver.source_count ||
            (o->output_path && same_path(o->depend_file, o->output_path))) {
          result = fail("dependency output would overwrite input/output: %s",
                        o->depend_file);
          break;
        }
      }
      mem_pool_reset();
      diag_init(o->input_path, 0);
      rc = ark_compile(o);
      errors = diag_errors();
      if (memory_report)
        fprintf(stderr, "[xhcc] %s: arena peak %d/%d B\n", o->input_path,
                mem_pool_high(), mem_pool_capacity());
      if (rc && !errors)
        fprintf(stderr, "[xhcc] compilation failed (%d): %s\n", rc,
                o->input_path);
      diag_shutdown();
      if (rc || errors)
        result = 1;
    }
  }
done:
  diag_shutdown();
  return result;
}

#ifndef XH_DRIVER_NO_MAIN
int main(int argc, char **argv) { return xh_driver_main(argc, argv); }
#endif
