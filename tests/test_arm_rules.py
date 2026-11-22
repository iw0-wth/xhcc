"""Actual ARM object execution from external text rules; compiler never rebuilds.
Run after the common compiler build. GNU mixed/CPU/section regression gates are
owned by the main test runner, which must serialize builds.
"""
from pathlib import Path
import hashlib, os, shutil, subprocess, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    pd=Path(sys.executable).parent
    os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'

class ArmRules(unittest.TestCase):
    def command(self,*args,ok=True):
        p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True)
        if ok:self.assertEqual(p.returncode,0,p.stdout+'\n'+p.stderr)
        else:self.assertNotEqual(p.returncode,0,p.stdout+'\n'+p.stderr)
        return p
    def execute(self,elf_path,name='entry',a=0,b=0):
        with elf_path.open('rb') as stream:
            elf=ELFFile(stream);sym=elf.get_section_by_name('.symtab').get_symbol_by_name(name)[0]
            uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
            uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x20000)
            for sec in elf.iter_sections():
                if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
            uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x4f001)
            uc.reg_write(UC_ARM_REG_R0,a);uc.reg_write(UC_ARM_REG_R1,b)
            uc.emu_start(sym['st_value']|1,0x4f000,count=100000)
            return uc.reg_read(UC_ARM_REG_R0)&0xffffffff
    def compile_link(self,work,source,package_root=None,suffix='base'):
        obj=work/(suffix+'.o');elf=work/(suffix+'.elf')
        args=[sys.executable,'tools/xhcc.py']
        if package_root:args+=['--package-root',str(package_root),'variant-gcc']
        else:args+=['arm-none-eabi-gcc']
        self.command(*args,'-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
        self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',elf,obj)
        return elf
    def test_external_sequences_and_dynamic_constants_change_execution_same_sha(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-rules-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';pkg.mkdir(parents=True);(pkg/'include').mkdir()
            manifest=(ROOT/'packages/arm-cortex-m3/package.ini').read_text()
            manifest=manifest.replace('id=arm-cortex-m3','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc')
            if 'templates=' not in manifest:manifest+='\ntemplates=arm.rules\n'
            if 'encoding=' not in manifest:manifest+='\nencoding=arm.encoding.rules\n'
            (pkg/'package.ini').write_text(manifest)
            shutil.copy2(ROOT/'packages/arm-cortex-m3/arm.encoding.rules',pkg/'arm.encoding.rules')
            shutil.copy2(ROOT/'packages/arm-cortex-m3/object.rules',pkg/'object.rules')
            source=work/'source.c';source.write_text('int entry(int a,int b){return a+b;}\nint constant(void){return 41;}\n')
            base=(ROOT/'packages/arm-cortex-m3/arm.rules').read_text()
            variants=[base,base.replace('rule binary.add\ntext     ADD r0, r0, r1\\n','rule binary.add\ntext     SUB r0, r0, r1\\n',1),base.replace('rule constant.small\ntext     MOV r${0}, #${1}\\n','rule constant.small\ntext     MOV r${0}, #${1}\\n    ADD r${0}, r${0}, #1\\n',1)]
            outputs=[];constants=[]
            for i,text in enumerate(variants):
                (pkg/'arm.rules').write_text(text)
                elf=self.compile_link(work,source,packages,str(i));outputs.append(self.execute(elf,a=10,b=3));constants.append(self.execute(elf,'constant'))
            self.assertEqual(outputs[:2],[13,7]);self.assertEqual(constants,[41,41,42])
            (pkg/'arm.rules').write_text('XHTEMPLATE 999\n')
            rejected=work/'rejected.o'
            p=self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-c',source.relative_to(ROOT),'-o',rejected.relative_to(ROOT),ok=False)
            self.assertIn('template',p.stderr);self.assertFalse(rejected.exists())
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)
    def test_external_thumb_encoding_changes_execution_same_sha(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-encoding-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant'
            shutil.copytree(ROOT/'packages/arm-cortex-m3',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm-cortex-m3','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc')
            if 'templates=' not in manifest:manifest+='\ntemplates=arm.rules\n'
            if 'encoding=' not in manifest:manifest+='\nencoding=arm.encoding.rules\n'
            (pkg/'package.ini').write_text(manifest)
            source=work/'source.c';source.write_text('int entry(void){return 4096;}\n')
            path=pkg/'arm.encoding.rules';base=path.read_text();values=[]
            for i,variant in enumerate([False,True]):
                # MOVW immediate bit12 moves one place upward: 4096 -> 8192.
                path.write_text(base.replace('bits 1 12 4 16','bits 1 12 3 17',1) if variant else base)
                values.append(self.execute(self.compile_link(work,source,packages,'encoding'+str(i))))
            self.assertEqual(values,[4096,8192])
            path.write_text('XHENC 999\n');rejected=work/'bad.o'
            p=self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-c',source.relative_to(ROOT),'-o',rejected.relative_to(ROOT),ok=False)
            self.assertIn('encoding',p.stderr);self.assertFalse(rejected.exists())
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)

    def test_system_and_vfp_encodings_match_gnu_bytes(self):
        cases=[('cpsid','cpsid i',2),('cpsie','cpsie f',2),('dmb','dmb sy',4),('dsb','dsb sy',4),('isb','isb sy',4),('rev','rev r2, r3',2),('rev16','rev16 r4, r5',2),('revsh','revsh r6, r7',2),('rrx','rrx r3, r7',4)]
        for op in ['vadd','vsub','vmul','vdiv']:
            cases += [(op+'s',op+'.f32 s1, s16, s3',4),(op+'d',op+'.f64 d3, d9, d2',4)]
        for op in ['vneg','vabs','vsqrt']:
            cases += [(op+'s',op+'.f32 s17, s3',4),(op+'d',op+'.f64 d3, d9',4)]
        cases += [('movto','vmov s17, r3',4),('movfrom','vmov r4, s19',4),('vmrs','vmrs APSR_nzcv, FPSCR',4),('cmps','vcmp.f32 s17, s3',4),('cmpd','vcmp.f64 d3, d9',4),('cmpzero','vcmp.f32 s17, #0.0',4),('vldr','vldr s17, [r3, #24]',4),('vstr','vstr d9, [r4, #40]',4)]
        conversions=[('s32.f32','s17, s3'),('u32.f32','s17, s3'),('f32.s32','s17, s3'),('f32.u32','s17, s3'),('s32.f64','s17, d3'),('u32.f64','s17, d3'),('f64.s32','d9, s3'),('f64.u32','d9, s3'),('f32.f64','s17, d3'),('f64.f32','d9, s3')]
        cases += [('cvt'+str(i),'vcvt.'+suf+' '+operands,4) for i,(suf,operands) in enumerate(conversions)]
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-bytes-') as tmp:
            work=Path(tmp);source=work/'cases.c';gas=work/'cases.s';obj=work/'xh.o';ref=work/'gnu.o'
            source.write_text(''.join('__asm int case_'+name+'(void){\n'+asm+'\nBX lr\n}\n' for name,asm,_ in cases))
            gas.write_text('.syntax unified\n.thumb\n'+''.join('.global case_'+name+'\n.thumb_func\ncase_'+name+':\n'+asm+'\nbx lr\n' for name,asm,_ in cases))
            self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-mcpu=cortex-m7','-mfpu=fpv5-d16','-mfloat-abi=softfp','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
            self.command(GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m7','-mfpu=fpv5-d16','-mthumb',gas,'-o',ref)
            def code(path):
                with path.open('rb') as f:
                    elf=ELFFile(f);result={}
                    for name,_,size in cases:
                        sym=elf.get_section_by_name('.symtab').get_symbol_by_name('case_'+name)[0];sec=elf.get_section(sym['st_shndx']);start=(sym['st_value']&~1)-sec['sh_addr'];result[name]=sec.data()[start:start+size]
                    return result
            actual=code(obj);expected=code(ref)
            for name,asm,_ in cases:self.assertEqual(actual[name],expected[name],asm)

    def test_vfp_arithmetic_executes(self):
        from unicorn.arm_const import UC_ARM_REG_C1_C0_2,UC_ARM_REG_FPEXC
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-vfp-') as tmp:
            work=Path(tmp);source=work/'vfp.c';obj=work/'vfp.o';elfpath=work/'vfp.elf'
            source.write_text('__asm int entry(void){\nMOVW r0, #0\nMOVT r0, #0x3fc0\nVMOV s17, r0\nMOVT r0, #0x4020\nVMOV s3, r0\nVADD.F32 s19, s17, s3\nVMOV r0, s19\nBX lr\n}\n')
            self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-mcpu=cortex-m7','-mfpu=fpv5-d16','-mfloat-abi=softfp','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
            self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',elfpath,obj)
            with elfpath.open('rb') as f:
                elf=ELFFile(f);uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x20000)
                for sec in elf.iter_sections():
                    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
                uc.reg_write(UC_ARM_REG_C1_C0_2,uc.reg_read(UC_ARM_REG_C1_C0_2)|(0xf<<20));uc.reg_write(UC_ARM_REG_FPEXC,0x40000000)
                uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.emu_start(elf['e_entry']|1,0x4f000,count=10000)
                self.assertEqual(uc.reg_read(UC_ARM_REG_R0),0x40800000) # 1.5 + 2.5 = 4.0

    def test_scalar_operations_execute(self):
        operations={'add':'+','sub':'-','mul':'*','and':'&','or':'|','xor':'^','shl':'<<','shr':'>>','eq':'==','ne':'!=','lt':'<','gt':'>','le':'<=','ge':'>=','div':'/','mod':'%'}
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-scalar-') as tmp:
            work=Path(tmp);source=work/'source.c'
            text='int entry(void){return 0;}\n'
            for prefix,typ in [('s','int'),('u','unsigned')]:
                for name,op in operations.items():text+=f'{typ} {prefix}_{name}({typ} a,{typ} b){{return a {op} b;}}\n'
            text+='int neg(int a){return -a;} int bnot(int a){return ~a;} int lnot(int a){return !a;}\n'
            source.write_text(text);elf=self.compile_link(work,source)
            for unsigned in [False,True]:
                for name,op in operations.items():
                    pairs=[(0,1),(1,1),(0xffffffff,1),(0x80000000,0x7fffffff),(17,5)]
                    if name in ['shl','shr']:pairs=[(a,b) for a in [0,1,0xffffffff,0x80000000] for b in [0,1,15,31]]
                    for a,b in pairs:
                        aa=a if unsigned or a<0x80000000 else a-0x100000000;bb=b if unsigned or b<0x80000000 else b-0x100000000
                        if name in ['div','mod']:
                            q=abs(aa)//abs(bb);q=-q if (aa<0)!=(bb<0) else q
                            want=q if name=='div' else aa-q*bb
                        else:want=eval(f'aa {op} bb')
                        self.assertEqual(self.execute(elf,('u_' if unsigned else 's_')+name,a,b),int(want)&0xffffffff,(name,unsigned,a,b))
            for a in [0,1,0xffffffff,0x80000000]:
                for name,want in [('neg',-a),('bnot',~a),('lnot',not a)]:self.assertEqual(self.execute(elf,name,a),int(want)&0xffffffff)
if __name__=='__main__':unittest.main()
