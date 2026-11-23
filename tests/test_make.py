"""Real xhccm integration tests; build separately, then run this file.
XHCCM_COMMAND is a JSON argv array. Default: Wasmtime build/wasm/xhccm.wasm.
The implementation never launches processes; this host-side test harness does.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def command():
    if os.environ.get('XHCCM_COMMAND'):
        return json.loads(os.environ['XHCCM_COMMAND'])
    runtime = shutil.which('wasmtime')
    if not runtime:
        candidate = Path(r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
        if candidate.exists():
            runtime = str(candidate)
    if not runtime:
        raise RuntimeError('wasmtime unavailable; set XHCCM_COMMAND')
    return [runtime, 'run', '--dir=.', str(ROOT / 'build/wasm/xhccm.wasm')]


class MakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()

    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix='make-', dir=ROOT)
        self.directory = Path(self.workspace.name)
        self.relative = self.directory.relative_to(ROOT).as_posix()
        for name, value in [('one', 1), ('two', 2)]:
            (self.directory / (name + '.c')).write_text(
                f'int {name}(void) {{ return {value}; }}\n')

    def tearDown(self):
        self.workspace.cleanup()

    def makefile(self, text):
        (self.directory / 'Makefile').write_text(text)

    def run_make(self, *args, success=True):
        # Environment relative package paths are relative to make's -C directory.
        result = subprocess.run(
            [*self.command, 'make', *args, '-C', self.relative], cwd=ROOT,
            capture_output=True, text=True, timeout=90)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    @staticmethod
    def recipe(tool='$(CC)'):
        return f'{tool} --package-root ../packages $(CFLAGS) -c "$<" -o "$@"'

    def test_two_sources_incremental_and_force(self):
        self.makefile('all: one.o two.o\n%.o: %.c\n\t' + self.recipe() + '\n')
        result = self.run_make('arm-none-eabi-gcc')
        self.assertEqual(result.stdout.count(' -c '), 2)
        old = {}
        for name in ('one.o', 'two.o'):
            path = self.directory / name
            self.assertEqual(path.read_bytes()[:4], b'\x7fELF')
            old[name] = path.stat().st_mtime_ns
        self.assertNotIn(' -c ', self.run_make('arm-none-eabi-gcc').stdout)
        self.assertEqual(old, {n: (self.directory / n).stat().st_mtime_ns for n in old})
        self.assertEqual(self.run_make('arm-none-eabi-gcc', '-B').stdout.count(' -c '), 2)
        # Deterministically set the source later than both objects, without sleeps.
        future = max(old.values()) + 2_000_000_000
        os.utime(self.directory / 'two.c', ns=(future, future))
        result = self.run_make('arm-none-eabi-gcc')
        self.assertIn('"two.c"', result.stdout)
        self.assertNotIn('"one.c"', result.stdout)

    def test_variable_precedence_and_explicit_compiler(self):
        (self.directory / 'two.c').write_text(
            '#if VALUE != 7\n#error bad variable injection\n#endif\nint two(void){return VALUE;}\n')
        self.makefile('CC = missing-tool\nCFLAGS = -DVALUE=0\n'
                      'two.o: two.c\n\t' + self.recipe() + '\n')
        self.run_make('arm-none-eabi-gcc', 'CC=x86_64-linux-gnu-gcc', 'CFLAGS=-DVALUE=7')
        data = (self.directory / 'two.o').read_bytes()
        self.assertEqual(int.from_bytes(data[18:20], 'little'), 62)
        self.makefile('two.o: two.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n')
        self.run_make('x86_64-linux-gnu-gcc', '-B', 'CFLAGS=-DVALUE=7')
        self.assertEqual(int.from_bytes((self.directory / 'two.o').read_bytes()[18:20], 'little'), 40)

    def test_variables_include_continuation_automatic_and_builtins(self):
        (self.directory / 'vars.mk').write_text('A = late\nB := $(A)\nA = now\nQ ?= first\nQ ?= second\nQ += tail\n')
        self.makefile('include vars.mk\n-include absent.mk\n.PHONY: all\n'
                      'all: one.c \\\n two.c one.c # comment\n'
                      '\t@echo $(A) ${B} $(Q) $@ $< $^ $$literal\n'
                      '\tmkdir -p "space dir"\n\tcp one.c "space dir/copy.c"\n'
                      '\trm -f "space dir/copy.c"\n')
        result = self.run_make()
        self.assertIn('now late first tail all one.c one.c two.c $literal', result.stdout)
        self.assertTrue((self.directory / 'space dir').is_dir())
        self.assertFalse((self.directory / 'space dir/copy.c').exists())

    def test_quoted_paths_pattern_and_dry_run(self):
        (self.directory / 'space name.c').write_text('int quoted(void){return 3;}\n')
        self.makefile('all: "space name.o"\n%.o: "%.c"\n\t' + self.recipe() + '\n')
        self.run_make('arm-none-eabi-gcc', '-n')
        self.assertFalse((self.directory / 'space name.o').exists())
        self.run_make('arm-none-eabi-gcc')
        self.assertEqual((self.directory / 'space name.o').read_bytes()[:4], b'\x7fELF')

    def test_multiple_explicit_compilers_same_process(self):
        self.makefile('all: one.o two.o\none.o: one.c\n\t' +
                      self.recipe('arm-none-eabi-gcc') + '\ntwo.o: two.c\n\t' +
                      self.recipe('x86_64-linux-gnu-gcc') + '\n')
        self.run_make('arm-none-eabi-gcc')
        self.assertEqual(int.from_bytes((self.directory / 'one.o').read_bytes()[18:20], 'little'), 40)
        self.assertEqual(int.from_bytes((self.directory / 'two.o').read_bytes()[18:20], 'little'), 62)

    def test_suffix_and_builtin_rule(self):
        self.makefile('all: two.o\n.c.o:\n\t' + self.recipe() + '\n')
        self.run_make('arm-none-eabi-gcc')
        self.assertEqual((self.directory / 'two.o').read_bytes()[:4], b'\x7fELF')
        (self.directory / 'two.o').unlink()
        self.makefile('all: two.o\n')
        self.run_make('CC=arm-none-eabi-gcc --package-root ../packages')
        self.assertEqual((self.directory / 'two.o').read_bytes()[:4], b'\x7fELF')

    def test_existing_object_without_source_is_leaf(self):
        self.makefile('all: prebuilt.o\n')
        prebuilt = self.directory / 'prebuilt.o'
        prebuilt.write_bytes(b'prebuilt-object')
        old = prebuilt.stat().st_mtime_ns
        self.assertNotIn(' -c ', self.run_make('arm-none-eabi-gcc').stdout)
        self.assertEqual(prebuilt.read_bytes(), b'prebuilt-object')
        self.assertEqual(prebuilt.stat().st_mtime_ns, old)
        prebuilt.unlink()
        self.assertIn('no rule', self.run_make('arm-none-eabi-gcc', success=False).stderr)
        # An existing implicit input must still trigger compilation when newer.
        self.makefile('all: two.o\n')
        self.run_make('CC=arm-none-eabi-gcc --package-root ../packages')
        source = self.directory / 'two.c'
        future = (self.directory / 'two.o').stat().st_mtime_ns + 2_000_000_000
        os.utime(source, ns=(future, future))
        self.assertIn(' -c ', self.run_make('CC=arm-none-eabi-gcc --package-root ../packages').stdout)

    def test_conditions_and_nested_text_functions(self):
        self.makefile('MODE ?= debug\nDIR = obj/\nFILES = src/one.c src/two.c\n'
                      'ifeq ($(strip $(MODE)),debug)\nCFLAGS = -DVALUE=7\n'
                      'ifdef DIR\nSELECTED = yes\nelse\nSELECTED = wrong\nendif\n'
                      'else\ninclude must-not-open.mk\nBAD := $(unsupported nope)\n'
                      'broken inactive statement\nendif\n'
                      'ifneq ($(MODE),release)\nifndef ABSENT\nEXTRA = enabled\nendif\nendif\n'
                      'NAMES := $(addprefix $(DIR),$(addsuffix .o,$(basename $(notdir $(FILES)))))\n'
                      'all: two.o\n\t@echo $(SELECTED) $(EXTRA) $(NAMES) $(subst one,ONE,$(NAMES))\n'
                      'two.o: two.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n')
        (self.directory / 'two.c').write_text(
            '#if VALUE != 7\n#error conditional flag missing\n#endif\nint two(void){return VALUE;}\n')
        result = self.run_make()
        self.assertIn('yes enabled obj/one.o obj/two.o obj/ONE.o obj/two.o', result.stdout)

    def test_order_only_does_not_trigger_rebuild(self):
        self.makefile('.PHONY: prepare\nall: two.o\n'
                      'two.o: two.c|prepare\n\t' + self.recipe('arm-none-eabi-gcc') + '\n'
                      '\t@echo normal=$^ first=$<\nprepare:\n\tmkdir -p generated\n')
        first = self.run_make()
        self.assertIn('normal=two.c first=two.c', first.stdout)
        old = (self.directory / 'two.o').stat().st_mtime_ns
        second = self.run_make()
        self.assertIn('mkdir -p generated', second.stdout)
        self.assertNotIn(' -c ', second.stdout)
        self.assertEqual((self.directory / 'two.o').stat().st_mtime_ns, old)

    def test_condition_and_function_errors(self):
        cases = ['else\n', 'ifeq (a,a)\nall:\n',
                 'ifeq (a,a)\nelse ifeq (b,b)\nendif\n',
                 'all:\n\techo $(unknown value)\n',
                 'all:\n\techo $(addprefix one)\n',
                 'all:\n\techo $(strip $(missing)\n',
                 'ifdef X\n' * 9 + 'endif\n' * 9]
        for content in cases:
            with self.subTest(content=content):
                self.makefile(content)
                self.run_make(success=False)

    def test_filter_sort_patsubst_and_substitution_references(self):
        self.makefile('SOURCES = two.c note.txt one.c two.c sub/third.c\n'
                      'CFILES := $(sort $(filter %.c,$(SOURCES)))\n'
                      'OBJECTS := $(patsubst %.c,%.o,$(CFILES))\n'
                      'all: $(filter two.o,$(OBJECTS))\n'
                      '\t@echo sorted=$(CFILES)\n'
                      '\t@echo other=$(filter-out %.c,$(SOURCES))\n'
                      '\t@echo suffix=$(CFILES:.c=.o)\n'
                      '\t@echo pattern=$(CFILES:%.c=build/%.o)\n'
                      '\t@echo nested=$(patsubst %.c,%.o,$(filter %.c,$(sort $(SOURCES))))\n'
                      'two.o: two.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n')
        result = self.run_make()
        self.assertIn('sorted=one.c sub/third.c two.c', result.stdout)
        self.assertIn('other=note.txt', result.stdout)
        self.assertIn('suffix=one.o sub/third.o two.o', result.stdout)
        self.assertIn('pattern=build/one.o build/sub/third.o build/two.o', result.stdout)
        self.assertIn('nested=one.o sub/third.o two.o', result.stdout)
        self.assertEqual((self.directory / 'two.o').read_bytes()[:4], b'\x7fELF')

    def test_default_goal_and_serial_job_option(self):
        self.makefile('GOAL = selected\n.DEFAULT_GOAL = $(GOAL)\n'
                      'first:\n\techo unwanted\nselected:\n\techo chosen\n')
        for flags in [('-j1',), ('-j', '1')]:
            result = self.run_make(*flags)
            self.assertIn('chosen', result.stdout)
            self.assertNotIn('unwanted', result.stdout)
        result = self.run_make('first', '-j1')
        self.assertIn('unwanted', result.stdout)
        self.assertNotIn('chosen', result.stdout)
        for flags in [('-j2',), ('-j', '4'), ('-j',)]:
            self.assertIn('only serial', self.run_make(*flags, success=False).stderr)
        self.makefile('.DEFAULT_GOAL = first selected\nfirst:\nselected:\n')
        self.assertIn('.DEFAULT_GOAL', self.run_make(success=False).stderr)

    def test_wildcard_sources_sorting_and_optional_includes(self):
        (self.directory / '.hidden.c').write_text('invalid hidden source\n')
        self.makefile('SOURCES := $(wildcard *.c one.c absent/*.c)\n'
                      '-include $(wildcard missing*.mk)\n'
                      'all: $(SOURCES:.c=.o)\n'
                      '\t@echo sources=$(SOURCES)\n'
                      '\t@echo question=$(wildcard t?o.c)\n'
                      '%.o: %.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n')
        result = self.run_make()
        self.assertIn('sources=one.c two.c', result.stdout)
        self.assertIn('question=two.c', result.stdout)
        for name in ('one.o', 'two.o'):
            self.assertEqual((self.directory / name).read_bytes()[:4], b'\x7fELF')
        self.assertFalse((self.directory / '.hidden.o').exists())
        self.assertNotIn(' -c ', self.run_make().stdout)

    def test_origin_error_and_warning(self):
        self.makefile('BEFORE := $(origin CC)\nCC ?= ignored\nAFTER := $(origin CC)\n'
                      'FILE = value\nFILE += tail\nCLI = ignored\n'
                      '$(warning notice $(FILE))\n'
                      'all:\n\t@echo $(BEFORE) $(AFTER) $(origin FILE) "$(origin CLI)" $(origin MISSING) $(origin @)\n')
        result = self.run_make('arm-none-eabi-gcc', 'CLI=external')
        self.assertIn('default default file command line undefined automatic', result.stdout)
        self.assertIn('warning: notice value tail', result.stderr)
        self.makefile('CC = arm-none-eabi-gcc\nall:\n\t@echo $(origin CC)\n')
        self.assertIn('file', self.run_make().stdout)
        self.makefile('$(error stop $(strip  now ))\nall:\n')
        self.assertIn('error: stop now', self.run_make(success=False).stderr)
        self.makefile('all:\n\t-echo $(error recipe-stop)\n')
        self.assertIn('recipe-stop', self.run_make(success=False).stderr)

    def test_wildcard_limits_and_rejected_patterns(self):
        for index in range(129):
            (self.directory / f'f{index:03}.dat').write_text('')
        self.makefile('FILES := $(wildcard *.dat)\nall:\n')
        self.assertIn('too many wildcard paths', self.run_make(success=False).stderr)
        for pattern in ['[ab].c', '*/one.c']:
            self.makefile(f'FILES := $(wildcard {pattern})\nall:\n')
            self.assertIn('unsupported', self.run_make(success=False).stderr)

    def test_dependency_files_merge_and_header_rebuild(self):
        (self.directory / 'header.h').write_text('#define HEADER_VALUE 7\n')
        for name in ('one', 'two'):
            (self.directory / f'{name}.c').write_text(
                '#include "header.h"\nint ' + name + '(void){return HEADER_VALUE;}\n')
        self.makefile('OBJECTS = one.o two.o\nall: $(OBJECTS)\n'
                      '-include $(OBJECTS:.o=.d) absent-one.d absent-two.d\n'
                      '%.o: %.c\n\t' + self.recipe('arm-none-eabi-gcc') + ' -I. -MMD -MP\n')
        self.assertEqual(self.run_make().stdout.count(' -c '), 2)
        for name in ('one.d', 'two.d'):
            self.assertIn('header.h', (self.directory / name).read_text())
        self.assertNotIn(' -c ', self.run_make().stdout)
        future = max((self.directory / n).stat().st_mtime_ns for n in ('one.o', 'two.o')) + 2_000_000_000
        os.utime(self.directory / 'header.h', ns=(future, future))
        self.assertEqual(self.run_make().stdout.count(' -c '), 2)

    def test_multiple_target_dependency_declarations_and_recipe_conflict(self):
        (self.directory / 'header.h').write_text('#define X 1\n')
        self.makefile('all: two.o\n'
                      'two.o two.d: two.c header.h\n'
                      'two.o: two.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n'
                      '\t@echo unique=$^\n'
                      'two.o: header.h\nmissing-header.h:\n')
        result = self.run_make()
        self.assertIn('unique=two.c header.h', result.stdout)
        self.assertNotIn(' -c ', self.run_make().stdout)
        self.makefile('all: missing-header.h\nmissing-header.h:\n')
        self.run_make()
        self.makefile('all:\n\techo first\nall:\n\techo second\n')
        self.assertIn('conflicting recipes', self.run_make(success=False).stderr)
        self.makefile('all other:\n\techo unsupported\n')
        self.assertIn('multiple-target', self.run_make(success=False).stderr)

    def test_path_functions_automatic_parts_and_long_options(self):
        (self.directory / 'src').mkdir()
        (self.directory / 'src/two.c').write_text('int two(void){return 2;}\n')
        self.makefile('SOURCES = src/two.c one.c README\n'
                      'all: obj/deep/two.o\n'
                      '\t@echo dirs=$(dir $(SOURCES)) suffixes=$(suffix $(SOURCES))\n'
                      '\t@echo second=$(word 2,$(SOURCES)) count=$(words $(SOURCES))\n'
                      '\t@echo flat=$(@D)/$(@F)\n'
                      'obj/deep/two.o: src/two.c\n'
                      '\tmkdir -p "$(@D)"\n'
                      '\tarm-none-eabi-gcc --package-root ../packages -c "$(<D)/$(<F)" -o "$(@D)/$(@F)"\n')
        (self.directory / 'Alternate.mk').write_text((self.directory / 'Makefile').read_text())
        result = self.run_make('--file=Alternate.mk', '--dry-run')
        self.assertFalse((self.directory / 'obj').exists())
        self.assertIn('obj/deep', result.stdout)
        result = self.run_make('--file=Alternate.mk')
        self.assertIn('dirs=src/ ./ ./ suffixes=.c .c', result.stdout)
        self.assertIn('second=one.c count=3', result.stdout)
        self.assertIn('flat=./all', result.stdout)
        self.assertEqual((self.directory / 'obj/deep/two.o').read_bytes()[:4], b'\x7fELF')
        self.assertNotIn(' -c ', self.run_make('--file=Alternate.mk').stdout)
        self.assertIn(' -c ', self.run_make('--file=Alternate.mk', '--always-make').stdout)
        result = self.run_make('--silent')
        self.assertNotIn('mkdir -p', result.stdout)
        # Independent invocation tests the long directory alias without -C twice.
        result = subprocess.run([*self.command, 'make', '--directory=' + self.relative,
                                 '--file', 'Alternate.mk'], cwd=ROOT,
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_word_errors_and_expansion_limit(self):
        for value in ['0', '-1', 'bad']:
            self.makefile(f'X := $(word {value},one two)\nall:\n')
            self.assertIn('positive integer', self.run_make(success=False).stderr)
        self.makefile('X = ' + 'a' * 1100 + '\nY := $(X)$(X)\nall:\n')
        self.assertIn('exceeds limit', self.run_make(success=False).stderr)
        self.makefile('all:\n\techo $(shell echo forbidden)\n')
        self.assertIn('unsupported make function', self.run_make(success=False).stderr)
        self.makefile('all: bad good\nbad:\n\tunknown-tool\ngood:\n\techo survivor\n')
        self.assertIn('survivor', self.run_make('--keep-going', success=False).stdout)

    def test_lazy_if_foreach_objects_findstring_join(self):
        (self.directory / 'two.c').write_text(
            '#if VALUE != 7\n#error lazy-if flags missing\n#endif\nint two(void){return VALUE;}\n')
        self.makefile('MODE = debug\nNAMES = one two\n'
                      'OBJECTS := $(foreach item,$(NAMES),$(item).o)\n'
                      'CFLAGS := $(if $(findstring debug,$(MODE)),-DVALUE=7,$(error unselected))\n'
                      'EMPTY := $(if ,$(error unselected),safe)\n'
                      'all: $(OBJECTS)\n'
                      '\t@echo empty=$(EMPTY) joined=$(join a b c,.o .d)\n'
                      '\t@echo extra=$(join a,.o .d)\n'
                      '%.o: %.c\n\t' + self.recipe('arm-none-eabi-gcc') + '\n')
        result = self.run_make()
        self.assertEqual(result.stdout.count(' -c '), 2)
        self.assertIn('empty=safe joined=a.o b.d c', result.stdout)
        self.assertIn('extra=a.o .d', result.stdout)
        self.assertNotIn(' -c ', self.run_make().stdout)

    def test_foreach_scope_and_lazy_errors(self):
        self.makefile('ITEM = file-value\n'
                      'LOOP := $(foreach ITEM,one two,$(ITEM):$(origin ITEM))\n'
                      'AFTER := $(ITEM):$(origin ITEM)\n'
                      'NESTED := $(foreach ITEM,a b,$(foreach ITEM,c d,$(ITEM)))\n'
                      'all:\n\t@echo loop=$(LOOP) after=$(AFTER) nested=$(NESTED)\n')
        result = self.run_make('ITEM=command-value')
        self.assertIn('loop=one:automatic two:automatic after=command-value:command line nested=c d c d', result.stdout)
        self.makefile('X := $(if yes,$(error selected),safe)\nall:\n')
        self.assertIn('selected', self.run_make(success=False).stderr)
        self.makefile('X := $(if ,safe,$(error selected-else))\nall:\n')
        self.assertIn('selected-else', self.run_make(success=False).stderr)
        self.makefile('X := $(foreach i,' + ' '.join(['x'] * 129) + ',$(i))\nall:\n')
        self.assertIn('too many function words', self.run_make(success=False).stderr)
        self.makefile('TEXT = ' + 'x' * 1100 + '\nX := $(foreach i,a b,$(TEXT))\nall:\n')
        self.assertIn('exceeds limit', self.run_make(success=False).stderr)

    def test_override_include_compiler_and_flavor(self):
        (self.directory / 'override.mk').write_text(
            'override CC = riscv32-unknown-elf-gcc\n'
            'override CFLAGS := -DVALUE=7\noverride CFLAGS += -O1\n'
            'override KEEP ?= replaced\n')
        (self.directory / 'two.c').write_text(
            '#if VALUE != 7\n#error override flags failed\n#endif\nint two(void){return VALUE;}\n')
        self.makefile('include override.mk\nCC = missing\nCFLAGS += -DVALUE=0\n'
                      'all: two.o\n\t@echo origins=$(origin CC)/$(origin CFLAGS)/$(origin KEEP)\n'
                      '\t@echo flavors=$(flavor CC)/$(flavor CFLAGS)/$(flavor MISSING)\n'
                      'two.o: two.c\n\t' + self.recipe() + '\n')
        result = self.run_make('CC=arm-none-eabi-gcc', 'CFLAGS=-DVALUE=0', 'KEEP=command')
        self.assertIn('origins=override/override/command line', result.stdout)
        self.assertIn('flavors=recursive/simple/undefined', result.stdout)
        self.assertEqual(int.from_bytes((self.directory / 'two.o').read_bytes()[18:20], 'little'), 243)

    def test_undefine_and_raw_value(self):
        self.makefile('FILE = file\nundefine FILE\nundefine CLI\n'
                      'override OV = protected\nundefine OV\n'
                      'KEEP := $(CLI):$(origin CLI):$(OV):$(origin OV)\n'
                      'override undefine CLI\noverride undefine OV\n'
                      'RAW = $(error must-not-run)\nCOPY := $(value RAW)\n'
                      'all:\n\t@echo before=$(KEEP) after=$(origin CLI)/$(origin OV)/$(origin FILE)\n'
                      '\t@echo raw=$(value RAW) copied=$(value COPY)\n')
        result = self.run_make('CLI=command')
        self.assertIn('before=command:command line:protected:override after=undefined/undefined/undefined', result.stdout)
        self.assertIn('raw=$(error must-not-run) copied=$(error must-not-run)', result.stdout)
        for content in ['override\n', 'override bad\n', 'undefine\n', 'undefine A B\n',
                        'override undefine A B\n']:
            self.makefile(content + 'all:\n')
            self.run_make(success=False)

    def test_target_variables_compilers_and_inheritance(self):
        self.makefile('all: one.o parent\n'
                      'one.o: CC = arm-none-eabi-gcc\n'
                      'parent: CC = riscv32-unknown-elf-gcc\n'
                      'parent: CFLAGS = -DLOCAL_VALUE=7\n'
                      'parent: two.o\n'
                      '%.o: %.c\n\t' + self.recipe() + '\n')
        (self.directory / 'two.c').write_text(
            '#if LOCAL_VALUE != 7\n#error inherited flag missing\n#endif\nint two(void){return LOCAL_VALUE;}\n')
        self.run_make()
        self.assertEqual(int.from_bytes((self.directory / 'one.o').read_bytes()[18:20], 'little'), 40)
        self.assertEqual(int.from_bytes((self.directory / 'two.o').read_bytes()[18:20], 'little'), 243)
        self.assertNotIn(' -c ', self.run_make().stdout)

    def test_target_variable_precedence_flavors_and_restore(self):
        self.makefile('GLOBAL = outside\n'
                      'not-default: UNUSED = yes\n'
                      'all: one.o two.o after\n'
                      'one.o: CC = missing\n'
                      'one.o: CFLAGS ?= -DFLAG=1\n'
                      'two.o: override CC = riscv32-unknown-elf-gcc\n'
                      'two.o: FLAGS := immediate\n'
                      'two.o: FLAGS += tail\n'
                      'two.o: GLOBAL ?= ignored\n'
                      'two.o: two.c\n\t' + self.recipe() + '\n'
                      '\t@echo local=$(FLAGS):$(flavor FLAGS):$(GLOBAL):$(origin CC)\n'
                      'one.o: one.c\n\t' + self.recipe() + '\n'
                      'after:\n\t@echo restored=$(origin FLAGS):$(CC):$(GLOBAL)\n')
        result = self.run_make('CC=arm-none-eabi-gcc')
        self.assertIn('local=immediate tail:simple:outside:override', result.stdout)
        self.assertIn('restored=undefined:arm-none-eabi-gcc:outside', result.stdout)
        self.assertEqual(int.from_bytes((self.directory / 'one.o').read_bytes()[18:20], 'little'), 40)
        self.assertEqual(int.from_bytes((self.directory / 'two.o').read_bytes()[18:20], 'little'), 243)
        self.makefile('%%.o: CC = arm-none-eabi-gcc\nall:\n')
        self.assertIn('multiple percent', self.run_make(success=False).stderr)

    def test_pattern_variables_specificity_and_compilers(self):
        for directory in ('arm', 'rv'):
            (self.directory / directory).mkdir()
            (self.directory / directory / 'unit.c').write_text('int unit(void){return 7;}\n')
        self.makefile('all: arm/unit.o rv/unit.o after\n'
                      'all: CC = missing-parent\n'
                      '%.o: CC = arm-none-eabi-gcc\n'
                      '%unit.o: ORDER += medium\n'
                      '%.o: ORDER += broad\n'
                      'rv/%.o: CC = riscv32-unknown-elf-gcc\n'
                      'rv/%.o: ORDER += directory\n'
                      'arm/unit.o: ORDER += explicit\n'
                      '%.o: %.c\n\t' + self.recipe() + '\n'
                      '\t@echo order=$@/$(ORDER)\n'
                      'after:\n\t@echo restored=$(origin ORDER)\n')
        result = self.run_make()
        self.assertIn('order=arm/unit.o/broad medium explicit', result.stdout)
        self.assertIn('order=rv/unit.o/broad directory medium', result.stdout)
        self.assertIn('restored=undefined', result.stdout)
        self.assertEqual(int.from_bytes((self.directory / 'arm/unit.o').read_bytes()[18:20], 'little'), 40)
        self.assertEqual(int.from_bytes((self.directory / 'rv/unit.o').read_bytes()[18:20], 'little'), 243)
        self.assertNotIn(' -c ', self.run_make().stdout)

    def test_pattern_variables_command_override_and_explicit_priority(self):
        self.makefile('all: one.o two.o\n'
                      '%.o: CC = missing-pattern\n'
                      't%.o: override CC = riscv32-unknown-elf-gcc\n'
                      'two.o: override CC = arm-none-eabi-gcc\n'
                      '%.o: %.c\n\t' + self.recipe() + '\n')
        self.run_make('CC=arm-none-eabi-gcc')
        for name in ('one.o', 'two.o'):
            self.assertEqual(int.from_bytes((self.directory / name).read_bytes()[18:20], 'little'), 40)

    def test_errors_cycles_and_shell_rejection(self):
        for content, expected in [
            ('all: missing\n', 'no rule'),
            ('all: next\nnext: all\n', 'cycle'),
            ('X = $(X)\nall:\n\techo $(X)\n', 'recursive variable'),
            ('all:\n\techo ok && echo bad\n', 'unsupported shell'),
            ('all:\n\tunknown-command argument\n', ''),
            ('all:\n\tarm-none-eabi-gcc --package-root ../packages -unsupported\n', ''),
        ]:
            with self.subTest(content=content):
                self.makefile(content)
                result = self.run_make(success=False)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_keep_going_and_ignore_failure(self):
        self.makefile('all: bad good\nbad:\n\tunknown-tool\ngood:\n\techo survived\n')
        result = self.run_make('-k', success=False)
        self.assertIn('survived', result.stdout)
        self.makefile('all:\n\t-unknown-tool\n\techo survived\n')
        self.assertIn('survived', self.run_make().stdout)


if __name__ == '__main__':
    unittest.main()
