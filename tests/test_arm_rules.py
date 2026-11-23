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
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(ROOT/'packages/arm/package.ini').read_text()
            manifest=manifest.replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc')
            if 'templates=' not in manifest:manifest+='\ntemplates=arm.rules\n'
            if 'encoding=' not in manifest:manifest+='\nencoding=arm.encoding.rules\n'
            (pkg/'package.ini').write_text(manifest)
            shutil.copy2(ROOT/'packages/arm/arm.encoding.rules',pkg/'arm.encoding.rules')
            shutil.copy2(ROOT/'packages/arm/object.rules',pkg/'object.rules')
            shutil.copy2(ROOT/'packages/arm/relocation.rules',pkg/'relocation.rules')
            source=work/'source.c';source.write_text('int entry(int a,int b){return a+b;}\nint constant(void){return 41;}\n')
            base=(ROOT/'packages/arm/arm.rules').read_text()
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
            shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc')
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
        for reg in ['MSP','PSP','PRIMASK','FAULTMASK','BASEPRI','CONTROL']:
            cases += [('mrs'+reg,'mrs r3, '+reg,4),('msr'+reg,'msr '+reg+', r4',4)]
        cases += [('vpushs','vpush {s16-s19}',4),('vpops','vpop {s16-s19}',4),('vpushd','vpush {d8-d11}',4),('vpopd','vpop {d8-d11}',4),('vldms','vldmia r3!, {s17-s20}',4),('vstmd','vstmdb r4!, {d8-d11}',4),('clz','clz r3, r9',4),('rbit','rbit r4, r10',4),('tsthi','tst r8, r9',4)]
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

    def test_branches_match_gnu_bytes_forward_backward_and_short(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-branch-') as tmp:
            work=Path(tmp)
            for short in [False,True]:
                body='.Lstart:\nNOP\nBL .Lforward\nB .Lforward\nBEQ .Lforward\nBNE .Lstart\n.Lforward:\nBL .Lstart\nB .Lstart\nBEQ .Lstart\nBX lr\n'
                source=work/'branches.c';gas=work/'branches.s';obj=work/('xh'+str(short)+'.o');ref=work/('gnu'+str(short)+'.o')
                source.write_text('__asm int entry(void){\n'+body+'}\n')
                # The compiler chooses wide local branches unless branch shrinking is enabled.
                wide=body.replace('B .Lforward','B.W .Lforward').replace('BEQ .Lforward','BEQ.W .Lforward').replace('BNE .Lstart','BNE.W .Lstart').replace('B .Lstart','B.W .Lstart').replace('BEQ .Lstart','BEQ.W .Lstart')
                gas.write_text('.syntax unified\n.thumb\n.global entry\n.thumb_func\nentry:\n'+(body if short else wide))
                args=[sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-O0']
                if short:args+=['-fopt-branch']
                self.command(*args,'-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                self.command(GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m3','-mthumb',gas,'-o',ref)
                def section(path):
                    with path.open('rb') as f:return ELFFile(f).get_section_by_name('.text').data()
                expected=section(ref)
                self.assertEqual(section(obj)[:len(expected)],expected,'short='+str(short))

    def test_extended_encoding_formulas_gnu_bytes(self):
        cases=[('stm16','stmia r3!, {r0-r2}',2),('ldm16','ldmia r3!, {r0-r2}',2),('stmwide','stmia r8!, {r0-r3,r9,lr}',4),('ldmwide','ldmia r8!, {r0-r3,r9,lr}',4),('stmdb','stmdb r8!, {r0-r3,r9,lr}',4),('ldmdb','ldmdb r8!, {r0-r3,r9,lr}',4),('tbb','tbb [r3,r4]',4),('tbh','tbh [r3,r4,lsl #1]',4),('mla','mla r0,r1,r2,r3',4),('mls','mls r0,r1,r2,r3',4),('smull','smull r0,r1,r2,r3',4),('smlal','smlal r0,r1,r2,r3',4),('sadd','sadd16 r3,r7,r5',4),('movshi','movs r12,r3',4),('movsim','movs r9,#0x80000000',4),('addsim','adds r8,r9,#0x100',4),('subsim','subs r8,r9,#0x100',4),('lsls','lsls r8,r9,r10',4),('lsrs','lsrs r8,r9,r10',4),('asrs','asrs r8,r9,r10',4),('ands','ands r3,r4',2),('orrs','orrs r3,r4',2),('eors','eors r3,r4',2),('bics','bics r3,r4',2),('mvns','mvns r3,r4',2)]
        for op in ['sxtb','sxth','uxtb','uxth']:cases += [(op+'lo',op+' r3,r4',2),(op+'hi',op+' r8,r9',4)]
        for op in ['ldrd','strd']:
            for i,offset in enumerate([-1020,-4,0,4,1020]):cases.append((op+str(i),op+' r2,r3,[r8,#'+str(offset)+']',4))
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-extra-bytes-') as tmp:
            work=Path(tmp);source=work/'cases.c';gas=work/'cases.s';obj=work/'xh.o';ref=work/'gnu.o'
            source.write_text(''.join('__asm int case_'+name+'(void){\n'+asm+'\nBX lr\n}\n' for name,asm,_ in cases))
            gas.write_text('.syntax unified\n.thumb\n'+''.join('.global case_'+name+'\n.thumb_func\ncase_'+name+':\n'+asm+'\nbx lr\n' for name,asm,_ in cases))
            self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','--asm-extra','-mcpu=cortex-m7','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
            self.command(GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m7','-mthumb',gas,'-o',ref)
            def code(path):
                with path.open('rb') as f:
                    elf=ELFFile(f);out={}
                    for name,_,size in cases:
                        sym=elf.get_section_by_name('.symtab').get_symbol_by_name('case_'+name)[0];sec=elf.get_section(sym['st_shndx']);start=(sym['st_value']&~1)-sec['sh_addr'];out[name]=sec.data()[start:start+size]
                    return out
            actual=code(obj);expected=code(ref)
            for name,asm,_ in cases:self.assertEqual(actual[name],expected[name],asm)
            for asm in ['ldrd r2,r3,[r8,#1024]','strd r2,r3,[r8,#2]','ldrd r3,r4,[r8,#4]','tbb [r3,r4,lsl #1]','tbh [r3,r4,lsl #2]']:
                source.write_text('__asm int entry(void){\n'+asm+'\nBX lr\n}\n');self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','--asm-extra','-O0','-c',source.relative_to(ROOT),'-o',(work/'bad.o').relative_to(ROOT),ok=False)

    def test_mnemonic_map_changes_actual_execution_same_sha(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-map-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'source.c';source.write_text('__asm int entry(void){\nMOV r1,#3\nMOV r2,#4\nMOV r3,#20\nMLA r0,r1,r2,r3\nBX lr\n}\n')
            path=pkg/'arm.rules';base=path.read_text();values=[]
            for i,text in enumerate([base,base.replace('rule map.MLA\ntext MLA\n','rule map.MLA\ntext MLS\n',1)]):
                path.write_text(text);obj=work/(str(i)+'.o');exe=work/(str(i)+'.elf')
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','--asm-extra','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',exe,obj);values.append(self.execute(exe))
            self.assertEqual(values,[32,8])
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)

    def test_abi_descriptor_self_calls_varargs_and_contract_rejection(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';before=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-abi-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'abi.c';source.write_text('#include <stdarg.h>\nlong long mixed(int a,long long b,int c,long long d,int e){return a+b+c+d+e;}\nlong long variadic(int tag,...){va_list ap;long long a,b;int c;va_start(ap,tag);a=va_arg(ap,long long);c=va_arg(ap,int);b=va_arg(ap,long long);va_end(ap);return tag+a+c+b;}\nstruct Pair{int a,b;};int pair(int a,struct Pair p,int z){return a+p.a+p.b+z;}\nint entry(void){struct Pair p;p.a=11;p.b=13;return (int)mixed(1,0x100000002LL,3,0x200000004LL,5)+(int)variadic(7,0x100000008LL,9,0x20000000aLL)+pair(3,p,17);}\n')
            elf=self.compile_link(work,source,packages,'abi');self.assertEqual(self.execute(elf),93)
            path=pkg/'arm.rules';base=path.read_text()
            for bad in ['4 3 2 8 8 16 1','4 4 2 8 4 16 1','4 4 2 8 8 12 1','4 4 2 8 8 16 1 0','4 -4 2 8 8 16 1']:
                path.write_text(base.replace('text 4 4 2 8 8 16 1\n','text '+bad+'\n',1));obj=work/'bad.o'
                if obj.exists():obj.unlink()
                p=self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT),ok=False)
                self.assertIn('AAPCS',p.stderr);self.assertFalse(obj.exists())
        self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),before)

    def test_memory_policy_forms_gnu_bytes_and_safe_same_sha_variant(self):
        cases=[('wordlo','ldr r3,[r4,#124]',2),('wordwide','ldr r3,[r4,#128]',4),('byte','strb r3,[r4,#31]',2),('bytewide','strb r3,[r4,#32]',4),('half','ldrh r3,[r4,#62]',2),('halfwide','ldrh r3,[r4,#64]',4),('signedb','ldrsb r3,[r4,#255]',4),('signedh','ldrsh r3,[r4,#4095]',4),('negative','ldr r3,[r4,#-255]',4),('sp','str r3,[sp,#1020]',2),('pc','ldr r3,[pc,#1020]',2),('reglo','ldr r3,[r4,r5]',2),('regwide','ldr r8,[r9,r10,lsl #3]',4),('writeback','strh r3,[r4,#-255]!',4)]
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-memory-policy-') as tmp:
            work=Path(tmp);source=work/'cases.c';gas=work/'cases.s';obj=work/'xh.o';ref=work/'gnu.o'
            source.write_text(''.join('__asm int case_'+name+'(void){\n'+asm+'\nBX lr\n}\n' for name,asm,_ in cases));gas.write_text('.syntax unified\n.thumb\n'+''.join('.global case_'+name+'\n.thumb_func\ncase_'+name+':\n'+asm+'\nbx lr\n' for name,asm,_ in cases))
            self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT));self.command(GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m3','-mthumb',gas,'-o',ref)
            def code(path):
                with path.open('rb') as f:
                    elf=ELFFile(f);out={}
                    for name,_,size in cases:
                        sym=elf.get_section_by_name('.symtab').get_symbol_by_name('case_'+name)[0];sec=elf.get_section(sym['st_shndx']);start=(sym['st_value']&~1)-sec['sh_addr'];out[name]=sec.data()[start:start+size]
                    return out
            actual=code(obj);expected=code(ref)
            for name,asm,_ in cases:self.assertEqual(actual[name],expected[name],asm)
            packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg);manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source.write_text('int entry(void){int x=41;return x+1;}\n');path=pkg/'arm.rules';base=path.read_text();compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest();images=[];values=[]
            # Prefer wide positive offsets by deleting only the IMM16 candidate.
            variant=base.replace('4 0 7 0 1 0 0 0 124 4\\n','',2);self.assertNotEqual(base,variant)
            for i,text in enumerate([base,variant]):
                path.write_text(text);elf=self.compile_link(work,source,packages,'policy'+str(i));images.append(elf.read_bytes());values.append(self.execute(elf))
            self.assertEqual(values,[42,42]);self.assertNotEqual(images[0],images[1]);self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
            path.write_text(base.replace('rule mem.policy.L.W\n','rule missing.mem.policy.L.W\n',1));self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',(work/'bad.o').relative_to(ROOT),ok=False)

    def test_float_semantic_selection_same_sha_and_effect_validation(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-float-semantic-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'float.c';source.write_text('int entry(void){float a=7.5f,b=2.5f;return (int)(a+b);}\nint casts(void){unsigned u=3000000000u;double d=(double)u;float f=(float)d;long long k=(long long)f;return ((unsigned)d==u)&&((unsigned long long)k==3000000000ULL)&&((int)f==2147483647);}\n')
            path=pkg/'arm.rules';base=path.read_text();values=[]
            lib=subprocess.run([str(GNU/'arm-none-eabi-gcc.exe'),'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-print-libgcc-file-name'],capture_output=True,text=True,check=True).stdout.strip()
            original='floatbin add 32 v soft sem.floatbin.add.32.soft 2 1';variant='floatbin add 32 v soft sem.floatbin.sub.32.soft 2 1'
            for i,text in enumerate([base,base.replace(original,variant,1)]):
                path.write_text(text);obj=work/(str(i)+'.o');exe=work/(str(i)+'.elf')
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',exe,obj,lib);values.append(self.execute(exe))
            self.assertEqual(values,[10,5]);self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
            for replacement in [original.replace('2 1','1 1'),original+'\\n'+original,original.replace('floatbin add','floatbin missing')]:
                path.write_text(base.replace(original,replacement,1));obj=work/'bad.o'
                if obj.exists():obj.unlink()
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT),ok=False);self.assertFalse(obj.exists())

    def test_hardware_float_semantic_modes_execute(self):
        from unicorn.arm_const import UC_ARM_REG_C1_C0_2,UC_ARM_REG_FPEXC,UC_ARM_REG_PC,UC_CPU_ARM_CORTEX_A15
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-fp-modes-') as tmp:
            work=Path(tmp);source=work/'fp.c'
            source.write_text('int entry(void){float a=7.5f,b=2.5f;unsigned u=19;return (int)((a+b)*(a-b)/b)+(int)(float)u;}\nint dp(void){double a=7.5,b=2.5;unsigned u=19;return (int)((a+b)*(a-b)/b)+(int)(double)u;}\nint nancompare(void){union {unsigned u;float f;} n;n.u=0x7fc00000u;return (n.f<1.0f)+2*(n.f<=1.0f)+4*(n.f>1.0f)+8*(n.f>=1.0f)+16*(n.f==1.0f)+32*(n.f!=1.0f);}\n')
            for cpu,fpu,name in [('cortex-m4','fpv4-sp-d16','entry'),('cortex-m7','fpv5-d16','dp')]:
                obj=work/(name+'.o');exe=work/(name+'.elf')
                self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-mcpu='+cpu,'-mfpu='+fpu,'-mfloat-abi=softfp','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                lib=subprocess.run([str(GNU/'arm-none-eabi-gcc.exe'),'-mcpu='+cpu,'-mfpu='+fpu,'-mfloat-abi=softfp','-mthumb','-print-libgcc-file-name'],capture_output=True,text=True,check=True).stdout.strip()
                self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e',name,'-o',exe,obj,lib)
                with exe.open('rb') as f:
                    # Cortex-A15 provides the DP instruction oracle Unicorn's default
                    # MCLASS model lacks; target compilation remains M4/M7 softfp.
                    # This checks Thumb/VFP results, not full MCU behavior or speed.
                    elf=ELFFile(f);uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB);uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15);uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x20000)
                    for sec in elf.iter_sections():
                        if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
                    uc.reg_write(UC_ARM_REG_C1_C0_2,uc.reg_read(UC_ARM_REG_C1_C0_2)|(0xf<<20));uc.reg_write(UC_ARM_REG_FPEXC,0x40000000);uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x4f001);
                    try:uc.emu_start(elf['e_entry']|1,0x4f000,count=100000)
                    except Exception as exc:self.fail(str((cpu,fpu,name,hex(uc.reg_read(UC_ARM_REG_PC)),exc)))
                    self.assertEqual(uc.reg_read(UC_ARM_REG_R0),39,(cpu,fpu,name))
                    nanentry=elf.get_section_by_name('.symtab').get_symbol_by_name('nancompare')[0]['st_value'];uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.emu_start(nanentry|1,0x4f000,count=100000)
                    self.assertEqual(uc.reg_read(UC_ARM_REG_R0),32,'unordered NaN comparisons')

    def test_integer64_and_floatcmp_semantic_records_execute(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-64-semantic-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'ops.c';source.write_text('long long divide(long long a,long long b){return a/b;}\nint relation(long long a,long long b){return a<b;}\nint entry(void){return (int)divide(37LL,5LL);}\nint compare(void){return relation(3LL,8LL);}\nint floatrelation(float a,float b){return a<b;}\nint fpentry(void){return floatrelation(3.0f,8.0f);}\nint bounds(void){long long a=-100LL,b=7LL;unsigned long long u=0x8000000000000000ULL;return ((a+b)==-93LL)&&((a-b)==-107LL)&&((a*b)==-700LL)&&((a%b)==-2LL)&&((a&b)==4LL)&&((a|b)==-97LL)&&((a^b)==-101LL)&&((a>>3)==-13LL)&&((u>>63)==1ULL)&&((u/3ULL)==3074457345618258602ULL)&&((u%3ULL)==2ULL)&&((b<<33)==60129542144LL)&&(u>7ULL);}\n')
            lib=subprocess.run([str(GNU/'arm-none-eabi-gcc.exe'),'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-print-libgcc-file-name'],capture_output=True,text=True,check=True).stdout.strip();path=pkg/'arm.rules';base=path.read_text()
            changes=[base,base.replace('integerbin div 64 s stack sem.integerbin.div.64.s.stack 4 2','integerbin div 64 s stack sem.integerbin.mod.64.s.stack 4 2',1),base.replace('integercmp lt 64 s stack sem.integercmp.lt.64.s.stack 4 1','integercmp lt 64 s stack sem.integercmp.gt.64.s.stack 4 1',1),base.replace('floatcmp lt 32 v soft sem.floatcmp.lt.32.v.soft 2 1','floatcmp lt 32 v soft sem.floatcmp.gt.32.v.soft 2 1',1)]
            results=[]
            for i,text in enumerate(changes):
                path.write_text(text);obj=work/(str(i)+'.o');exe=work/(str(i)+'.elf')
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT));self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',exe,obj,lib)
                results.append((self.execute(exe),self.execute(exe,'compare'),self.execute(exe,'fpentry')))
                self.assertEqual(self.execute(exe,'bounds'),1)
            self.assertEqual(results,[(7,1,1),(2,1,1),(7,0,1),(7,1,0)]);self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)

    def test_complex_semantic_sequences_and_same_sha_selection(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-complex-semantic-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'complex.c';source.write_text('#include <complex.h>\n_Complex double plus(_Complex double a,_Complex double b){return a+b;}\nint entry(void){_Complex double a=3.0+4.0*I,b=1.0+2.0*I,c;c=plus(a,b);return (int)(creal(c)*10+cimag(c));}\nint operations(void){_Complex double a=3.0+4.0*I,b=1.0+2.0*I,m=a*b,d=a/b,n=conj(a);return ((int)creal(m)==-5)&&((int)cimag(m)==10)&&((int)(creal(d)*5)==11)&&((int)(cimag(d)*5)==-2)&&((int)creal(n)==3)&&((int)cimag(n)==-4)&&(a==a)&&(a!=b);}\nint conversions(void){int i=7;unsigned u=9;float f=11.0f;double d=13.0;_Complex double a=i,b=u,c=f,e=d;return (int)(creal(a)+creal(b)+creal(c)+creal(e)+cimag(a)+cimag(b));}\n')
            lib=subprocess.run([str(GNU/'arm-none-eabi-gcc.exe'),'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-print-libgcc-file-name'],capture_output=True,text=True,check=True).stdout.strip();path=pkg/'arm.rules';base=path.read_text();values=[]
            original='complex add 128 v stack sem.complex.add 8 4';variant='complex add 128 v stack sem.complex.sub 8 4'
            for i,text in enumerate([base,base.replace(original,variant,1)]):
                path.write_text(text);obj=work/(str(i)+'.o');exe=work/(str(i)+'.elf')
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT));self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',exe,obj,lib)
                values.append(self.execute(exe))
                if i==0:self.assertEqual(self.execute(exe,'operations'),1);self.assertEqual(self.execute(exe,'conversions'),40)
            self.assertEqual(values,[46,22]);self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)

    def test_complex_math_and_memory_abi_semantic_routes(self):
        compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-complex-routes-') as tmp:
            work=Path(tmp);packages=work/'packages';pkg=packages/'variant';shutil.copytree(ROOT/'packages/arm',pkg)
            manifest=(pkg/'package.ini').read_text().replace('id=arm','id=variant').replace('tools=arm-none-eabi-gcc,armcc','tools=variant-gcc');(pkg/'package.ini').write_text(manifest)
            source=work/'routes.c';source.write_text('#include <complex.h>\n_Complex float bounce(_Complex float z){return z;}\n_Complex double bounced(_Complex double z){return z;}\nint entry(void){_Complex double z=3.0+4.0*I;return (int)cabs(z);}\nint math_routes(void){_Complex double z=0.0+0.0*I,e=cexp(z);double a=carg(0.0+1.0*I);return ((int)creal(e)==1)&&((int)cimag(e)==0)&&(a>1.57)&&(a<1.58);}\nint memory_routes(void){_Complex float a=3.0f+4.0f*I,b;_Complex float v[2];v[0]=a;v[1]=5.0f+6.0f*I;b=bounce(v[0]);a=b;return (int)(crealf(a)*10+cimagf(a))+(int)(crealf(v[1])*10+cimagf(v[1]));}\nint double_return(void){_Complex double a=bounced(3.0+4.0*I);return (int)(creal(a)*10+cimag(a));}\n')
            source.write_text(source.read_text()+'int local_only(void){_Complex float a=3.0f+4.0f*I;return (int)(crealf(a)*10+cimagf(a));}\nint call_only(void){_Complex float a=bounce(3.0f+4.0f*I);return (int)(crealf(a)*10+cimagf(a));}\nint array_only(void){_Complex float v[2];v[0]=3.0f+4.0f*I;v[1]=5.0f+6.0f*I;return (int)(crealf(v[0])*10+cimagf(v[0]));}\n')
            def libpath(name):return subprocess.run([str(GNU/'arm-none-eabi-gcc.exe'),'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-print-file-name='+name],capture_output=True,text=True,check=True).stdout.strip()
            libgcc=libpath('libgcc.a');libm=libpath('libm.a');libc=libpath('libc.a');libnosys=libpath('libnosys.a');path=pkg/'arm.rules';base=path.read_text();values=[]
            original='complex abs 128 v stack sem.complex.abs.128.stack 4 2';variant='complex abs 128 v stack sem.complex.real 4 2'
            for i,text in enumerate([base,base.replace(original,variant,1)]):
                path.write_text(text);obj=work/(str(i)+'.o');exe=work/(str(i)+'.elf')
                self.command(sys.executable,'tools/xhcc.py','--package-root',packages,'variant-gcc','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                self.command(GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry','-o',exe,obj,'--start-group',libm,libc,libgcc,libnosys,'--end-group');values.append(self.execute(exe))
                if i==0:
                    self.assertEqual(self.execute(exe,'math_routes'),1)
                    results={name:self.execute(exe,name) for name in ['local_only','call_only','array_only','memory_routes','double_return']}
                    self.assertEqual(results,dict(local_only=34,call_only=34,array_only=34,memory_routes=90,double_return=34))
            self.assertEqual(values,[5,3]);self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)

    def test_explicit_cpu_feature_requirements_for_raw_assembly(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-features-') as tmp:
            work=Path(tmp);source=work/'feature.c'
            cases=[('cortex-m3',None,'VADD.F32 s0,s0,s1',False),('cortex-m4',None,'VADD.F32 s0,s0,s1',True),('cortex-m4',None,'VADD.F64 d0,d0,d1',False),('cortex-m7','fpv5-d16','VADD.F64 d0,d0,d1',True),('cortex-m7','fpv5-sp-d16','VADD.F64 d0,d0,d1',False),('cortex-m7','fpv5-sp-d16','VCVT.F64.F32 d0,s0',False),('cortex-m4',None,'VLDR d0,[r0]',True),('cortex-m3',None,'SADD16 r0,r0,r1',False),('cortex-m33',None,'SADD16 r0,r0,r1',True)]
            for i,(cpu,fpu,asm,allowed) in enumerate(cases):
                source.write_text('__asm int entry(void){\n'+asm+'\nBX lr\n}\n')
                for mode in ['-c','-S']:
                    out=work/(str(i)+('.o' if mode=='-c' else '.s'));args=[sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','-mcpu='+cpu,'-O0',mode]
                    if fpu:args+=['-mfpu='+fpu,'-mfloat-abi=softfp']
                    p=self.command(*args,source.relative_to(ROOT),'-o',out.relative_to(ROOT),ok=allowed)
                    if not allowed:self.assertIn('capabilities',p.stderr);self.assertFalse(out.exists())

    def test_adr_cbz_cbnz_gnu_boundaries(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-narrow-bytes-') as tmp:
            work=Path(tmp)
            for op,reg,deltas in [('ADR','r3',[0,4,1020]),('CBZ','r4',[0,2,62,64,126]),('CBNZ','r5',[0,2,62,64,126])]:
                for delta in deltas:
                    # Instruction is at offset0, aligned PC=4. Target at4+delta.
                    body=op+' '+reg+', .Ltarget\n.space '+str(delta+2)+'\n.Ltarget:\nBX lr\n'
                    source=work/'case.c';gas=work/'case.s';obj=work/'xh.o';ref=work/'gnu.o'
                    source.write_text('__asm int entry(void){\n'+body+'}\n');gas.write_text('.syntax unified\n.thumb\n.global entry\n.thumb_func\nentry:\n'+body)
                    self.command(sys.executable,'tools/xhcc.py','arm-none-eabi-gcc','--asm-extra','-O0','-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT))
                    self.command(GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m3','-mthumb',gas,'-o',ref)
                    def first(path):
                        with path.open('rb') as f:return ELFFile(f).get_section_by_name('.text').data()[:2]
                    self.assertEqual(first(obj),first(ref),(op,delta))

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
