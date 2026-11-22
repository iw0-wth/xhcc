"""Predefined/CPU/package macros precede ordered command -D/-U actions.
Run after the lead's build; this suite never builds or edits installed packages.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_driver import ROOT, command


class PredefineOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='predefine-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)
        cls.relative = cls.directory.relative_to(ROOT).as_posix()

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def preprocess(self, source, *flags):
        src = self.directory / 'case.c'
        src.write_text(source, encoding='utf-8')
        prefix = [] if any(Path(arg).name == 'xhcc.py' for arg in self.command) else ['arm-none-eabi-gcc']
        result = subprocess.run([*self.command, *prefix, '-E', *flags,
                                 src.relative_to(ROOT).as_posix()], cwd=ROOT,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_define_overrides_package_cpu_and_width_presets(self):
        self.preprocess('''
#if __arm__ != 17 || __thumb__ != 19 || __SIZEOF_POINTER__ != 23
#error command definitions were replaced by target presets
#endif
int order_ok;
''', '-D__arm__=17', '-D__thumb__=19', '-D__SIZEOF_POINTER__=23')

    def test_undef_removes_package_cpu_and_width_presets(self):
        self.preprocess('''
#if defined(__arm__) || defined(__thumb__) || defined(__SIZEOF_POINTER__)
#error a removed preset was injected again
#endif
int undef_ok;
''', '-U__arm__', '-U__thumb__', '-U__SIZEOF_POINTER__')

    def test_last_ordered_action_wins(self):
        self.preprocess('''
#if __arm__ != 31 || defined(__thumb__)
#error ordered D U actions were not respected
#endif
int last_ok;
''', '-U__arm__', '-D__arm__=31', '-D__thumb__=5', '-U__thumb__')

    def test_standard_macros_remain_but_user_action_is_last(self):
        text = self.preprocess('''
#if __STDC_VERSION__ != 7 || __STDC_HOSTED__ != 9
#error language mode overrode explicit command definitions
#endif
int standard_override_ok;
int standard_value = __STDC__;
''', '-std=c11', '-D__STDC_VERSION__=7', '-D__STDC_HOSTED__=9', '-D__STDC__=53')
        self.assertIn('standard_value = 53', text)

    def test_undef_preserves_standard_and_removes_compiler_presets(self):
        self.preprocess('''
#if !defined(__STDC__) || !defined(__STDC_VERSION__) || !defined(__STDC_HOSTED__)
#error undef removed standard macros
#endif
#if defined(__GNUC__) || defined(__arm__) || defined(__thumb__) || defined(__SIZEOF_POINTER__)
#error undef retained compiler or package presets
#endif
#ifndef __XHCC_TARGET_ID__
#error canonical internal target identity was lost
#endif
int undef_mode_ok;
''', '-undef')

    def test_undef_keeps_explicit_command_definition(self):
        self.preprocess('''
#if __arm__ != 37 || CUSTOM_BOARD != 41
#error undef removed command macros
#endif
int explicit_ok;
''', '-D__arm__=37', '-DCUSTOM_BOARD=41', '-undef')

    def test_forced_include_sees_final_macro_actions(self):
        header = self.directory / 'forced.h'
        header.write_text('#if __arm__ != 43\n#error forced include saw stale target preset\n#endif\n')
        self.preprocess('int forced_ok;\n', '-D__arm__=43', '-include',
                        header.relative_to(ROOT).as_posix())

    def test_translation_unit_macro_state_does_not_leak(self):
        self.preprocess('#if USER_TU != 47\n#error missing user macro\n#endif\n', '-DUSER_TU=47')
        self.preprocess('#ifdef USER_TU\n#error previous unit leaked a user macro\n#endif\n')


if __name__ == '__main__':
    unittest.main()
