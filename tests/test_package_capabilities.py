"""Package requirements must fail before preprocessing/output when unsupported."""
import unittest,shutil,tempfile,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WASM=r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
class Capabilities(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='caps-');self.dir=Path(self.tmp.name);self.pack=self.dir/'packages/copy';shutil.copytree(ROOT/'packages/arm-cortex-m3',self.pack)
  self.base=(self.pack/'package.ini').read_text().replace('id=arm-cortex-m3','id=copy').replace('tools=arm-none-eabi-gcc,armcc','tools=copy-gcc')
  (self.dir/'a.c').write_text('int f(void){return 3;}\n')
 def tearDown(self):self.tmp.cleanup()
 def invoke(self,text,ok):
  (self.pack/'package.ini').write_text(text)
  rel=self.dir.relative_to(ROOT).as_posix();out=self.dir/'a.o';out.unlink(missing_ok=True)
  p=subprocess.run([WASM,'run','--dir=.',str(ROOT/'build/wasm/xhcc.wasm'),'copy-gcc','--package-root',rel+'/packages','-c',rel+'/a.c','-o',rel+'/a.o'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
  self.assertEqual(out.exists(),ok)
 def test_supported_and_legacy(self):
  self.invoke(self.base,True)
  self.invoke(self.base+'requires=package.options.v1,encoding.fields.v1,selection.scalar32.v2,object.elf32\n',True)
 def test_unsupported_and_malformed(self):
  for requirements in ['future.vm.v9','object.elf32,','object.elf32,object.elf32','selection..v1']:
   with self.subTest(requirements=requirements):self.invoke(self.base+'requires='+requirements+'\n',False)
 def test_target_abi_and_abandoned_schema(self):
  for text in [self.base.replace('abi=aapcs-soft','abi=sysv-lp64'),self.base.replace('target=arm','target=unimplemented'),self.base.replace('version=3','version=2')]:self.invoke(text,False)
if __name__=='__main__':unittest.main()
