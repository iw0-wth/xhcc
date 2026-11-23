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
  literal dollar. Command assignments have priority over ordinary file assignments.
  `override NAME =`, `:=`, `+=`, and `?=` mark assignments with `override` origin
  and priority above command arguments. Conditional `?=` still preserves any
  existing definition. Ordinary `undefine NAME` removes file/default definitions
  but preserves command-line/override definitions; `override undefine NAME`
  removes those too. Undefine expands its one variable name and rejects extra
  names. Later ordinary assignments cannot replace an override definition.
  `value NAME` returns a variable's stored text without expanding it; `flavor NAME`
  reports `undefined`, `recursive`, or `simple`. Their name arguments expand,
  while a raw stored `$(error ...)` remains unevaluated when returned by value.
- One explicit target can define local variables using `target: NAME = value`,
  `:=`, `+=`, or `?=`, optionally `target: override NAME = value`. The local
  bindings apply during that target and inherit into its prerequisites; global
  values are restored afterwards. Command-line/override values retain priority
  unless a local override is specified. `?=` preserves a definition already
  visible in the inherited context. Immediate local `:=` expands when the
  makefile is read using the then-current global context; other bindings resolve
  when the target is entered, in declaration order. A shared prerequisite uses
  its first traversal's context, as it is built once. Variable-only declarations
  do not choose a default goal and cannot carry recipes. Multiple-target
  variable declarations remain unsupported.
  Pattern-specific forms such as `%.o: CFLAGS += -O1` and `src/%.o: CC = tool`
  use the same bindings and precedence. Matching patterns are applied from
  longest stem to shortest stem, with declaration order for equal stem lengths;
  explicit target bindings then apply. Patterns override ordinary inherited
  parent values, while command-line and override protections still apply.
  At most 128 variable patterns may match one target; multiple `%` characters
  in a variable pattern are rejected. Scope storage shares the make budget.
  GNU make 4.4.1 comparison confirms that `rv/unit.o` applies `%.o`, `rv/%.o`,
  then `%unit.o`: their stems have lengths 7, 4, and 3 respectively. Explicit
  target bindings apply after these, and ordinary inherited parent bindings
  are replaced by matching patterns. The integration tests exercise both
  directory-selected compiler architectures and restoration outside that scope.
- Nested references and text functions `strip`, `addprefix`, `addsuffix`,
  `notdir`, `basename`, `subst`, `filter`, `filter-out`, `sort`, and `patsubst`.
  Function arguments can contain nested variable references/functions. `filter`
  and `filter-out` accept whitespace-separated patterns with one `%`; `patsubst`
  replaces matching words using the first `%` as the stem placeholder. `sort`
  uses deterministic byte ordering and removes duplicates. Substitution references
  `$(VAR:%.c=%.o)` and `$(VAR:.c=.o)` support pattern and word-suffix replacement.
  Text functions split words on whitespace; they do not interpret path quotes.
  `dir` returns each word's directory including `/`, or `./` without a slash;
  `suffix` returns its final dot suffix (excluding directory components).
  `word n,text` selects a one-based word or empty text when out of range, and
  `words text` counts words. A zero, negative, malformed or overflowing word index
  fails. Paths use `/`; these functions share the normal text and word limits.
  Unknown functions and malformed references fail.
- `if condition,then[,else]` expands the condition, trims surrounding whitespace,
  and expands only the selected branch. Missing else yields empty text; errors
  or warnings in an unselected branch are not evaluated.
  `foreach variable,list,text` expands the name and word list, then expands text
  once per list word with a temporary simple variable. The local binding overrides
  file/command-line values during the loop, reports origin `automatic`, and is
  restored on exit (including failure). Nested loops are supported within the
  ordinary expansion-depth, 128-word, 2047-byte and memory limits.
  `findstring find,in` returns the searched substring if present, otherwise empty.
  `join list1,list2` concatenates corresponding words, preserving unmatched words
  from the longer list. Both lists use normal whitespace splitting.
- `wildcard` enumerates existing paths with `*` and `?` in the final path
  component. Directory components must be literal; bracket classes, backslashes,
  and whitespace in discovered paths are rejected. Unmatched patterns contribute
  no words. Results from all patterns are byte-sorted and deduplicated, capped
  at 128 paths and the text limit. Hidden names require a pattern starting with
  `.`; `.` and `..` are omitted. This function reads directories through WASI/POSIX
  `opendir`/`readdir`; recipe arguments and ordinary prerequisites are not globbed.
