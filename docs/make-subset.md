# xhccm make subset

`xhccm` is the compiler driver with an in-process C99 make subset:

```
xhccm make [TOOL] [-f file] [-C dir] [-n/-B/-s/-k] [NAME=VALUE] [targets]
xhccm arm-none-eabi-gcc -c source.c -o source.o
```

The first form runs make; all other invocations call `xh_driver_main` directly.
The optional leading TOOL is recognized through package discovery, including
package-provided names. It supplies the default `CC` (otherwise
`arm-none-eabi-gcc`). A makefile assignment to
`CC` can replace that default; a command-line `CC=...` overrides makefile
assignments. A compiler name written explicitly in a recipe selects that named
compiler, regardless of the default TOOL or `CC`. Package discovery uses
`XHCC_PACKAGE_ROOT` or `packages`; after `-C`, recipes must supply a suitable
`--package-root` or an absolute `XHCC_PACKAGE_ROOT`. TOOL recognition happens
before `-C`.

```
CC ?= arm-none-eabi-gcc
CFLAGS = -O2
.PHONY: all clean
all: one.o two.o
%.o: %.c
	$(CC) $(CFLAGS) -c "$<" -o "$@"
clean:
	rm -f one.o two.o
```

Supported syntax and behavior:

- Recursive `=`, immediate `:=`, conditional `?=`, and append `+=` variables.
  `$(NAME)`, `${NAME}`, and single-character variables expand; `$$` yields a
  literal dollar. Command assignments have priority over file assignments.
- Nested references and text functions `strip`, `addprefix`, `addsuffix`,
  `notdir`, `basename`, and `subst`. Function arguments can contain nested
  variable references/functions. Unknown functions and malformed references fail.
- `ifeq (left,right)`, `ifneq (left,right)`, `ifdef NAME`, `ifndef NAME`, `else`,
  and `endif`, up to eight nested blocks per file. Comparisons expand both sides;
  `ifdef` tests whether the named variable has a nonempty raw value. Inactive
  branches skip assignments, includes, rules, and recipes. Only parenthesized
  comparisons are supported; quoted comparison syntax and `else ifeq` chains
  are rejected. Each included file must balance its own conditions.
- Backslash/newline continuations, blank lines, `#` comments outside recipes,
  `include`, `-include`, and `sinclude`. Includes are only read; missing optional
  includes are ignored. Include paths follow the current working directory.
- One explicit target per rule, whitespace-separated prerequisites, `.PHONY`,
  one-percent pattern targets/prerequisites, `.c.o` suffix recipes, and a built-in
  `.c` to `.o` compilation recipe using `CC`, `CPPFLAGS`, and `CFLAGS`.
  Prerequisites after `|` are order-only: they are built and failures propagate,
  but their mtime/rebuilding does not trigger the target recipe. They are omitted
  from `$^` and `$<`; `$|` is not implemented.
- Tab-prefixed recipe lines; automatic `$@`, `$<`, `$^` (deduplicated), and `$*`.
  `$*` is the pattern/suffix stem and is empty for ordinary explicit rules.
- Dependency traversal, missing-file errors, cycle detection, mtime-based
  rebuilding, and propagation of recipe/dependency failures. Successful rebuilt
  prerequisites trigger their parents even on coarse timestamp filesystems.
- `-n` prints without executing, `-B` rebuilds, `-s` suppresses recipe echo, `-k`
  continues independent prerequisites/targets. Recipe prefixes `@` and `-`
  suppress echo and ignore an executed command's failure respectively.
- Recipe tokenization supports single/double quotes, quoted portions inside an
  argument, backslash escaping, and spaces in paths. Quote paths in rules too.
  This is a deterministic token parser, not an operating-system shell.

Compiler commands call `xh_driver_main` in the same process; package discovery
validates command names. No shell, `system`, `popen`, or subprocess is used.
Unknown commands fail. The built-in commands are `echo [words...]`,
`mkdir -p paths...`, `rm -f files...`, and `cp source destination`. They perform
literal file operations without globbing or environment expansion. Shell
operators (`;`, `&`, `|`, `<`, `>`, backticks) are explicitly rejected, including
inside quotes. Literal dollars can be printed with `$$`; command substitution
is unsupported. An ignored command failure is reported, not silently swallowed.

This does **not** implement full GNU make: functions beyond the six listed,
conditionals beyond the listed forms, target-specific variables,
export/environment variable import, grouped
or multiple targets, double-colon/static pattern rules, inline recipes,
secondary expansion, wildcard expansion, parallel jobs, recursive make, shell
scripts, and linking are outside this subset. Unsupported statements fail.
Duplicate explicit rules are not merged; the first matching rule wins. Pattern
selection is first-match without GNU implicit-rule search/backtracking.

Make state uses dynamically allocated, tracked storage with a 64 KiB default
budget (`XH_MAKE_MEMORY`, overridable at compilation). Graph traversal scratch
frames share that budget and are freed as traversal returns. Text/expanded
lines are limited to 2047 bytes, words to 128, variable expansion to eight nested
references, includes to eight levels, and graph traversal to 32 levels; the memory
budget can reject a graph before its depth limit. All make allocations are freed
on exit and the entry restores its original current directory. Compiler memory
and package state are managed separately by the driver.

Run `tests/test_make.py` after building `xhccm.wasm`. It defaults to Wasmtime and
`build/wasm/xhccm.wasm`; `XHCCM_COMMAND` accepts a JSON command array for
another built artifact. The tests execute two actual C compilations in one make
invocation, inspect ELF headers, verify incremental/forced rebuilds and variable
priority, test explicit compiler selection and quoted paths, and reject errors,
cycles, and unsupported shell commands. The test does not build the compiler.
