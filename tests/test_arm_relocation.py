"""Actual shared ARM relocation VM: native formula/GNU oracles and atomic errors.
Builds only the small VM test harness, never a compiler image.
"""
from pathlib import Path
import os,sys,subprocess,unittest,hashlib
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/arm-relocation-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_relocation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){unsigned char code[64];int i,len,rc;XhRelocContext c={0};if(n!=8)return 2;len=(int)strlen(v[7])/2;if(len>64)return 2;for(i=0;i<len;i++){unsigned x;if(sscanf(v[7]+i*2,"%2x",&x)!=1)return 2;code[i]=(unsigned char)x;}c.symbol=strtoull(v[3],0,0);c.place=strtoull(v[4],0,0);if(xh_relocation_set_path("arm",v[1]))return 2;rc=xh_relocation_apply("arm",v[2],code,(size_t)strtoul(v[5],0,0),(size_t)strtoul(v[6],0,0),&c);for(i=0;i<len;i++)printf("%02x",code[i]);puts("");return rc?1:0;}
'''
def mov(rd,imm,top=False):
    hi=(0xf2c0 if top else 0xf240)|((imm>>11&1)<<10)|(imm>>12&15)
    lo=((imm>>8&7)<<12)|(rd<<8)|(imm&255)
    return hi.to_bytes(2,'little')+lo.to_bytes(2,'little')
def pair(rd,value):return mov(rd,value&65535)+mov(rd,value>>16,True)
def branch(key,delta,cond=0):
    off=delta>>1
    if key=='THM_BCOND':
        hi=0xf000|((off>>19&1)<<10)|(cond<<6)|(off>>11&63);lo=0x8000|((off>>18&1)<<13)|((off>>17&1)<<11)|(off&2047)
    else:
        sign=off>>23&1;j1=~((off>>22&1)^sign)&1;j2=~((off>>21&1)^sign)&1
        hi=0xf000|(sign<<10)|(off>>11&1023);lo=(0xd000 if key=='THM_BL' else 0x9000)|(j1<<13)|(j2<<11)|(off&2047)
    return hi.to_bytes(2,'little')+lo.to_bytes(2,'little')
class ArmRelocation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
        env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','')
        p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/arm-relocation-tests/harness.c','src/backend/rules/relocation.c','-o','build/arm-relocation-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)
    def apply(self,key,data,s=0,p=0,length=None,site=0,ok=True,path='packages/arm/relocation.rules'):
        proc=subprocess.run([WASM,'run','--dir=.','build/arm-relocation-tests/vm.wasm',path,key,str(s&0xffffffffffffffff),str(p&0xffffffffffffffff),str(len(data) if length is None else length),str(site),data.hex()],cwd=ROOT,capture_output=True,text=True)
        self.assertEqual(proc.returncode,0 if ok else 1,proc.stderr);return bytes.fromhex(proc.stdout.strip())
    def test_mov_pair_native_oracle_and_atomic_rejection(self):
        for rd in [0,3,12,15]:
            for value in [0,1,0x12345678,0x7fffffff,0xffffffff]:
                for delta in [0,1,-1,0x10000,0x7fffffff]:
                    data=b'\xaa\xbb'+pair(rd,value)+b'\xcc\xdd'
                    got=self.apply('MOVW_MOVT_DELTA',data,delta&0xffffffff,site=2)
                    self.assertEqual(got,b'\xaa\xbb'+pair(rd,(value+delta)&0xffffffff)+b'\xcc\xdd')
        good=pair(3,0x12345678)
        bad=[good[:4]+mov(4,0x1234,True),mov(3,0x5678,True)+good[4:],good[:4]+mov(3,0x1234),bytes([0])+good[1:],good[:3]+bytes([good[3]|128])+good[4:]]
        for data in bad:self.assertEqual(self.apply('MOVW_MOVT_DELTA',data,1,ok=False),data)
        self.assertEqual(self.apply('MOVW_MOVT_DELTA',good,1,length=7,ok=False),good)
    def test_branch_native_oracle_boundaries_and_atomic(self):
        for key in ['THM_BL','THM_BW','THM_BCOND']:
            limit=1<<20 if key=='THM_BCOND' else 1<<24
            for delta in [-limit,-4096,-4,0,2,4096,limit-2]:
                for cond in ([0,1,10,13] if key=='THM_BCOND' else [0]):
                    data=branch(key,0,cond);got=self.apply(key,data,s=0x2000000+4+delta,p=0x2000000)
                    self.assertEqual(got,branch(key,delta,cond))
            data=branch(key,0,3)
            for delta in [-limit-2,limit,1]:self.assertEqual(self.apply(key,data,s=0x2000000+4+delta,p=0x2000000,ok=False),data)
            invalid=data[:1]+bytes([0])+data[2:];self.assertEqual(self.apply(key,invalid,s=10,ok=False),invalid)
    def test_narrow_boundaries_preservation_and_atomicity(self):
        for key,opcode,low,high,align in [('THM_B16',0xe000,-2048,2046,2),('THM_BCC16',0xd500,-256,254,2),('THM_ADR16',0xa300,0,1020,4),('THM_CBZ16',0xb103,0,126,2),('THM_CBNZ16',0xb903,0,126,2)]:
            initial=opcode.to_bytes(2,'little');place=0x1002;base=((place+4)&~3) if key=='THM_ADR16' else place+4
            for delta in [low,0,align,high]:
                got=int.from_bytes(self.apply(key,initial,s=base+delta,p=place),'little')
                if key=='THM_B16':want=opcode|((delta>>1)&2047)
                elif key=='THM_BCC16':want=opcode|((delta>>1)&255)
                elif key=='THM_ADR16':want=opcode|(delta>>2)
                else:want=opcode|(((delta>>1)&31)<<3)|(((delta>>1)&32)<<4)
                self.assertEqual(got,want,(key,delta))
            for delta in [low-align,high+align,1]:self.assertEqual(self.apply(key,initial,s=base+delta,p=place,ok=False),initial)
            invalid=b'\x00\x00';self.assertEqual(self.apply(key,invalid,s=base,p=place,ok=False),invalid)
            self.assertEqual(self.apply(key,initial,s=base,p=place,length=1,ok=False),initial)

    def test_mov_pair_matches_gnu_assembled_bytes(self):
        sys.path.insert(0,str(ROOT/'build/test-deps'))
        from elftools.elf.elffile import ELFFile
        source=OUT/'oracle.s';obj=OUT/'oracle.o';value=0x12345678;delta=0x1abcd
        wanted=(value+delta)&0xffffffff
        source.write_text('.syntax unified\n.thumb\n.global oracle\noracle:\nmovw r9, #'+str(wanted&65535)+'\nmovt r9, #'+str(wanted>>16)+'\n')
        gnu=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin/arm-none-eabi-as.exe'
        proc=subprocess.run([str(gnu),'-mcpu=cortex-m3','-mthumb',str(source),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
        self.assertEqual(proc.returncode,0,proc.stderr)
        with obj.open('rb') as f:expected=ELFFile(f).get_section_by_name('.text').data()
        self.assertEqual(self.apply('MOVW_MOVT_DELTA',pair(9,value),delta),expected)

    def test_actual_compiler_branch_formula_variant_same_sha(self):
        import tempfile,shutil
        sys.path.insert(0,str(ROOT/'build/test-deps'))
        from elftools.elf.elffile import ELFFile
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-reloc-compiler-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'source.c';source.write_text('__asm int entry(void){\nB .Ldest\nNOP\n.Ldest:\nMOV r0, #17\nBX lr\n}\n')
            path=pkg/'relocation.rules';base=path.read_text();start=base.index('rule THM_BW 4');end=base.index('\nend',start)
            changed=base[:start]+base[start:end].replace('const r3 0x4','const r3 0x8',1)+base[end:]
            self.assertNotEqual(base,changed);outputs=[]
            for i,text in enumerate([base,changed]):
                path.write_text(text);obj=work/(str(i)+'.o')
                proc=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(packages),'variant-gcc','-O0','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(proc.returncode,0,proc.stderr)
                with obj.open('rb') as f:outputs.append(ELFFile(f).get_section_by_name('.text').data())
            self.assertNotEqual(outputs[0],outputs[1]);self.assertEqual(len(outputs[0]),len(outputs[1]))
            self.assertEqual(outputs[0][:4],branch('THM_BW',2));self.assertEqual(outputs[1][:4],branch('THM_BW',-2))
            from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
            from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_LR,UC_ARM_REG_SP
            uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000);uc.mem_write(0x10000,outputs[0]);uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x2f001);uc.emu_start(0x10001,0x2f000,count=10000)
            self.assertEqual(uc.reg_read(UC_ARM_REG_R0),17)
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)

    def test_actual_compiler_narrow_formula_variant_same_sha(self):
        import tempfile,shutil
        sys.path.insert(0,str(ROOT/'build/test-deps'))
        from elftools.elf.elffile import ELFFile
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-narrow-compiler-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'source.c';source.write_text('__asm int entry(void){\nB .Ldest\nNOP\n.Ldest:\nMOV r0, #17\nBX lr\n}\n')
            path=pkg/'relocation.rules';base=path.read_text();start=base.index('rule THM_B16 2');end=base.index('\nend',start)
            changed=base[:start]+base[start:end].replace('const r3 4','const r3 8',1)+base[end:];self.assertNotEqual(base,changed);outputs=[]
            for i,text in enumerate([base,changed]):
                path.write_text(text);obj=work/(str(i)+'.o')
                proc=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(packages),'variant-gcc','-O0','-fopt-branch','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(proc.returncode,0,proc.stderr)
                with obj.open('rb') as f:outputs.append(ELFFile(f).get_section_by_name('.text').data())
            self.assertEqual(outputs[0][:2],bytes.fromhex('00e0'));self.assertEqual(outputs[1][:2],bytes.fromhex('fee7'));self.assertNotEqual(outputs[0],outputs[1])
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)

    def test_external_formula_variant_same_vm_sha(self):
        vm=OUT/'vm.wasm';before=hashlib.sha256(vm.read_bytes()).digest();path=OUT/'variant.rules'
        text=(ROOT/'packages/arm/relocation.rules').read_text();path.write_text(text.replace('set r7 S\nadd r6 r6 r7','set r7 S\nsub r6 r6 r7',1))
        data=pair(3,100);normal=self.apply('MOVW_MOVT_DELTA',data,7);changed=self.apply('MOVW_MOVT_DELTA',data,7,path=str(path.relative_to(ROOT)))
        self.assertEqual(normal,pair(3,107));self.assertEqual(changed,pair(3,93));self.assertEqual(hashlib.sha256(vm.read_bytes()).digest(),before)
if __name__=='__main__':unittest.main()
