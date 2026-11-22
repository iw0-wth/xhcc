"""External CPU descriptions: copied package, aliases, ISA rejection and schema."""
import shutil
import unittest
import test_driver
ROOT = test_driver.ROOT

class CpuRuleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_driver.DriverTests.setUpClass.__func__(cls)
        cls.package_root=cls.dir/'packages';cls.package_root.mkdir()
        cls.package=cls.package_root/'custom-arm'
        shutil.copytree(ROOT/'packages/arm',cls.package)
        manifest=(cls.package/'package.ini').read_text(encoding='utf-8')
        manifest='\n'.join('id=custom-arm' if line.startswith('id=') else
                           'tools=custom-arm-cc' if line.startswith('tools=') else line
                           for line in manifest.splitlines())+'\n'
        if 'cpus=' not in manifest: manifest+='cpus=cpus.rules\n'
        (cls.package/'package.ini').write_text(manifest,encoding='utf-8')
        cls.original=(cls.package/'cpus.rules').read_text(encoding='utf-8')
    tearDownClass=classmethod(test_driver.DriverTests.tearDownClass.__func__)
    path=test_driver.DriverTests.path
    run_xhcc=test_driver.DriverTests.run_xhcc
    def setUp(self):
        (self.package/'cpus.rules').write_text(self.original,encoding='utf-8')
    def invoke(self,*args,success=True):
        return self.run_xhcc('custom-arm-cc','--package-root',self.path('packages'),*args,success=success)
    def test_custom_cpu_name_and_alias_same_scalar_isa(self):
        text=self.original.replace('name=Cortex-M4\n','name=Vendor-Scalar-Core\n').replace('aliases=M4\n','aliases=M4,board-core\n')
        (self.package/'cpus.rules').write_text(text,encoding='utf-8')
        (self.dir/'cpu.c').write_text('#if __TARGET_ARCH_M != 7\n#error arch\n#endif\n#ifndef __TARGET_FEATURE_DSP\n#error dsp\n#endif\nint custom(void){return 4;}\n')
        for cpu in ('Vendor-Scalar-Core','BOARD-CORE'):
            self.invoke('-c','-mcpu='+cpu,'-mfloat-abi=softfp','-mfpu=fpv4-sp-d16',
                        '-o',self.path('custom.o'),self.path('cpu.c'))
            self.assertEqual((self.dir/'custom.o').read_bytes()[:4],b'\x7fELF')
    def test_thumb1_rejected_by_capability(self):
        for cpu in ('M0','M0+','M1','M23'):
            self.invoke('-c','-mcpu='+cpu,self.path('one.c'),success=False)
    def test_invalid_schema_and_duplicate_alias(self):
        variants=[self.original.replace('version=1','version=99'),
                  self.original.replace('thumb2=1','thumb2=2',1),
                  self.original.replace('aliases=M4','aliases=M3',1),
                  self.original.replace('has_fpu=1','has_fpu=1\nunknown=1',1)]
        for text in variants:
            with self.subTest(text=text[:80]):
                (self.package/'cpus.rules').write_text(text,encoding='utf-8')
                self.invoke('-c','-mcpu=M4',self.path('one.c'),success=False)
    def test_missing_descriptions_fail_without_builtin_fallback(self):
        (self.package/'cpus.rules').unlink()
        self.invoke('-c','-mcpu=M3',self.path('one.c'),success=False)

if __name__=='__main__': unittest.main()
