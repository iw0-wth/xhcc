/* xhcc: bounded command parsing followed by direct ark_compile calls. */
#include "compiler.h"
#include "config.h"
#include "cpu.h"
#include "pipeline.h"
#include "diag.h"
#ifndef XH_ADAPTER
#include "memory.h"
#else
#include "xh_request.h"
#endif
#include "xh_driver.h"
#include "xh_package.h"
#include "xh_package_options.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef XH_MODULE_TARGET
#define XH_MODULE_TARGET "arm"
#endif

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

/* Only flags with real config semantics reach config_apply_args. */
static int config_flag(const char *a) {
  static const char *names[] = {
    "vla", "complex", "wchar", "trigraph", "opt-constfold", "constfold",
    "opt-dce", "dce", "opt-regalloc", "regalloc", "opt-regalloc-near",
    "regalloc-near", "opt-vstack", "vstack", "opt-addr", "addr",
    "opt-strength", "strength", "opt-cbz", "cbz", "opt-branch", "branch",
    "opt-imm", "imm", "opt-peep", "peep", "opt-cbranch", "cbranch",
    "opt-copyprop", "copyprop", "opt-live", "live", "opt-inline",
    "autoinline", "opt-vectorize", "vectorize", "enum-is-int", "dollar", 0
  };
  static const char *long_names[] = {
    "--enum_is_int", "--dollar", "--signed_chars", "--unsigned_chars",
    "--wchar16", "--wchar32", "--autoinline", "--no_autoinline",
    "--vectorize", "--no_vectorize", 0
  };
  const char *name;
  int i;
  for (i = 0; long_names[i]; i++)
    if (!strcmp(a, long_names[i])) return 1;
  if (strncmp(a, "-f", 2)) return 0;
  name = !strncmp(a, "-fno-", 5) ? a + 5 : a + 2;
  for (i = 0; names[i]; i++)
    if (!strcmp(name, names[i])) return 1;
  return 0;
}

static void apply_flag(const char *a) {
  char *args[2];
  args[0] = (char *)"xhcc";
  args[1] = (char *)a;
  config_apply_args(2, args);
}

static int runtime_flag(const char *a) {
  struct Switch { const char *name; int *field; };
  struct Switch switches[] = {
    {"warn-mem", &g_config.enable_memlint}, {"warn-enum", &g_config.enable_enumcheck},
    {"warn-unused-result", &g_config.enable_unused_result},
    {"warn-unreachable", &g_config.enable_warn_unreachable},
    {"warn-unused-static", &g_config.enable_warn_unused_static},
    {"warn-recursion", &g_config.enable_warn_recursion},
    {"licm", &g_config.enable_licm}, {"unroll", &g_config.enable_unroll},
    {"tailmerge", &g_config.enable_tailmerge}, {"xblock", &g_config.enable_xblock},
    {"tailcall", &g_config.enable_tailcall}, {"divconst", &g_config.enable_divconst},
    {"const-global", &g_config.enable_const_global},
    {"dce-cfg", &g_config.enable_dce_cfg}, {"reg-top", &g_config.enable_reg_top},
    {"rt-lazy", &g_config.enable_rt_lazy}, {"asm-extra", &g_config.enable_asm_extra}
  };
  const char *name;
  int enable = 1;
  size_t i;
  if (strncmp(a, "--", 2)) return 0;
  name = a + 2;
  if (!strncmp(name, "no-", 3)) { enable = 0; name += 3; }
  for (i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
    if (!strcmp(name, switches[i].name)) { *switches[i].field = enable; return 1; }
  return 0;
}

/* GCC spellings expose only checks implemented by this compiler. */
static int warning_flag(const char *a) {
  const char *name;
  int enable = 1;
  if (!strcmp(a, "-Werror")) { diag_set_warnings_as_errors(1); return 1; }
  if (!strcmp(a, "-Wno-error")) { diag_set_warnings_as_errors(0); return 1; }
  if (!strcmp(a, "-w")) { diag_set_warnings_disabled(1); return 1; }
  if (!strcmp(a, "-Wall")) {
    g_config.enable_enumcheck = 1;
    g_config.enable_unused_result = 1;
    g_config.enable_warn_unused_static = 1;
    g_config.enable_warn_unreachable = 1;
    return 1;
  }
  if (!strcmp(a, "-Wextra")) {
    g_config.enable_memlint = 1;
    g_config.enable_warn_recursion = 1;
    return 1;
  }
  if (strncmp(a, "-W", 2)) return 0;
  name = a + 2;
  if (!strncmp(name, "no-", 3)) { enable = 0; name += 3; }
  if (!strcmp(name, "unused")) {
    driver.opts.no_unused_warn = !enable;
    g_config.enable_unused_result = enable;
    g_config.enable_warn_unused_static = enable;
  } else if (!strcmp(name, "unused-function")) g_config.enable_warn_unused_static = enable;
  else if (!strcmp(name, "unused-result")) g_config.enable_unused_result = enable;
  else if (!strcmp(name, "switch-enum")) g_config.enable_enumcheck = enable;
  else if (!strcmp(name, "unreachable-code")) g_config.enable_warn_unreachable = enable;
  else if (!strcmp(name, "infinite-recursion")) g_config.enable_warn_recursion = enable;
  else if (!strcmp(name, "return-local-addr") || !strcmp(name, "use-after-free"))
    g_config.enable_memlint = enable;
  else if (!strcmp(name, "cpp")) {
    if (enable) diag_default(1215); else diag_suppress(1215);
  } else return 0;
  return 1;
}

static int diagnostic_policy(const char *a) {
  static const char *names[] = {"--diag_suppress=", "--diag_remark=", "--diag_error=", 0};
  int i;
  for (i = 0; names[i]; i++) {
    size_t n = strlen(names[i]);
    if (!strncmp(a, names[i], n)) {
      const char *p = a + n;
      if (!*p) return -1;
      while (*p) {
        int code = 0;
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) {
          if (code > 100000) return -1;
          code = code * 10 + (*p++ - '0');
        }
        if (i == 2) diag_force_error(code); else diag_suppress(code);
        if (*p && (*p++ != ',' || !*p)) return -1;
      }
      return 1;
    }
  }
  return 0;
}

