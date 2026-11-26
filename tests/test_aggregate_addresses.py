"""Certified aggregate address slots and cast string literal regressions.
Use XHCC_COMMAND for post-build ELF validation; source harness needs only GCC.
"""
import os
from pathlib import Path
import subprocess
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_aggregate_constexpr import HARNESS, AggregateConstexprSourceHarness, AggregateConstexprArmElf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))

SOURCE = HARNESS.split('int main(void)')[0].replace(
    'int type_ptr_size(void) { return 4; }',
    'static int ptr_bytes=4; int type_ptr_size(void) { return ptr_bytes; }') + r'''
static int calls;
static int certify(void *ud,int off,Type *t) {
    (void)ud;(void)t;calls++;return off==0;
}
static int check(int kind,int width,int expected) {
    Type scalar={.kind=kind},array={.kind=TYPE_ARRAY,.base=&scalar,.size=1};
    unsigned char out[8]={0};int rc;
    ptr_bytes=width;calls=0;
    rc=ark_agg_fill_ex(&array,"{&symbol}",out,sizeof(out),certify,0);
    return (expected ? rc!=0 || calls!=1 : rc==0 || calls!=0);
}
int main(void) {
    if(check(TYPE_INT,4,1)||check(TYPE_LLONG,8,1)||
       check(TYPE_INT,8,0)||check(TYPE_SHORT,4,0)||
       check(TYPE_CHAR,4,0)||check(TYPE_FLOAT,4,0)||
       check(TYPE_DOUBLE,8,0)) return 1;
    puts("certified 4/8-byte integer addresses and narrow/float rejection: OK");
    return 0;
}
'''

class AggregateAddressSourceHarness(unittest.TestCase):
    def test_certified_slots_follow_target_pointer_width(self):
        AggregateConstexprSourceHarness.run_harness(self,SOURCE,'agginit.c')

class AggregateAddressArmElf(unittest.TestCase):
    setUpClass = classmethod(AggregateConstexprArmElf.setUpClass.__func__)
    tearDownClass = classmethod(AggregateConstexprArmElf.tearDownClass.__func__)
    compile = AggregateConstexprArmElf.compile

    def test_cast_strings_and_uintptr_vector_match_gnu_relocations(self):
        from elftools.elf.elffile import ELFFile
        source=r'''
typedef unsigned int uintptr_t;
extern int anchor;
extern void handler(void);
struct S { int *base; unsigned int hash,len; const unsigned char *data; };
const struct S version={&anchor,0,7,(const unsigned char *)(const char *)"a,]b\\c"};
const uintptr_t vector[] __attribute__((section(".isr_vector")))={
    (uintptr_t)&anchor,(uintptr_t)&handler,0,(uintptr_t)&handler
};
'''
        src,obj,result=self.compile(source,'addresses')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        gcc=os.environ.get('XH_C_ARM_CC',r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe')
        oracle=src.with_name('gnu-addresses.o')
        ref=subprocess.run([gcc,'-std=c99','-O0','-c',str(src),'-o',str(oracle)],capture_output=True,text=True,timeout=60)
        self.assertEqual(ref.returncode,0,ref.stdout+ref.stderr)
        def inspect(path):
            with path.open('rb') as f:
                elf=ELFFile(f);syms=elf.get_section_by_name('.symtab');objects={}
                for name in ('version','vector'):
                    s=syms.get_symbol_by_name(name)[0];sec=elf.get_section(s['st_shndx']);start=s['st_value'];end=start+s['st_size'];refs=[]
                    for relsec in elf.iter_sections():
                        if relsec['sh_type']!='SHT_REL' or relsec['sh_info']!=s['st_shndx']:continue
                        rs=elf.get_section(relsec['sh_link'])
                        for r in relsec.iter_relocations():
                            if start<=r['r_offset']<end:
                                target=rs.get_symbol(r['r_info_sym']);label=target.name
                                if name=='version' and r['r_offset']-start==12:
                                    pool=elf.get_section(target['st_shndx']).data()
                                    add=int.from_bytes(sec.data()[r['r_offset']:r['r_offset']+4],'little')
                                    self.assertEqual(pool[target['st_value']+add:target['st_value']+add+7],bytes([97,44,93,98,92,99,0]))
                                    label='string'
                                refs.append((r['r_offset']-start,label,r['r_info_type']))
                    objects[name]=(s['st_size'],sec.data()[start:end],refs)
                return objects
        actual,expected=inspect(obj),inspect(oracle)
        self.assertEqual(actual,expected)
        self.assertEqual(actual['version'][2],[(0,'anchor',2),(12,'string',2)])
        self.assertEqual(actual['vector'][2],[(0,'anchor',2),(4,'handler',2),(12,'handler',2)])

if __name__=='__main__':unittest.main(verbosity=2)