- `origin NAME` returns `undefined`, `default`, `file`, `command line`, `override`, or
  `automatic` for the supported automatic variables during a recipe. Built-in
  `CC` and the TOOL default have origin `default`; file assignment changes it
  to `file`, while ignored `?=` and overridden file assignments preserve origin.
  Environment import and export directives remain unsupported.
- `error text` expands its message and stops with an error. `warning text`
  emits an expanded diagnostic and returns empty text. These work in recursive
  expansion, recipes, or as standalone `$(error ...)`/`$(warning ...)` lines;
  an inactive conditional branch does not evaluate them.
- `ifeq (left,right)`, `ifneq (left,right)`, `ifdef NAME`, `ifndef NAME`, `else`,
  and `endif`, up to eight nested blocks per file. Comparisons expand both sides;
  `ifdef` tests whether the named variable has a nonempty raw value. Inactive
  branches skip assignments, includes, rules, and recipes. Only parenthesized
  comparisons are supported; quoted comparison syntax and `else ifeq` chains
  are rejected. Each included file must balance its own conditions.
- Backslash/newline continuations, blank lines, `#` comments outside recipes,
  `include`, `-include`, and `sinclude`. Includes are only read; missing optional
  includes are ignored. Include paths follow the current working directory.
- Explicit rules with whitespace-separated prerequisites, `.PHONY`,
  one-percent pattern targets/prerequisites, `.c.o` suffix recipes, and a built-in
  `.c` to `.o` compilation recipe using `CC`, `CPPFLAGS`, and `CFLAGS`.
  Prerequisites after `|` are order-only: they are built and failures propagate,
  but their mtime/rebuilding does not trigger the target recipe. They are omitted
  from `$^` and `$<`; `$|` is not implemented.
  Repeated explicit declarations merge and deduplicate prerequisites, with a
  normal prerequisite taking precedence over an order-only occurrence. At most
  one declaration may supply recipes; a second nonempty recipe block fails.
  Multiple explicit targets on a line (for example `one.o two.o: shared.c`)
  receive the same prerequisites and recipe text. Each target independently
  checks timestamps and executes the recipe using its own automatic variables
  and target-specific bindings. These are independent targets, not grouped
  outputs: deleting one target rebuilds only that target. Repeated names in a
  declaration are deduplicated. Multiple pattern targets and multi-target
  variable declarations are rejected. Included `.d` prerequisites combine with
  a matching pattern recipe. Empty explicit dummy rules support `-MP` output.
  Includes accept multiple filenames, with missing files ignored for optional
  includes. Includes are read only; make does not regenerate or reread them.
- Tab-prefixed recipe lines; automatic `$@`, `$<`, `$^` (deduplicated), and `$*`.
  `$*` is the pattern/suffix stem and is empty for ordinary explicit rules.
  `$(@D)`, `$(@F)`, `$(<D)` and `$(<F)` select the directory or filename of
  target/first normal prerequisite. Automatic directories omit the final `/`
  and return `.` when there is no directory component.
- Dependency traversal, missing-file errors, cycle detection, mtime-based
  rebuilding, and propagation of recipe/dependency failures. Successful rebuilt
  prerequisites trigger their parents even on coarse timestamp filesystems.
- `-n` prints without executing, `-B` rebuilds, `-s` suppresses recipe echo, `-k`
  continues independent prerequisites/targets. Recipe prefixes `@` and `-`
  suppress echo and ignore an executed command's failure respectively.
  `-j1` and `-j 1` select the existing serial execution; other job counts and bare
  `-j` fail explicitly. `.DEFAULT_GOAL` selects one expanded default target when
  no target argument was given; command-line target arguments take precedence.
  Long aliases are `--always-make`, `--dry-run`, `--silent`, `--keep-going`,
  `--file NAME`/`--file=NAME`, and `--directory DIR`/`--directory=DIR`.
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

This does **not** implement full GNU make: functions beyond the twenty-four listed,
conditionals beyond the listed forms, multiple-target variable declarations,
export/environment variable import, grouped targets (`&:`), multiple pattern
targets, double-colon/static pattern rules, inline recipes,
secondary expansion, implicit wildcard expansion outside the function,
parallel jobs, recursive make, shell
scripts, and linking are outside this subset. Unsupported statements fail.
Pattern rules retain first-match selection without GNU implicit-rule
search/backtracking; duplicate pattern rules are not merged.

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
