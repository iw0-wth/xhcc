"""External object target descriptors: real emitted ELF and immutable compiler variants."""
from pathlib import Path
import os,sys,subprocess,tempfile,shutil,hashlib,unittest
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
OUT=ROOT/'build/object-rule-tests'
class ObjectRules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):OUT.mkdir(parents=True,exist_ok=True)
 def compile(self,root,source,output):
  return subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(source.relative_to(ROOT)),'-o',str(output.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
 def test_default_descriptors_and_external_relocation_variant(self):
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  source=OUT/'external.c';source.write_text('extern int external(int); int entry(void){return external(7);}\n')
  for package,machine,cls,key,newtype in [('arm','EM_ARM',32,10,30),('riscv32','EM_RISCV',32,23,26),('x86','EM_386',32,2,1),('x86_64','EM_X86_64',64,2,1)]:
   with self.subTest(target=package),tempfile.TemporaryDirectory(prefix='xh-object-') as tmp:
    root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages'/package,pkg)
    text=(pkg/'package.ini').read_text();lines=[('id=variant' if s.startswith('id=') else 'tools=variant-gcc' if s.startswith('tools=') else s) for s in text.splitlines()];(pkg/'package.ini').write_text('\n'.join(lines)+'\n')
    output=OUT/f'{package}.o';p=self.compile(root,source,output);self.assertEqual(p.returncode,0,p.stderr)
    with output.open('rb') as f:
     elf=ELFFile(f);self.assertEqual(elf['e_machine'],machine);self.assertEqual(elf.elfclass,cls);self.assertTrue(elf.little_endian)
     types=[r['r_info_type'] for sec in elf.iter_sections() if sec['sh_type'] in ['SHT_REL','SHT_RELA'] for r in sec.iter_relocations()];self.assertIn(key,types)
    rules=(pkg/'object.rules').read_text();(pkg/'object.rules').write_text(rules.replace(f'relmap {key} {key}\n',f'relmap {key} {newtype}\n'))
    p=self.compile(root,source,output);self.assertEqual(p.returncode,0,p.stderr)
    with output.open('rb') as f:
     elf=ELFFile(f);types=[r['r_info_type'] for sec in elf.iter_sections() if sec['sh_type'] in ['SHT_REL','SHT_RELA'] for r in sec.iter_relocations()];self.assertIn(newtype,types);self.assertNotIn(key,types)
    # Class mismatch must never produce a misleading ELF header.
    (pkg/'object.rules').write_text(rules.replace(f'class {cls}',f'class {64 if cls==32 else 32}'))
    p=self.compile(root,source,output);self.assertNotEqual(p.returncode,0);self.assertIn('data model' if package!='arm' else 'ARM target',p.stderr)
    (pkg/'object.rules').write_text(rules+f'relmap {key} 99\n')
    p=self.compile(root,source,output);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_arm_profile_selection_tracks_isa_capabilities(self):
  source=OUT/'profiles.c';source.write_text('int entry(void){return 7;}\n')
  for cpu,arch,thumb in [('cortex-m0',12,1),('cortex-m3',10,2)]:
   with self.subTest(cpu=cpu):
    output=OUT/(cpu+'_attrs.o');p=subprocess.run([sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-mcpu='+cpu,'-mthumb','-c',str(source.relative_to(ROOT)),'-o',str(output.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
    with output.open('rb') as f:
     attrs=ELFFile(f).get_section_by_name('.ARM.attributes');self.assertIsNotNone(attrs);values={a.tag:a.value for sub in attrs.iter_subsections() for section in sub.iter_subsubsections() for a in section.iter_attributes()};self.assertEqual(values['TAG_CPU_ARCH'],arch);self.assertEqual(values['TAG_THUMB_ISA_USE'],thumb)
  with tempfile.TemporaryDirectory(prefix='xh-t1-no-profile-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/arm',pkg);text=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if s.startswith('id=') else 'tools=variant-gcc' if s.startswith('tools=') else s for s in text.splitlines())+'\n');rules=(pkg/'object.rules').read_text();(pkg/'object.rules').write_text('\n'.join(s for s in rules.splitlines() if not s.startswith('attributes_profile thumb1 '))+'\n');output=OUT/'no_t1_profile.o';output.unlink(missing_ok=True);p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-mcpu=cortex-m0','-mthumb','-c',str(source.relative_to(ROOT)),'-o',str(output.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0,p.stdout+p.stderr);self.assertFalse(output.exists());self.assertIn('profile',p.stdout+p.stderr)
 def test_flags_are_external(self):
  with tempfile.TemporaryDirectory(prefix='xh-object-flags-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/x86',pkg);text=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if s.startswith('id=') else 'tools=variant-gcc' if s.startswith('tools=') else s for s in text.splitlines())+'\n')
   rules=(pkg/'object.rules').read_text();(pkg/'object.rules').write_text(rules.replace('flags 0\n','flags 0x1234\n'));source=OUT/'flags.c';source.write_text('int flags_entry(void){return 1;}\n');output=OUT/'flags.o';p=self.compile(root,source,output);self.assertEqual(p.returncode,0,p.stderr)
   with output.open('rb') as f:self.assertEqual(ELFFile(f)['e_flags'],0x1234)
 def test_embedded_nul_and_duplicate_attributes_are_rejected(self):
  with tempfile.TemporaryDirectory(prefix='xh-object-invalid-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
   manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if s.startswith('id=') else 'tools=variant-gcc' if s.startswith('tools=') else s for s in manifest.splitlines())+'\n')
   source=OUT/'invalid.c';source.write_text('int entry(void){return 3;}\n');output=OUT/'invalid.o';rules=(pkg/'object.rules').read_bytes()
   for contents in [rules+b'\x00ignored trailing data',rules+b'attributes '+b'41'*128+b'\n']:
    (pkg/'object.rules').write_bytes(contents);output.unlink(missing_ok=True)
    result=self.compile(root,source,output);self.assertNotEqual(result.returncode,0,result.stderr);self.assertFalse(output.exists())
if __name__=='__main__':unittest.main()
