"""One shared compiler process; external packages contain configuration only."""
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('xhcc_host', ROOT / 'tools/xhcc.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class HostTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='xhcc-host-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.compiler = self.root / 'shared.wasm'
        self.compiler.write_bytes(b'host must not read module')

    def package(self, name='selected', aliases='chosen', version=3, **overrides):
        directory = self.root / name
        directory.mkdir()
        data = dict(version=str(version), id=name, tools=aliases, target='arm', cpu='cortex-m3',
                    abi='aapcs-soft', include='include')
        data.update(overrides)
        (directory / 'package.ini').write_text(''.join(f'{k}={v}\n' for k, v in data.items()), encoding='utf-8')
        return directory

    def invoke(self, *arguments):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            result = host.main(['--package-root', str(self.root), '--compiler', str(self.compiler), *arguments])
        return result, out.getvalue(), err.getvalue()

    def test_one_shared_process_raw_argv_and_environment(self):
        self.package(dialect='armcc', options='options.ini')
        raw = ['--compiler', 'package-owned', '--package-root', 'opaque', '--json', '--', '源 file.c']
        with patch.object(host.subprocess, 'run') as run:
            run.return_value.returncode = 23
            result, output, trace = self.invoke('--json', '--trace-load', 'chosen', *raw)
        self.assertEqual(result, 23)
        run.assert_called_once()
        plan = json.loads(output)
        self.assertEqual(plan['argv'], ['chosen', *raw])
        self.assertEqual(plan['command'][-len(raw)-2:], [str(self.compiler), 'chosen', *raw])
        self.assertEqual(plan['environment'], {'XHCC_PACKAGE_ROOT': '/xh-packages', 'XHCC_PACKAGE_DIR': '/xh-packages/selected'})
        self.assertIn('--dir=' + str(self.root) + '::/xh-packages', plan['command'])
        self.assertEqual(json.loads(trace), {'event': 'execute', 'module': str(self.compiler), 'package': 'selected', 'worker_count': 1})

    def test_copied_configuration_package_needs_no_wasm(self):
        source = self.package(engine='rv32-rule-v1', encoding='encoding.rules', selection='selection.rules', templates='arm.rules', object='object.rules', relocations='relocation.rules', cpus='cpus.rules', target_description='target.rules')
        (source / 'encoding.rules').write_text('external rule fixture\n')
        (source / 'selection.rules').write_text('external selector fixture\n')
        (source / 'arm.rules').write_text('external template fixture\n')
        (source / 'object.rules').write_text('external object descriptor fixture\n')
        (source / 'relocation.rules').write_text('external relocation metadata fixture\n')
        (source / 'cpus.rules').write_text('external CPU descriptor fixture\n')
        (source / 'target.rules').write_text('external target descriptor fixture\n')
        copy = self.root / 'copy'
        shutil.copytree(source, copy)
        path = copy / 'package.ini'
        path.write_text(path.read_text().replace('id=selected', 'id=copy').replace('tools=chosen', 'tools=copied'))
        with patch.object(host.subprocess, 'run') as run:
            result, output, trace = self.invoke('--dry-run', '--json', '--trace-load', 'copied')
        run.assert_not_called()
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output)['package'], 'copy')
        self.assertEqual(json.loads(output)['module'], str(self.compiler))
        self.assertEqual(json.loads(trace)['worker_count'], 0)
        self.assertEqual(list(copy.glob('*.wasm')), [])
        self.assertEqual((copy / 'encoding.rules').read_text(), 'external rule fixture\n')
        self.assertEqual((copy / 'selection.rules').read_text(), 'external selector fixture\n')
        self.assertEqual((copy / 'arm.rules').read_text(), 'external template fixture\n')
        self.assertEqual((copy / 'object.rules').read_text(), 'external object descriptor fixture\n')
        self.assertEqual((copy / 'relocation.rules').read_text(), 'external relocation metadata fixture\n')
        self.assertEqual((copy / 'cpus.rules').read_text(), 'external CPU descriptor fixture\n')
        self.assertEqual((copy / 'target.rules').read_text(), 'external target descriptor fixture\n')
        self.assertEqual(host.manifest(path)['cpus'], 'cpus.rules')
        self.assertEqual(host.manifest(path)['target_description'], 'target.rules')
        self.assertEqual(host.manifest(path)['relocations'], 'relocation.rules')
        self.assertEqual(host.manifest(path)['object'], 'object.rules')
        self.assertEqual(host.manifest(path)['templates'], 'arm.rules')
        self.assertEqual(host.manifest(path)['selection'], 'selection.rules')
        self.assertEqual(host.manifest(path)['engine'], 'rv32-rule-v1')

    def test_v1_module_is_ignored_even_if_missing(self):
        self.package(version=1, module='../missing-backend.wasm')
        original_open = Path.open
        def guarded_open(path, *args, **kwargs):
            if path.suffix == '.wasm':
                raise AssertionError('host opened Wasm: ' + str(path))
            return original_open(path, *args, **kwargs)
        with patch.object(Path, 'open', guarded_open):
            _, output, _ = self.invoke('--dry-run', '--json', 'chosen')
        self.assertEqual(json.loads(output)['module'], str(self.compiler))

    def test_v2_staged_packages_require_upgrade(self):
        self.package(version=2, module='backend.wasm', adapter='adapter.wasm')
        with self.assertRaisesRegex(ValueError, 'upgrade to version 3'):
            host.discover(self.root)

    def test_invalid_alias_schema_and_paths(self):
        source = self.package()
        self.package('duplicate', 'chosen')
        with self.assertRaisesRegex(ValueError, 'alias'):
            host.discover(self.root)
        for key, value in [('include', '../outside'), ('options', '/absolute'), ('encoding', '../outside.rules'), ('encoding', 'C:/escape.rules'), ('selection', '../outside.rules'), ('selection', '/absolute.rules'), ('templates', '../outside.rules'), ('templates', 'C:/escape.rules'), ('object', '../outside.rules'), ('object', '/absolute.rules'), ('relocations', '../outside.rules'), ('relocations', '/absolute.rules'), ('cpus', '../outside.rules'), ('cpus', '/absolute.rules'), ('target_description', '../outside.rules'), ('target_description', 'C:/escape.rules'), ('include', 'sub\\path'), ('dialect', 'unknown'), ('module', 'backend.wasm')]:
            name = 'invalid' + str(len(list(self.root.iterdir())))
            directory = self.package(name, name, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                host.manifest(directory / 'package.ini')

    def test_requires_syntax_only_and_legacy_default(self):
        valid = ['package.options.v1,encoding.fields.v1,selection.scalar32.v1',
                 'selection.scalar32.v2,object.elf32,object.elf64,codegen.templates.v1,object.descriptor.v1,relocation.vm.v1,cpu.descriptor.v1,target.descriptor.v1', 'future.capability.v99']
        for index, requires in enumerate(valid):
            directory = self.package('valid' + str(index), 'valid' + str(index), requires=requires)
            self.assertEqual(host.manifest(directory / 'package.ini')['requires'], requires)
        legacy = self.package()
        self.assertNotIn('requires', host.manifest(legacy / 'package.ini'))
        with patch.object(host.subprocess, 'run') as run:
            run.return_value.returncode = 42
            result, _, _ = self.invoke('valid2')
        run.assert_called_once()
        self.assertEqual(result, 42)  # Unknown capabilities are the compiler's decision.
        invalid = ['', ',object.elf32', 'object.elf32,', 'object.elf32,,object.elf64',
                   'object.elf32,object.elf32', 'object.elf32, object.elf64',
                   'Object.elf32', 'object/elf32', 'object..elf32', 'object.elf32;', '能力.v1']
        for index, requires in enumerate(invalid):
            directory = self.package('invalidcap' + str(index), 'invalidcap' + str(index), requires=requires)
            with self.subTest(requires=requires), self.assertRaisesRegex(ValueError, 'requires'):
                host.manifest(directory / 'package.ini')

    def test_list_does_not_require_compiler(self):
        self.package()
        self.compiler.unlink()
        result, output, _ = self.invoke('--list', '--json')
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output), {'chosen': 'selected'})

    def test_missing_shared_compiler_rejected(self):
        self.package()
        self.compiler.unlink()
        with patch.object(host.subprocess, 'run') as run, self.assertRaises(SystemExit):
            self.invoke('--dry-run', 'chosen')
        run.assert_not_called()

    def test_copied_built_configuration_compiles(self):
        manifest_path = ROOT / 'packages/arm/package.ini'
        compiler = ROOT / 'build/wasm/xhcc.wasm'
        if 'version=3' not in manifest_path.read_text() or not compiler.is_file():
            self.skipTest('v3 shared compiler is not built')
        runtime = shutil.which('wasmtime') or Path(r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
        if not Path(runtime).is_file():
            self.skipTest('Wasmtime CLI unavailable')
        with tempfile.TemporaryDirectory(prefix='host-', dir=ROOT / 'build') as temporary:
            root = Path(temporary)
            package = root / 'second-arm'
            shutil.copytree(ROOT / 'packages/arm', package)
            path = package / 'package.ini'
            path.write_text(path.read_text().replace('id=arm', 'id=second-arm').replace('arm-none-eabi-gcc,armcc', 'second-gcc'))
            self.assertEqual(list(package.glob('*.wasm')), [])
            output = root / 'copy.o'
            result = subprocess.run([sys.executable, str(ROOT / 'tools/xhcc.py'), '--package-root', str(root),
                                     '--compiler', str(compiler), '--wasmtime', str(runtime), 'second-gcc', '-c',
                                     'examples/add.c', '-o', output.relative_to(ROOT).as_posix()],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(output.read_bytes()[:4], b'\x7fELF')


if __name__ == '__main__':
    unittest.main()
