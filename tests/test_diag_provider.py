"""Bounded source-provider regressions; run after the application build."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command


class DiagnosticProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='diag-provider-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)
        cls.relative = cls.directory.relative_to(ROOT).as_posix()

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def diagnose(self, name, contents, *options):
        (self.directory / name).write_bytes(contents)
        result = subprocess.run(
            [*self.command, '-fsyntax-only', *options, f'{self.relative}/{name}'],
            cwd=ROOT, capture_output=True, timeout=120)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return (result.stdout + result.stderr).decode('utf-8')

    def test_large_crlf_source_tail_error_echo(self):
        prefix = b'/* bounded source padding */\r\n' * 40000
        self.assertGreater(len(prefix), 1024 * 1024)
        log = self.diagnose('tail.c', prefix + b'int tail(void) { return TAIL_UNDEFINED; }\r\n')
        self.assertIn('TAIL_UNDEFINED', log)
        self.assertIn('  int tail(void) { return TAIL_UNDEFINED; }', log)
        self.assertIn('^', log)

    def test_long_unicode_line_excerpt_reaches_error(self):
        line = 'int wide(void) { const char *s="' + '界' * 900 + '"; return WIDE_UNDEFINED; }\r\n'
        log = self.diagnose('wide.c', line.encode('utf-8'), '--diag_style=modern')
        self.assertIn('WIDE_UNDEFINED', log)
        echoes = [line for line in log.splitlines() if ' | ' in line]
        self.assertTrue(any('WIDE_UNDEFINED' in line for line in echoes), log)
        self.assertTrue(any('...' in line for line in echoes), log)
        self.assertTrue(any('^' in line for line in echoes), log)
        self.assertTrue(all(len(line.encode('utf-8')) < 512 for line in echoes), log)

    def test_preprocessor_warning_can_echo_without_memory_source(self):
        log = self.diagnose('warning.c', b'#warning PROVIDER_WARNING\r\nint f(void) { return 0; }\r\n', '-Werror')
        self.assertIn('  #warning PROVIDER_WARNING', log)

    def test_ide_still_has_no_echo(self):
        log = self.diagnose('ide.c', b'int f(void) { return IDE_UNDEFINED; }\n', '--diag_style=ide')
        self.assertNotIn('  int f(void)', log)
        self.assertNotIn('^', log)


if __name__ == '__main__':
    unittest.main()