static int validate_cpu(ArkOptions *o, int arm_flags) {
  if (!strcmp(o->target, "arm")) {
    const CpuInfo *cpu = cpu_lookup(o->cpu);
    if (!cpu || !cpu->thumb2)
      return fail("ARM backend requires a Thumb-2 CPU (M3/M4/M7/M33): %s",
                  o->cpu ? o->cpu : "(none)");
    o->cpu = cpu->name;
  } else {
    if (arm_flags) return fail("ARM instruction/float options require ARM target", 0);
    if (o->cpu && strcmp(o->cpu, "generic") &&
        !( !strcmp(o->target, "x86_64") && !strcmp(o->cpu, "x86-64")) &&
        !( !strcmp(o->target, "x86") && !strcmp(o->cpu, "i686")) &&
        !( !strcmp(o->target, "riscv32") && !strcmp(o->cpu, "rv32im")))
      return fail("unsupported CPU for selected target: %s", o->cpu);
  }
  return 0;
}

static void usage(void) {
  puts("xhcc 0.2 - multi-target C99/C11 subset compiler\n"
       "Usage: xhcc [TOOL] -E|-S|-c [options] file.c ...\n"
       "  Tools: arm-none-eabi-gcc, armcc, x86_64-linux-gnu-gcc,\n"
       "         i686-linux-gnu-gcc, riscv32-unknown-elf-gcc, xh-c-gcc\n"
       "  -o FILE  -I DIR  -D NAME[=VALUE]  -U NAME  -include FILE\n"
       "  -std=c99|c11|gnu99|gnu11 (C11 subset; no thread library/TLS)\n"
       "  -O0|-O1|-O2  -mcpu=CPU (ARM: Cortex-M3/M4/M7/M33)\n"
       "  -config FILE  -f[no-]FEATURE  --diag_style=gnu|arm|ide|modern\n"
       "  -E -dM: final macro definitions; -E -P: omit line markers\n"
       "  -fsyntax-only  -ffreestanding|-fhosted -f[no-]builtin -undef\n"
       "  -f[no-]record-sysv-abi (x86_64): record IR plan metadata only;\n"
       "    does not enable native GCC aggregate ABI execution\n"
       "  -f[no-]use-sysv-abi-plan (x86_64, default disabled):\n"
       "    certified scalar and aggregate subsets only; see ABI-plan contract\n"
       "    enabled use always implies record, even after -fno-record-sysv-abi;\n"
       "    unsupported aggregate plans are rejected by the backend\n"
       "  -fhosted selects hosted macros; it does not provide a complete libc\n"
       "  -Wall -Wextra (available checks only) -Werror -w\n"
       "  -W[no-]unused[-function|-result], switch-enum, unreachable-code,\n"
       "    infinite-recursion, cpp, return-local-addr/use-after-free (shared check)\n"
       "  --ark-compat ARGS: original Ark CLI, private objects/link/IPO/MDK\n"
       "  -mthumb -mfloat-abi=soft|softfp -mfpu=fpv4-sp-d16|fpv5[-sp]-d16\n"
       "  (armcc: --cpu Cortex-M3 --thumb --fpu "
       "SoftVFP)\n"
       "  -M|-MM|-MD|-MMD -MP -MF FILE -MT TARGET|-MQ QUOTED_TARGET\n"
       "  --private-object  --obj-ir (requires -c --private-object) --memory-report\n"
       "  --package-root DIR (default XHCC_PACKAGE_ROOT or packages)\n"
       "  XHCC_PROTOCOL_MAJOR must be 1 when set; --list-tools -v\n"
       "  @FILE: quoted arguments, cwd-relative nested paths; no expansion\n"
       "  Limits: 512 arguments, 64 sources, 32 KiB storage, 64 KiB response\n"
       "          input, 8 response levels, 511-byte derived output paths.\n"
       "  Multiple sources compile sequentially; explicit -o/-MF/-MT require\n"
       "  one source. Modern linking and unsupported options are rejected.");
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
  const char *root = "packages", *tool =
      !strcmp(XH_MODULE_TARGET, "x86_64") ? "x86_64-linux-gnu-gcc" :
      !strcmp(XH_MODULE_TARGET, "x86") ? "i686-linux-gnu-gcc" :
      !strcmp(XH_MODULE_TARGET, "riscv32") ? "riscv32-unknown-elf-gcc" :
      !strcmp(XH_MODULE_TARGET, "c") ? "xh-c-gcc" : "arm-none-eabi-gcc";
  int i, literal = 0, list_tools = 0, memory_report = 0, result = 0;
  const char *explicit_cpu = 0, *arch = 0, *abi = 0, *fpu = 0;
  char package_dialect[16] = "gcc";
  const char *protocol_major = getenv("XHCC_PROTOCOL_MAJOR");
  const char *environment_root = getenv("XHCC_PACKAGE_ROOT");
  int softfp = 0;
#ifdef XH_ADAPTER
  int wrote_request = 0;
  FILE *request_queue = 0;
#endif
  int width = 0, query = 0;
  int seen_e = 0, seen_s = 0, seen_c = 0, arm_flags = 0;
  memset(&driver, 0, sizeof(driver));
  config_init();
  /* Compiled capabilities are available but opt-in for modern ELF output. */
  g_config.enable_fpu_hw = 0;
  g_config.enable_dwarf = 0;
  diag_init(0, 0);
  diag_set_warnings_as_errors(0);
  diag_set_warnings_disabled(0);
  if (environment_root && *environment_root) root = environment_root;
  if (protocol_major && strcmp(protocol_major, "1")) {
    result = fail("unsupported package protocol major (expected 1): %s", protocol_major);
    goto done;
  }
  for (i = 1; i < argc; i++)
    if (expand_argument(argv[i], 0)) {
      result = 1;
      goto done;
    }
  /* Expand response files before dispatch so both interfaces keep @ support. */
  if (driver.argc && !strcmp(driver.args[0], "--ark-compat")) {
#ifdef XH_ADAPTER
    result = fail("legacy --ark-compat requires the debugall compiler", 0);
    goto done;
#else
    char *legacy_args[XH_DRIVER_MAX_ARGS + 1];
    legacy_args[0] = argv[0];
    for (i = 1; i < driver.argc; i++) legacy_args[i] = driver.args[i];
    legacy_args[driver.argc] = 0;
    diag_shutdown();
    return xh_legacy_main(driver.argc, legacy_args);
#endif
  }
  /* Select package before translating its data-only option aliases. Response
   * files were expanded once above; emitted tokens never expand @ files. */
  {
    char options_path[XH_DRIVER_PATH_BYTES];
    char *inputs[XH_DRIVER_MAX_ARGS], **mapped;
    int count=0, mapped_count=0, skip=0, informational=0, package_args_started=0;
    xh_package_options_load(0);
    if (driver.argc && driver.args[0][0]!='-' &&
        (strlen(driver.args[0])<3 || strcmp(driver.args[0]+strlen(driver.args[0])-2,".c"))) {
      tool=driver.args[0];skip=1;
    }
    for(i=skip;i<driver.argc;i++) {
      const char *a=driver.args[i];
      if(!strcmp(a,"--")){for(;i<driver.argc;i++)inputs[count++]=driver.args[i];break;}
      if(!package_args_started && (!strcmp(a,"--package-root")||!strncmp(a,"--package-root=",15))) {
        root=value(&i,a[14]=='='?a+15:0);if(!root){result=1;goto done;}continue;
      }
      package_args_started=1;
      inputs[count++]=driver.args[i];
      if(xh_package_options_takes_value(a)&&i+1<driver.argc) inputs[count++]=driver.args[++i];
    }
    /* Only standalone informational invocations bypass package validation.
     * Any compilation/options combination validates requirements first. */
    if(count==1 && (!strcmp(inputs[0],"--help")||!strcmp(inputs[0],"-h")||
       !strcmp(inputs[0],"--version")||!strcmp(inputs[0],"-dumpversion")||
       !strcmp(inputs[0],"--list-tools"))) informational=1;
    if(!informational) {
      if(xh_package_adapter_config(root,tool,options_path,sizeof(options_path),package_dialect,sizeof(package_dialect)) ||
         xh_package_options_load(options_path) ||
         xh_package_options_map(count,inputs,&mapped_count,&mapped)) {result=1;goto done;}
      driver.argc=0;
      /* Keep tool outside rewrite storage and rules. */
      driver.args[driver.argc++]=(char *)tool;
      if(mapped_count>=XH_DRIVER_MAX_ARGS){result=fail("mapped argument count exceeded",0);goto done;}
      for(i=0;i<mapped_count;i++)driver.args[driver.argc++]=mapped[i];
    } else {
      driver.argc=0;
      for(i=0;i<count;i++)driver.args[driver.argc++]=inputs[i];
    }
  }
  /* Load configuration first; CLI flags override it regardless of ordering. */
  for (i = 0; i < driver.argc; i++) {
    if (!strcmp(driver.args[i], "--")) break;
    if (!strcmp(driver.args[i], "-config")) {
      const char *path = value(&i, 0);
      FILE *fp;
      if (!path) { result = 1; goto done; }
      fp = fopen(path, "rb");
      if (!fp) { result = fail("cannot open config file: %s", path); goto done; }
      fclose(fp);
      config_load_file(path);
    } else if (!strcmp(driver.args[i], "-o") || !strcmp(driver.args[i], "-I") ||
               !strcmp(driver.args[i], "-D") || !strcmp(driver.args[i], "-U") ||
               !strcmp(driver.args[i], "-include") || !strcmp(driver.args[i], "-MF") ||
               !strcmp(driver.args[i], "-MT") || !strcmp(driver.args[i], "-MQ") || !strcmp(driver.args[i], "--cpu") ||
               !strcmp(driver.args[i], "-mcpu") || !strcmp(driver.args[i], "--fpu") || !strcmp(driver.args[i], "-mfpu") ||
               !strcmp(driver.args[i], "--package-root") ||
               !strcmp(driver.args[i], "--diag_style") || !strcmp(driver.args[i], "--errors") ||
               !strcmp(driver.args[i], "-x") || !strcmp(driver.args[i], "-target") ||
               !strcmp(driver.args[i], "--target") || !strcmp(driver.args[i], "-march") ||
               !strcmp(driver.args[i], "-mabi") || !strcmp(driver.args[i], "-isystem") ||
               !strcmp(driver.args[i], "-iquote")) i++;
  }
  o->opt_level = g_config.opt_level;
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
      puts("xhcc 0.2 (Ark C core), multi-target C99/C11 subset");
      goto done;
    }
    if (!literal && !strcmp(a, "-dumpversion")) { puts("0.2"); goto done; }
    if (!literal && (!strcmp(a, "-dumpmachine") || !strcmp(a, "-print-search-dirs"))) {
      query = !strcmp(a, "-dumpmachine") ? 1 : 2;
      continue;
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
    if (!literal && !strcmp(a, "--obj-ir")) {
#if ARKCC_FEATURE_OBJ_IR
      o->obj_ir = 1;
      continue;
#else
      result = fail("--obj-ir is unavailable in this build", 0); goto done;
#endif
    }
    if (!literal && !strcmp(a, "-v")) {
      o->verbose = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-E")) {
      seen_e = 1;
      continue;
    }
    if (!literal && !strcmp(a, "-dM")) { o->dump_macros = 1; continue; }
    if (!literal && !strcmp(a, "-P")) { o->no_line_markers = 1; continue; }
    if (!literal && !strcmp(a, "-dD")) {
      result = fail("-dD macro definition ordering is not implemented; use -dM -E for final macros", 0);
      goto done;
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
                     !strcmp(a, "-std=c11") || !strcmp(a, "-std=gnu99") ||
                     !strcmp(a, "-std=gnu11"))) {
      o->language_c11 = !strcmp(a, "-std=c11") || !strcmp(a, "-std=gnu11");
      continue;
    }
    if (!literal && (!strcmp(a, "-ffreestanding") || !strcmp(a, "-fhosted"))) {
      o->hosted_mode = !strcmp(a, "-fhosted");
      o->no_builtin = !o->hosted_mode;
      continue;
    }
    if (!literal && !strcmp(a, "-undef")) { o->undef_predefines = 1; continue; }
    if (!literal && (!strcmp(a, "-fbuiltin") || !strcmp(a, "-fno-builtin"))) {
      o->no_builtin = !strcmp(a, "-fno-builtin");
      continue;
    }
    if (!literal && (!strcmp(a, "-mthumb") || !strcmp(a, "--thumb") ||
                     !strcmp(a, "-mfloat-abi=soft") || !strcmp(a, "-mfloat-abi=softfp"))) {
      arm_flags = 1;
      if (!strncmp(a, "-mfloat-abi=", 12)) softfp = !strcmp(a, "-mfloat-abi=softfp");
      continue;
    }
    if (!literal && (!strcmp(a, "-O0") || !strcmp(a, "-O1") || !strcmp(a, "-O2"))) {
      o->opt_level = a[2] - '0';
      apply_flag(a);
      continue;
    }
    if (!literal && !strcmp(a, "-config")) {
      if (!value(&i, 0)) { result = 1; goto done; }
      continue;
    }
    if (!literal && (!strcmp(a, "-g") || !strcmp(a, "-ffunction-sections") ||
                     !strcmp(a, "--split_sections") || !strcmp(a, "--dwarf2") ||
                     !strcmp(a, "--dwarf3"))) {
      result = fail("option requires legacy --ark-compat (standard ELF support unavailable): %s", a);
      goto done;
    }
    if (!literal && config_flag(a)) {
      apply_flag(a);
      continue;
    }
    if (!literal && runtime_flag(a)) continue;
    if (!literal) {
      int policy = diagnostic_policy(a);
      if (policy < 0) { result = fail("invalid diagnostic policy: %s", a); goto done; }
      if (policy) continue;
    }
    if (!literal && !strncmp(a, "--pipeline=", 11)) {
      int profile = pipeline_profile_parse(a + 11);
      if (profile < 0) { result = fail("unknown pipeline: %s", a + 11); goto done; }
      g_config.pipeline_profile = profile;
      continue;
    }
    if (!literal && !strncmp(a, "--size-weight=", 14)) {
#if ARKCC_FEATURE_SIZEWEIGHT
      if (a[14] < '0' || a[14] > '9' || a[15]) { result = fail("size weight expects 0..9: %s", a); goto done; }
      g_config.size_weight = a[14] - '0';
      continue;
#else
      result = fail("size weight unavailable in this build", 0); goto done;
#endif
    }
    if (!literal && warning_flag(a)) continue;
    if (!literal && !strcmp(a, "-fsyntax-only")) { o->syntax_only = 1; continue; }
    if (!literal && (!strcmp(a, "-frecord-sysv-abi") || !strcmp(a, "-fno-record-sysv-abi"))) {
      o->record_sysv_abi = !strcmp(a, "-frecord-sysv-abi");
      continue;
    }
    if (!literal && (!strcmp(a, "-fuse-sysv-abi-plan") || !strcmp(a, "-fno-use-sysv-abi-plan"))) {
      o->use_sysv_abi_plan = !strcmp(a, "-fuse-sysv-abi-plan");
      continue;
    }
    if (!literal && (!strcmp(a, "--diag_style") || !strncmp(a, "--diag_style=", 13))) {
      v = value(&i, a[12] == '=' ? a + 13 : 0);
      if (!v || diag_set_style_name(v)) {
        result = fail("invalid diagnostic style: %s", v ? v : "");
        goto done;
      }
      continue;
    }
    if (!literal && (!strcmp(a, "--errors") || !strncmp(a, "--errors=", 9))) {
      v = value(&i, a[8] == '=' ? a + 9 : 0);
      if (!v || diag_open_sink(v)) { result = fail("cannot open diagnostic output: %s", v ? v : ""); goto done; }
      continue;
    }
    if (!literal && (!strcmp(a, "-x") || !strncmp(a, "-xc", 3))) {
      v = value(&i, a[2] ? a + 2 : 0);
      if (!v || strcmp(v, "c")) { result = fail("only C language is supported: %s", v ? v : ""); goto done; }
      continue;
    }
    if (!literal && (!strcmp(a, "-target") || !strcmp(a, "--target") || !strncmp(a, "--target=", 9))) {
      v = value(&i, !strncmp(a, "--target=", 9) ? a + 9 : 0);
      if (!v) { result = 1; goto done; }
      if (!strcmp(v, "arm") || !strcmp(v, "arm-none-eabi")) tool = "arm-none-eabi-gcc";
      else if (!strcmp(v, "x86_64") || !strcmp(v, "x86_64-linux-gnu")) tool = "x86_64-linux-gnu-gcc";
      else if (!strcmp(v, "x86") || !strcmp(v, "i686-linux-gnu")) tool = "i686-linux-gnu-gcc";
      else if (!strcmp(v, "riscv32") || !strcmp(v, "riscv32-unknown-elf")) tool = "riscv32-unknown-elf-gcc";
      else if (!strcmp(v, "c")) tool = "xh-c-gcc";
      else { result = fail("unsupported target: %s", v); goto done; }
      continue;
    }
    if (!literal && (!strcmp(a, "-march") || !strncmp(a, "-march=", 7) ||
                     !strcmp(a, "-mabi") || !strncmp(a, "-mabi=", 6))) {
      int is_arch = a[2] == 'a' && a[3] == 'r';
      int n = is_arch ? 6 : 5;
      v = value(&i, a[n] == '=' ? a + n + 1 : 0);
      if (!v) { result = 1; goto done; }
      if (is_arch) arch = v; else abi = v;
      continue;
    }
    if (!literal && (!strcmp(a, "-m32") || !strcmp(a, "-m64"))) {
      width = a[2] == '3' ? 32 : 64;
      tool = width == 32 ? "i686-linux-gnu-gcc" : "x86_64-linux-gnu-gcc";
      continue;
    }
    if (!literal && !strcmp(a, "-nostdinc")) { o->no_stdinc = 1; continue; }
    if (!literal && (!strcmp(a, "-isystem") || !strncmp(a, "-isystem", 8) ||
                     !strcmp(a, "-iquote") || !strncmp(a, "-iquote", 7))) {
      int system = !strncmp(a, "-isystem", 8);
      int n = system ? 8 : 7;
      int *count = system ? &o->system_include_dir_count : &o->quote_include_dir_count;
      const char **dirs = system ? o->system_include_dirs : o->quote_include_dirs;
      v = value(&i, a[n] ? a + n : 0);
      if (!v) { result = 1; goto done; }
      if (*count == ARK_PP_MAX_INC_DIRS) { result = fail("include search class limit exceeded", 0); goto done; }
      dirs[(*count)++] = v;
      continue;
    }
    if (!literal && (!strcmp(a, "-MMD") || !strcmp(a, "-MD") ||
                     !strcmp(a, "-M") || !strcmp(a, "-MM"))) {
      o->gen_depend = 1;
      o->deps_only = !strcmp(a, "-M") || !strcmp(a, "-MM");
      o->include_system_deps = !strcmp(a, "-M") || !strcmp(a, "-MD");
      continue;
    }
    if (!literal && !strcmp(a, "-MP")) { o->dep_phony = 1; continue; }
    if (!literal &&
        (!strcmp(a, "--package-root") || !strncmp(a, "--package-root=", 15))) {
      result = fail("--package-root must precede package options (or use XHCC_PACKAGE_ROOT)",0);
      goto done;
    }
    if (!literal && (!strcmp(a, "--cpu") || !strncmp(a, "--cpu=", 6) ||
                     !strcmp(a, "-mcpu") || !strncmp(a, "-mcpu=", 6))) {
      v = value(&i, a[5] == '=' ? a + 6 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      explicit_cpu = v;
      continue;
    }
    if (!literal && (!strcmp(a, "--fpu") || !strncmp(a, "--fpu=", 6) ||
                     !strcmp(a, "-mfpu") || !strncmp(a, "-mfpu=", 6))) {
      v = value(&i, a[5] == '=' ? a + 6 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      arm_flags = 1;
      if (!strcmp(v, "SoftVFP") || !strcmp(v, "softvfp")) fpu = "SoftVFP";
      else if (!strcmp(v, "fpv4-sp-d16") || !strcmp(v, "FPv4-SP-D16")) fpu = "fpv4-sp-d16";
      else if (!strcmp(v, "fpv5-d16") || !strcmp(v, "FPv5-D16")) fpu = "fpv5-d16";
      else if (!strcmp(v, "fpv5-sp-d16") || !strcmp(v, "FPv5-SP-D16")) fpu = "fpv5-sp-d16";
      else { result = fail("unsupported FPU: %s", v); goto done; }
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
    if (!literal && (!strncmp(a, "-MF", 3) || !strncmp(a, "-MT", 3) || !strncmp(a, "-MQ", 3))) {
      v = value(&i, a[3] ? a + 3 : 0);
      if (!v) {
        result = 1;
        goto done;
      }
      if (a[2] == 'F')
        o->depend_file = v;
      else {
        if (o->depend_target) { result = fail("multiple -MT/-MQ targets are unsupported", 0); goto done; }
        o->depend_target = v;
        o->dep_quote_target = a[2] == 'Q';
      }
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
  o->emulate_gcc = !strcmp(package_dialect, "gcc");
  /* Execution consumes the recorded plan even if -fno-record appeared last. */
  if (o->use_sysv_abi_plan) o->record_sysv_abi = 1;
  /* Validate requested output semantics before any query or preprocessing exit.
   * obj_ir records serialized IR inside Ark's private object container only. */
  if (o->obj_ir && (!seen_c || !o->private_object || seen_e || seen_s ||
                    o->deps_only || o->syntax_only)) {
    result = fail("--obj-ir requires -c --private-object without -E/-S/-M/-MM/-fsyntax-only", 0);
    goto done;
  }
  if (query) {
    if (xh_package_apply(root, tool, o)) { result = 1; goto done; }
    if (o->record_sysv_abi && strcmp(o->target, "x86_64")) {
      result = fail(o->use_sysv_abi_plan ? "-fuse-sysv-abi-plan requires x86_64" : "-frecord-sysv-abi requires x86_64 (IR metadata only)", 0); goto done;
    }
    if (query == 1) puts(!strcmp(o->target, "arm") ? "arm-none-eabi" :
      !strcmp(o->target, "x86_64") ? "x86_64-linux-gnu" :
      !strcmp(o->target, "x86") ? "i686-linux-gnu" :
      !strcmp(o->target, "riscv32") ? "riscv32-unknown-elf" : "xh-c");
    else {
      int j;
      printf("install: %s\n", root);
      printf("programs: =in-process\nlibraries: =\ninclude: =");
      for (j = 0; j < o->include_dir_count; j++) printf("%s%s", j ? ";" : "", o->include_dirs[j]);
      putchar('\n');
    }
    goto done;
  }
#ifdef XH_ADAPTER
  o->memory_report = memory_report;
#endif
  if (!driver.source_count) {
    result = fail("no input source files", 0);
    goto done;
  }
  if (!seen_e && !seen_s && !seen_c && !o->deps_only && !o->syntax_only) {
    result = fail("linking is unsupported; specify -E, -S or -c", 0);
    goto done;
  }
  if (driver.source_count > 1 &&
      (o->output_path || o->depend_file || o->depend_target)) {
    result = fail("-o, -MF and -MT require a single source", 0);
    goto done;
  }
  if (!o->gen_depend && (o->depend_file || o->depend_target || o->dep_phony)) {
    result = fail("-MF/-MT/-MQ/-MP require -M/-MM/-MD/-MMD", 0);
    goto done;
  }
  if ((o->dump_macros || o->no_line_markers) && (!seen_e || o->deps_only)) {
    result = fail("-dM/-P require -E without -M/-MM", 0); goto done;
  }
  if (o->syntax_only && (seen_e || seen_s || o->deps_only || o->private_object)) {
    result = fail("-fsyntax-only cannot combine with -E/-S/-M/-MM/--private-object", 0); goto done;
  }
  if (o->deps_only) {
    if (!o->depend_file) o->depend_file = o->output_path ? o->output_path : "-";
    o->output_path = 0;
  }
  o->preprocess_only = seen_e || o->deps_only;
  o->emit_asm = !o->preprocess_only && seen_s;
  o->compile_only = !o->preprocess_only && !seen_s && !o->syntax_only;
  if (o->private_object && !o->compile_only) {
    result = fail("--private-object requires -c without -E/-S", 0);
    goto done;
  }
  if (g_config.debug_info || g_config.split_sections || g_config.dwarf_version >= 2 ||
      g_config.scatter_file || g_config.linker_script) {
    result = fail("config requires legacy --ark-compat (debug/sections/linker layout)", 0);
    goto done;
  }
  if (g_config.enable_fpu_hw) {
    result = fail("hardware floating-point is unsupported by modern packages", 0);
    goto done;
  }
  g_config.fpu_type = "SoftVFP";
  g_config.opt_level = o->opt_level;
  if (xh_package_apply(root, tool, o)) {
    result = 1;
    goto done;
  }
  if (o->record_sysv_abi && strcmp(o->target, "x86_64")) {
    result = fail(o->use_sysv_abi_plan ? "-fuse-sysv-abi-plan requires x86_64" : "-frecord-sysv-abi requires x86_64 (IR metadata only)", 0); goto done;
  }
  if (width && strcmp(o->target, width == 32 ? "x86" : "x86_64")) {
    result = fail("-m32/-m64 conflicts with selected target", 0); goto done;
  }
  if (arch) {
    int valid = (!strcmp(o->target, "riscv32") && !strcmp(arch, "rv32im")) ||
      (!strcmp(o->target, "x86") && (!strcmp(arch, "i386") || !strcmp(arch, "i686"))) ||
      (!strcmp(o->target, "x86_64") && !strcmp(arch, "x86-64"));
    if (!strcmp(o->target, "arm")) {
      if (!strcmp(arch, "armv7-m")) { valid = 1; if (!explicit_cpu) explicit_cpu = "Cortex-M3"; }
      else if (!strcmp(arch, "armv7e-m")) { valid = 1; if (!explicit_cpu) explicit_cpu = "Cortex-M4"; }
      else if (!strcmp(arch, "armv8-m.main")) { valid = 1; if (!explicit_cpu) explicit_cpu = "Cortex-M33"; }
    }
    if (!valid) { result = fail("unsupported architecture for selected target: %s", arch); goto done; }
  }
  if (abi && !((!strcmp(o->target, "riscv32") && !strcmp(abi, "ilp32")) ||
               (!strcmp(o->target, "arm") && !strcmp(abi, "aapcs")) ||
               ((!strcmp(o->target, "x86") || !strcmp(o->target, "x86_64")) && !strcmp(abi, "sysv")))) {
    result = fail("unsupported ABI for selected target: %s", abi); goto done;
  }
  if (explicit_cpu) o->cpu = explicit_cpu;
  if (validate_cpu(o, arm_flags)) { result = 1; goto done; }
  if (softfp) {
    const CpuInfo *cpu = cpu_lookup(o->cpu);
    if (!cpu || !cpu->has_fpu) { result = fail("softfp requires a CPU with an FPU", 0); goto done; }
    if (!fpu) fpu = cpu->fpu_version == 4 ? "fpv4-sp-d16" :
                    cpu->has_fpu_dp ? "fpv5-d16" : "fpv5-sp-d16";
    if ((cpu->fpu_version == 4 && strcmp(fpu, "fpv4-sp-d16")) ||
        (cpu->fpu_version == 5 && strcmp(fpu, "fpv5-sp-d16") &&
          (!cpu->has_fpu_dp || strcmp(fpu, "fpv5-d16")))) {
      result = fail("FPU does not match selected CPU: %s", fpu); goto done;
    }
    g_config.enable_fpu_hw = 1;
    g_config.fpu_type = fpu;
  } else if (fpu && strcmp(fpu, "SoftVFP")) {
    result = fail("hardware -mfpu requires -mfloat-abi=softfp", 0); goto done;
  }
  if (o->verbose) config_print();
  {
    const char *explicit_output = o->output_path;
    const char *explicit_depend = o->depend_file;
    for (i = 0; i < driver.source_count; i++) {
      char output[XH_DRIVER_PATH_BYTES], depend[XH_DRIVER_PATH_BYTES];
      int rc, errors;
      o->input_path = driver.sources[i];
      o->output_path = explicit_output;
      o->depend_file = explicit_depend;
      if (!explicit_output && !o->preprocess_only && !o->syntax_only) {
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
      diag_init(o->input_path, 0);
#ifdef XH_ADAPTER
      {
        int j;
        xh_request_add_diagnostic_arg(0);
        for (j=0;j<driver.argc;j++) {
          const char *a=driver.args[j];
          if (!strcmp(a,"--")) break;
          if (!strncmp(a,"-W",2) || !strcmp(a,"-w") ||
              !strncmp(a,"--diag_",7) || !strncmp(a,"--errors",8)) {
            if (xh_request_add_diagnostic_arg(a)) { result=fail("diagnostic request argument limit",0); break; }
            if ((!strcmp(a,"--diag_style") || !strcmp(a,"--errors")) && j+1<driver.argc)
              if (xh_request_add_diagnostic_arg(driver.args[++j])) { result=1;break; }
          } else if (!strcmp(a,"-o") || !strcmp(a,"-I") || !strcmp(a,"-D") ||
              !strcmp(a,"-U") || !strcmp(a,"-include") || !strcmp(a,"-MF") ||
              !strcmp(a,"-MT") || !strcmp(a,"-MQ") || !strcmp(a,"--cpu") ||
              !strcmp(a,"-mcpu") || !strcmp(a,"--fpu") || !strcmp(a,"-mfpu") ||
              !strcmp(a,"-config") || !strcmp(a,"--package-root") || !strcmp(a,"-x") ||
              !strcmp(a,"-target") || !strcmp(a,"--target") || !strcmp(a,"-march") ||
              !strcmp(a,"-mabi") || !strcmp(a,"-isystem") || !strcmp(a,"-iquote")) j++;
        }
        if(result)break;
      }
      xh_request_set_tool(tool);
      {
        const char *base = getenv("XHCC_REQUEST_OUTPUT");
        char path[XH_DRIVER_PATH_BYTES];
        if (!base || !*base) { rc = fail("missing XHCC_REQUEST_OUTPUT",0); }
        else if (driver.source_count == 1) {
          rc = xh_request_write(o, base);
          if (!rc) wrote_request = 1;
        } else {
          int n;
          if (!request_queue) {
            n = snprintf(path,sizeof(path),"%s.queue",base);
            if (n<0 || (size_t)n>=sizeof(path)) { rc=fail("request queue path too long",0); goto request_done; }
            request_queue=fopen(path,"wb");
            if (!request_queue) { rc=fail("cannot open request queue",0); goto request_done; }
          }
          n=snprintf(path,sizeof(path),"%s.%d",base,i);
          if (n<0 || (size_t)n>=sizeof(path)) { rc=fail("request path too long",0); goto request_done; }
          if (strchr(path,'\n') || strchr(path,'\r')) {rc=fail("invalid newline in request queue path",0);goto request_done;}
          rc=xh_request_write(o,path);
          if (!rc && fprintf(request_queue,"%s\n",path)<0) rc=1;
          if (!rc) wrote_request=2;
        }
request_done:
        if (rc) fail("cannot write CompileRequest to XHCC_REQUEST_OUTPUT", 0);
      }
#else
      mem_pool_reset();
      rc = ark_compile(o);
      if (memory_report)
        fprintf(stderr, "[xhcc] %s: arena peak %d/%d B\n", o->input_path,
                mem_pool_high(), mem_pool_capacity());
#endif
      errors = diag_errors();
      if (rc && !errors)
        fprintf(stderr, "[xhcc] compilation failed (%d): %s\n", rc,
                o->input_path);
      diag_shutdown();
      if (rc || errors)
        result = 1;
    }
  }
done:
#ifdef XH_ADAPTER
  if (request_queue && fclose(request_queue)) result=fail("cannot close request queue",0);
  if (!result) {
    const char *stage_path = getenv("XHCC_STAGE_RESULT");
    FILE *stage = stage_path && *stage_path ? fopen(stage_path, "wb") : 0;
    if (!stage) result = fail("cannot open XHCC_STAGE_RESULT", 0);
    else {
      int written = fputs(wrote_request == 2 ? "queue\n" : wrote_request ? "request\n" : "complete\n", stage);
      int closed = fclose(stage);
      if (written == EOF || closed) result = fail("cannot write XHCC_STAGE_RESULT", 0);
    }
  }
#endif
  diag_shutdown();
  return result;
}

#ifndef XH_DRIVER_NO_MAIN
int main(int argc, char **argv) { return xh_driver_main(argc, argv); }
#endif
