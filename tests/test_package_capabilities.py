"""Package requirements must fail before preprocessing/output when unsupported."""
import unittest,shutil,tempfile,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WASM=r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
class Capabilities(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='caps-');self.dir=Path(self.tmp.name);self.pack=self.dir/'packages/copy';shutil.copytree(ROOT/'packages/arm',self.pack)
  self.base=(self.pack/'package.ini').read_text().replace('id=arm','id=copy').replace('tools=arm-none-eabi-gcc,armcc','tools=copy-gcc')
  self.base=''.join(line+'\n' for line in self.base.splitlines() if not line.startswith('requires='))
  (self.dir/'a.c').write_text('int f(void){return 3;}\n')
 def tearDown(self):self.tmp.cleanup()
 def invoke(self,text,ok,mode='-c'):
  (self.pack/'package.ini').write_text(text)
  rel=self.dir.relative_to(ROOT).as_posix();out=self.dir/'a.o';out.unlink(missing_ok=True)
  p=subprocess.run([WASM,'run','--dir=.',str(ROOT/'build/wasm/xhcc.wasm'),'copy-gcc','--package-root',rel+'/packages',mode,rel+'/a.c','-o',rel+'/a.o'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
  self.assertEqual(out.exists(),ok)
  return p
 def test_supported_and_legacy(self):
  self.invoke(self.base,True)
  self.invoke(self.base+'requires=package.options.v1,encoding.fields.v1,selection.scalar32.v2,object.elf32\n',True)
 def test_unsupported_and_malformed(self):
  for requirements in ['future.vm.v9','object.elf32,','object.elf32,object.elf32','selection..v1']:
   with self.subTest(requirements=requirements):self.invoke(self.base+'requires='+requirements+'\n',False)
 def test_shared_semantic_and_abi_requirements_are_supported(self):
  self.invoke(self.base+'requires=semantic.selector.v1,abi.decision.v1\n',True)
 def test_small_sse_struct_plan_capability_is_known(self):
  # Requirements describe the shared engine, not this ARM package's ABI.
  self.invoke(self.base+'requires=abi.plan-small-sse-struct.v1\n',True)
 def test_unknown_small_sse_struct_plan_capability_version_is_rejected(self):
  p=self.invoke(self.base+'requires=abi.plan-small-sse-struct.v999\n',False)
  self.assertIn('capability',(p.stdout+p.stderr).lower())
 def test_mixed_pair_plan_capability_is_known(self):
  # Shared-engine capability availability does not imply ARM ABI support.
  self.invoke(self.base+'requires=abi.plan-mixed-pair.v1\n',True)
 def test_unknown_mixed_pair_plan_capability_version_is_rejected(self):
  p=self.invoke(self.base+'requires=abi.plan-mixed-pair.v999\n',False)
  self.assertIn('capability',(p.stdout+p.stderr).lower())
 def test_new_semantic_or_abi_capability_versions_are_rejected(self):
  for requirement in ['semantic.selector.v999','abi.decision.v999','typeclass.v1']:
   with self.subTest(requirement=requirement):
    p=self.invoke(self.base+'requires='+requirement+'\n',False)
    self.assertIn('capability',(p.stdout+p.stderr).lower())
 def test_unknown_semantic_record_version_is_rejected(self):
  shutil.rmtree(self.pack)
  shutil.copytree(ROOT/'packages/c',self.pack)
  manifest=(self.pack/'package.ini').read_text().replace('id=c\n','id=copy\n').replace('tools=xh-c-gcc','tools=copy-gcc')
  manifest=''.join(line+'\n' for line in manifest.splitlines() if not line.startswith('requires='))
  manifest+='requires=semantic.selector.v1\n'
  path=self.pack/'c.rules';text=path.read_text()
  variants=['XHSEMANTIC 1','CSELECT 1','CSELECT1']
  changed=False
  for marker in variants:
   if marker in text:
    text=text.replace(marker,'XHSEMANTIC 999',1);changed=True;break
  self.assertTrue(changed,'C package must contain versioned semantic selector records')
  path.write_text(text)
  p=self.invoke(manifest,False,mode='-S')
  self.assertIn('semantic',(p.stdout+p.stderr).lower())
 def test_target_abi_and_abandoned_schema(self):
  for text in [self.base.replace('abi=aapcs-soft','abi=sysv-lp64'),self.base.replace('target=arm','target=unimplemented'),self.base.replace('version=3','version=2')]:self.invoke(text,False)
if __name__=='__main__':unittest.main()
