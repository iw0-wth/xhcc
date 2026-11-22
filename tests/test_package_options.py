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
        shutil.copytree(ROOT / 'packages/arm-cortex-m3', cls.package)
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
