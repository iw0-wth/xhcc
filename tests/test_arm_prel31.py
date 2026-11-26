"""PREL31 production VM tests; independent of the main compiler build."""
import unittest
from test_arm_elf_reloc import ArmElfReloc, GNU, OUT, ELFFile

class Prel31(ArmElfReloc):
 def test_prel31_vectors_atomicity(self):
  for addend in [0, 7, -7, (1<<30)-1, -(1<<30)]:
   for high in [0, 0x80000000]:
    word=(addend&0x7fffffff)|high
    decoded,got=self.apply(42,word.to_bytes(4,'little'),0x20000,0x20000)
    self.assertEqual(decoded,(addend,4));self.assertEqual(got,word.to_bytes(4,'little'))
  for symbol,place in [(1<<30,0),(0,(1<<30)+1)]:
   data=bytes.fromhex('00000080')
   self.assertEqual(self.apply(42,data,symbol,place,ok=False)[1],data)
  self.assertEqual(self.apply(42,(1).to_bytes(4,'little'),0x20001,0x10000,thumb=1)[1],(0x10001).to_bytes(4,'little'))
  self.assertEqual(self.apply(42,(1).to_bytes(4,'little'),0x20001,0x10000,thumb=0)[1],(0x10002).to_bytes(4,'little'))
 def test_prel31_gnu_oracle(self):
  source=OUT/'prel31.s';obj=OUT/'prel31.o'
  source.write_text('.syntax unified\n.thumb\n.section .target,"ax",%progbits\n.global function\n.thumb_func\nfunction: bx lr\n.section .probe,"a",%progbits\n'+''.join('.word '+hex(high|(a&0x7fffffff))+'\n.reloc .-4, R_ARM_PREL31, '+sym+'\n' for high in [0,0x80000000] for a in [0,1,-3] for sym in ['function','.target']))
  self.run_cmd([GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m4','-mthumb',source,'-o',obj])
  with obj.open('rb') as f:
   e=ELFFile(f);raw=e.get_section_by_name('.probe').data();r=e.get_section_by_name('.rel.probe');tab=e.get_section(r['sh_link']);entries=[(x['r_offset'],tab.get_symbol(x['r_info_sym']).name) for x in r.iter_relocations()]
  for target,probe in [(0x20000,0x10000),(0x10000,0x20000)]:
   exe=OUT/'prel31.elf';self.run_cmd([GNU/'arm-none-eabi-ld.exe','--section-start=.target='+hex(target),'--section-start=.probe='+hex(probe),'-e','function',obj,'-o',exe])
   with exe.open('rb') as f:expected=ELFFile(f).get_section_by_name('.probe').data()
   for off,name in entries:self.assertEqual(self.apply(42,raw[off:off+4],target|int(name=='function'),probe+off,thumb=int(name=='function'))[1],expected[off:off+4])

if __name__=='__main__':unittest.main()
