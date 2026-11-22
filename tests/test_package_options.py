"""Package-defined command aliases: no compiler source changes per tool."""
import shutil
import tempfile
import unittest
from pathlib import Path
import test_driver
ROOT = test_driver.ROOT


class PackageOptionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_driver.DriverTests.setUpClass.__func__(cls)
        cls.package_root = cls.dir / 'packages'
        cls.package_root.mkdir()
        cls.package = cls.package_root / 'copied-compiler'
        shutil.copytree(ROOT / 'packages/arm', cls.package)
        text = (cls.package / 'package.ini').read_text(encoding='utf-8')
        lines = []
        for line in text.splitlines():
            if line.startswith('version='): line = 'version=3'
            elif line.startswith('id='): line = 'id=copied-compiler'
            elif line.startswith('tools='): line = 'tools=custom-c99'
            elif line.startswith(('dialect=', 'options=')): continue
            lines.append(line)
        lines += ['dialect=gcc', 'options=options.ini']
        (cls.package / 'package.ini').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        cls.rules = ('[compile]\nmatch=--compile\nmode=flag\nemit=-c\n'
                     '[chip]\nmatch=--chip\nmode=value\nemit=-mcpu\n'
                     '[joined-chip]\nmatch=--chip=\nmode=joined\nemit=-mcpu=\n'
                     '[optimize]\nmatch=--fast\nmode=flag\nemit=-O2 -fopt-inline\n')

    tearDownClass = classmethod(test_driver.DriverTests.tearDownClass.__func__)
    path = test_driver.DriverTests.path
    run_xhcc = test_driver.DriverTests.run_xhcc

    def setUp(self):
        (self.package / 'options.ini').write_text(self.rules, encoding='utf-8')

    def invoke(self, *args, success=True):
        return self.run_xhcc('custom-c99', '--package-root', self.path('packages'), *args, success=success)

    def test_copied_package_alias_and_rules(self):
        for chip in (('--chip', 'cortex-m4'), ('--chip=cortex-m4',)):
            self.invoke('--compile', '--fast', *chip, '-o', self.path('custom.o'), self.path('one.c'))
            self.assertEqual((self.dir / 'custom.o').read_bytes()[:4], b'\x7fELF')
        self.assertEqual(self.invoke('-dumpmachine').stdout.strip(), 'arm-none-eabi')

    def test_option_values_are_not_control_options(self):
        # -D's value resembles help: it must be validated as a macro, not exit 0.
        self.invoke('--compile', '-D', '--help', self.path('one.c'), success=False)
        # Canonical option values must not be rewritten by package aliases.
        self.invoke('--compile', '-D', '--compile', self.path('one.c'), success=False)
        self.invoke('--compile', '--chip', '--help', self.path('one.c'), success=False)
        result = self.invoke('--compile', '--chip', '--package-root', self.path('one.c'), success=False)
        self.assertIn('CPU', result.stderr)
        self.assertNotIn('cannot open package root', result.stderr)
        self.invoke('--compile', '--package-root', 'other', self.path('one.c'), success=False)
        self.invoke('--compile', '-o', '--package-root', self.path('one.c'))
        output = ROOT / '--package-root'
        try:
            self.assertEqual(output.read_bytes()[:4], b'\x7fELF')
        finally:
            output.unlink(missing_ok=True)
        # Help paired with compilation must still validate the package rules.
        (self.package / 'options.ini').write_text('[bad]\nmode=magic\n', encoding='utf-8')
        self.invoke('--help', '--compile', self.path('one.c'), success=False)

    def test_rules_cannot_change_root_or_tool(self):
        for emit in ('--package-root other', '--package-root=other', '--target=arm',
                     '-target arm', '-m32', '-m64'):
            with self.subTest(emit=emit):
                (self.package / 'options.ini').write_text(
                    '[escape]\nmatch=--compile\nmode=flag\nemit=' + emit + '\n', encoding='utf-8')
                self.invoke('--compile', self.path('one.c'), success=False)

    def test_value_and_unknown_options_fail(self):
        self.invoke('--compile', '--chip', success=False)
        self.invoke('--compile', '--chip=', self.path('one.c'), success=False)
        self.invoke('--compile', '--unknown', self.path('one.c'), success=False)

    def test_response_is_expanded_before_mapping(self):
        (self.dir / 'custom.rsp').write_text('--compile --chip=cortex-m3 --fast -o ' +
                                            self.path('response.o') + ' ' + self.path('one.c'))
        self.invoke('@' + self.path('custom.rsp'))
        self.assertEqual((self.dir / 'response.o').read_bytes()[:4], b'\x7fELF')

    def test_invalid_schema_and_response_emission_rejected(self):
        invalid = ('[x]\nmatch=--compile\nmode=magic\nemit=-c\n',
                   '[x]\nmatch=--compile\nmode=flag\nemit=@evil.rsp\n',
                   '[x]\nmatch=--chip=\nmode=joined\nemit=-mcpu\n',
                   '[x]\nmatch=--compile\nmode=flag\nemit=-c\nunknown=1\n')
        for rules in invalid:
            with self.subTest(rules=rules):
                (self.package / 'options.ini').write_text(rules, encoding='utf-8')
                self.invoke('--compile', self.path('one.c'), success=False)


if __name__ == '__main__':
    unittest.main()
