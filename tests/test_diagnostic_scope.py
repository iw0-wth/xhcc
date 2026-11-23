"""Real frontend regressions for command-line versus TU pragma policy.

Run against the built application; this test never builds the compiler.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command


class DiagnosticScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='diag-scope-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)
        cls.relative = cls.directory.relative_to(ROOT).as_posix()
        (cls.directory / 'first.c').write_text(
            '#pragma diag_suppress 1215\n#warning FIRST_SCOPE\n'
            'int first_scope(void) { return 1; }\n')
        (cls.directory / 'second.c').write_text(
            '#warning SECOND_SCOPE\nint second_scope(void) { return 2; }\n')
        (cls.directory / 'default.c').write_text(
            '#pragma diag_default 1215\n#warning DEFAULT_SCOPE\n'
            'int default_scope(void) { return 3; }\n')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def path(self, name):
        return f'{self.relative}/{name}'

    def run_compiler(self, *args, success):
        result = subprocess.run([*self.command, *args], cwd=ROOT,
                                capture_output=True, text=True, timeout=60)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode == 0, success, log)
        return log

    def test_modern_pragma_suppression_stops_at_tu_boundary(self):
        log = self.run_compiler('-fsyntax-only', '-Werror', self.path('first.c'),
                                self.path('second.c'), success=False)
        self.assertIn('SECOND_SCOPE', log)
        self.assertIn('1215', log)
        self.assertNotIn('FIRST_SCOPE', log)
        self.assertIn('error', log.lower())

    def test_cli_policy_survives_all_tus_and_pragma_default(self):
        for policy in ('-Wno-cpp', '--diag_suppress=1215'):
            with self.subTest(policy=policy):
                log = self.run_compiler('-fsyntax-only', '-Werror', policy,
                                        self.path('first.c'), self.path('second.c'),
                                        success=True)
                self.assertNotIn('FIRST_SCOPE', log)
                self.assertNotIn('SECOND_SCOPE', log)
                self.assertNotIn('warning', log.lower())
                # A TU may locally re-enable the CLI-suppressed diagnostic;
                # the following TU must restore the command-line suppression.
                log = self.run_compiler('-fsyntax-only', policy,
                                        self.path('default.c'), self.path('second.c'),
                                        success=True)
                self.assertIn('DEFAULT_SCOPE', log)
                self.assertNotIn('SECOND_SCOPE', log)

    def test_legacy_ipo_uses_the_same_tu_policy_boundary(self):
        common = ('--ark-compat', '-target', 'arm', '--ipo', '-S',
                  '-o', self.path('scope.s'))
        log = self.run_compiler(*common, '--diag_error=1215',
                                self.path('first.c'), self.path('second.c'), success=False)
        self.assertIn('SECOND_SCOPE', log)
        self.assertIn('1215', log)
        self.assertNotIn('FIRST_SCOPE', log)
        self.assertIn('error', log.lower())
        log = self.run_compiler(*common, '--diag_error=1215', '--diag_suppress=1215',
                                self.path('first.c'), self.path('second.c'), success=True)
        self.assertNotIn('FIRST_SCOPE', log)
        self.assertNotIn('SECOND_SCOPE', log)


if __name__ == '__main__':
    unittest.main()
