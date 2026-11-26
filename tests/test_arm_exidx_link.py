"""Checked ARM CANTUNWIND tables, real GNU objects, standalone link core."""
from pathlib import Path
import io, os, subprocess, struct, unittest
from test_elf_link import HARNESS, ROOT, SDK, EXE, GNU, WASMTIME, ELFFile
OUT=ROOT/'build/arm-exidx-tests'
class ArmExidx(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
  cmd=[SDK/('bin/clang'+EXE),'--target=wasm32-wasip1','-std=c99','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fsanitize-trap=all','-Iinclude','-include','include/xh_small.h',OUT/'harness.c','src/link/elf_link.c','src/link/elf_read.c','src/link/arm_elf_reloc.c','src/link/rv_elf_reloc.c','src/link/ld_script.c','src/backend/rules/relocation.c','-o',OUT/'link.wasm']
  p=subprocess.run(list(map(str,cmd)),cwd=ROOT,capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
 def obj(self,name):
  p=OUT/(name+'.s');o=p.with_suffix('.o')
  p.write_text('.syntax unified\n.thumb\n.section .text.'+name+',"ax",%progbits\n.global '+name+'\n.thumb_func\n'+name+':\n.fnstart\nbx lr\n.cantunwind\n.fnend\n.data\n.word 17\n')
  r=subprocess.run([str(GNU/('arm-none-eabi-as'+EXE)),'-mcpu=cortex-m4','-mthumb',str(p),'-o',str(o)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr);return o
 def script(self,reverse=False,metadata=True):
  p=OUT/'layout.ld';p.write_text('MEMORY { ROM (rx) : ORIGIN = 0x08000000, LENGTH = 0x10000 RAM (rw) : ORIGIN = 0x20000000, LENGTH = 0x10000 }\nSECTIONS { .text : { '+('*(.text.b) *(.text.a)' if reverse else '*(.text*)')+' } >ROM\n'+('.ARM.exidx : { *(.ARM.exidx*) } >ROM\n' if metadata else '')+'_sidata = LOADADDR(.data); .data : { *(.data*) } >RAM AT>ROM\n.bss (NOLOAD) : { *(.bss*) } >RAM }\n');return p
 def link(self,objs,script,ok=True):
  out=OUT/'out.elf';old=b'previous-valid-output';out.write_bytes(old)
  env=os.environ.copy();env['XH_TEST_LINK_SCRIPT']=os.path.relpath(script,ROOT)
  p=subprocess.run(list(map(str,[WASMTIME,'run','--dir=.','--env','XH_TEST_LINK_SCRIPT='+script.relative_to(ROOT).as_posix(),OUT/'link.wasm',out.relative_to(ROOT),'a',0,0,0,0,0,*[os.path.relpath(o,ROOT) for o in objs]])),cwd=ROOT,env=env,capture_output=True,text=True)
  self.assertEqual(p.returncode,0 if ok else 1,p.stdout+p.stderr)
  if not ok:self.assertEqual(out.read_bytes(),old)
  return out,p.stderr
 def test_retained_type_link_order_lma_and_gnu(self):
  a,b=self.obj('a'),self.obj('b');script=self.script();out,_=self.link([a,b],script)
  e=ELFFile(io.BytesIO(out.read_bytes()));tables=[s for s in e.iter_sections() if s['sh_type']=='SHT_ARM_EXIDX'];self.assertEqual(len(tables),2)
  addresses=[]
  for s in tables:
   self.assertEqual(s['sh_flags'],0x82);code=e.get_section(s['sh_link']);self.assertEqual(code['sh_flags']&6,6)
   word,cant=struct.unpack('<II',s.data());d=word&0x7fffffff;d=d-(1<<31) if d&(1<<30) else d
   addresses.append(s['sh_addr']+d);self.assertEqual(addresses[-1],code['sh_addr']);self.assertEqual(cant,1)
  self.assertEqual(addresses,sorted(addresses))
  syms={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()};dat=next(s for s in e.iter_sections() if s.name=='.data' and s['sh_size']);segments=[p for p in e.iter_segments() if p['p_vaddr']<=dat['sh_addr']<p['p_vaddr']+p['p_memsz']];self.assertEqual(syms['_sidata'],segments[0]['p_paddr']);self.assertGreater(syms['_sidata'],tables[-1]['sh_addr'])
  oracle=OUT/'gnu.elf';p=subprocess.run(list(map(str,[GNU/('arm-none-eabi-ld'+EXE),'--no-merge-exidx-entries','-T',script,'-e','a',a,b,'-o',oracle])),capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
  g=ELFFile(io.BytesIO(oracle.read_bytes()));gt=g.get_section_by_name('.ARM.exidx');gw,gcant=struct.unpack('<II',gt.data()[:8]);gd=gw&0x7fffffff;gd=gd-(1<<31) if gd&(1<<30) else gd;self.assertEqual(gt['sh_addr']+gd,addresses[0]);self.assertEqual(gcant,1)
  # GNU coalesces adjacent CANTUNWIND entries even with --no-merge-exidx-entries.
  # xhcc deliberately retains every input record; compare each linker's actual LMA.
  gs={s.name:s['st_value'] for s in g.get_section_by_name('.symtab').iter_symbols()};gp=next(p for p in g.iter_segments() if p['p_vaddr']==0x20000000);self.assertEqual(gs['_sidata'],gp['p_paddr'])
 def test_reordered_code_rejected_atomically(self):
  _,err=self.link([self.obj('a'),self.obj('b')],self.script(reverse=True),False);self.assertIn('not strictly increasing',err)
 def test_unmatched_metadata_rejected(self):
  _,err=self.link([self.obj('a')],self.script(metadata=False),False);self.assertIn('unmatched alloc',err)
 def test_malformed_table_and_association_rejected(self):
  original=self.obj('a');data=original.read_bytes();e=ELFFile(io.BytesIO(data));idx=next(i for i,s in enumerate(e.iter_sections()) if s['sh_type']=='SHT_ARM_EXIDX');s=e.get_section(idx);sh=e['e_shoff']+idx*40
  for off,value in [(sh+24,0),(sh+24,idx),(sh+8,0x83),(sh+20,7),(sh+36,4),(s['sh_offset']+4,0x80000000)]:
   bad=OUT/'malformed.o';mut=bytearray(data);struct.pack_into('<I',mut,off,value);bad.write_bytes(mut);self.link([bad],self.script(),False)
 def test_real_runtime_member_retained(self):
  a=self.obj('a');runtime=ROOT.parent/'sd/micropython-host-make-run/runtime-libgcc-source/_udivmoddi4.o'
  if not runtime.exists():self.skipTest('optional external runtime audit input absent')
  copied=OUT/'real-udivmoddi4.o';copied.write_bytes(runtime.read_bytes())
  out,_=self.link([a,copied],self.script());e=ELFFile(io.BytesIO(out.read_bytes()));self.assertTrue(any(s.name=='.ARM.exidx.text.__udivmoddi4' and s['sh_type']=='SHT_ARM_EXIDX' for s in e.iter_sections()))
if __name__=='__main__':unittest.main()
