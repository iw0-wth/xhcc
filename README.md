# xhcc

面向 MCU / NuttX 等 RTOS 的多目标 C 交叉编译器。当前采用一个共享 Wasm 主程序，加外部配置、编码规则和运行库资源包；不使用后端子 Wasm。编译任务 RAM 目标约 4 MiB，允许适度超出但尽量控制；文件/Flash 体积独立统计。配套 `xhccm` 为含内置make子集的独立应用；`xhcc` 不含make代码，二者并列并共用包目录。

## 构建与使用

需要 Python 3、WASI SDK、Binaryen、WABT。运行使用 Wasmtime；Python 只是桌面宿主适配工具，主编译器不依赖 Python。

```powershell
D:\miniconda3\python.exe tools/build.py
D:\miniconda3\python.exe tools/xhcc.py --list-tools
D:\miniconda3\python.exe tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
D:\miniconda3\python.exe tools/xhcc.py armcc --cpu Cortex-M4 -S examples/add.c -o build/add.s
D:\miniconda3\python.exe tools/xhcc.py riscv32-unknown-elf-gcc -c examples/add.c -o build/add-rv.o
D:\miniconda3\python.exe tools/xhcc.py x86_64-linux-gnu-gcc -c examples/add.c -o build/add-x64.o
```

构建生成 `build/wasm/xhcc.wasm`，同一份程序按工具名选择 ARM、RV32、i386、AMD64 或 C 后端。也可独立调用：

```powershell
& 'C:\Program Files\Wasmtime\bin\wasmtime.exe' run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
```

工具位置可通过 `--sdk` / `WASI_SDK_PATH`、`--wasm-opt` / `WASM_OPT`、`--wasm-validate` / `WASM_VALIDATE` 设置。Windows 默认使用本机 D 盘安装；其他机器须配置。

## 内置 make 与模拟 SD

整体 `Makefile` 默认构建 `xhcc.wasm`、`xhccm.wasm` 两个应用；也可 `make xhcc` / `make xhccm` 单独构建。桌面可用 `make PYTHON=D:/miniconda3/python.exe`，或直接运行构建脚本。

```powershell
& 'C:\Program Files\Wasmtime\bin\wasmtime.exe' run --dir=. --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo all
# 若Makefile用外部CC，注入默认工具或CC变量：
# xhccm make riscv32-unknown-elf-gcc CFLAGS=-O0 all
# xhccm make CC=riscv32-unknown-elf-gcc CFLAGS=-O0 all
D:\miniconda3\python.exe tools/deploy_sd.py
D:\miniconda3\python.exe tests/test_sd.py
```

以上第一条命令从部署目录运行。部署目录默认 `../sd/xhcc/`，里面并列放两份应用和唯一 `packages/` 资源目录。测试以SD根作为WASI预开放目录，验证直接编译、make两源、mtime增量以及CC注入切换RV32。明文写在配方中的编译器名直接选择对应包，不受默认CC覆盖。

make采用有界子集，不启动shell。变量赋值、include、条件语句、十个文本函数、变量替换引用、显式/模式/后缀规则、order-only依赖、自动变量、增量/强制构建和有限文件命令已支持；-j1为串行兼容，大于1明确拒绝。其余GNU make函数、shell脚本和通用链接尚未实现。完整范围见 `docs/make-subset.md`。

## 语言和输出

继承 arkcc 的 C99 实现、C11 六关键词子集（`_Alignas/_Alignof/_Static_assert/_Generic/_Thread_local/_Atomic`）、ARMCC 扩展和可选优化/诊断。C11 子集不等于完整 C11；机器目标真实 TLS 尚未实现并明确拒绝，原子操作仍有目标和运行时边界。

基本 asm 和 ARMCC asm 函数按目标支持；GNU 扩展 asm 支持一个32位 `=r` 输出、最多两个 `r/i` 输入及 `memory/cc` clobber，实际求值和回写。通用 GCC 约束分配、64位/内存操作数、多输出等尚未实现，会拒绝。

机器目标 `-c` 生成标准 ELF ET_REL，不是私有 ARKOBJ 改后缀。ARM M3/M4/M7/M33已执行验证；M0/M0+/M1/M23需要Thumb-1指令选择，当前不假装支持。soft/softfp有CPU/FPU检查，hard-float调用ABI未实现。RV32为ILP32，i386为ILP32，AMD64为LP64。C后端输出C文本，不生成原生对象。

`-frecord-sysv-abi` 可在 AMD64 前端记录版本化参数/返回计划，`-c --private-object --obj-ir` 可持久化检查；它不会启用原生 GCC 聚合 ABI 执行。IR v2 保留计划表和独立索引，读取 v1 时默认无计划。

完整对照和限制见 `docs/feature-parity.md`、`object/README.md`。目标名称、ARM架构支持范围和C输出与wasm32目标的区别见 `docs/targets.md`。ARM包名为 `arm`，Cortex-M3仅是默认CPU。

## GCC/armcc 参数

提供 `-E/-S/-c`、`-fsyntax-only`、`-x c`、`-std=c99/c11/gnu99/gnu11`、O0/O1/O2、目标/CPU/ISA/ABI选项、`-I/-iquote/-isystem/-nostdinc`、`-D/-U/-include`、`-M/-MM/-MD/-MMD/-MF/-MT/-MQ/-MP`、`-dM/-P`、freestanding/hosted模式、警告控制、诊断格式、配置和原有优化开关。支持有界 `@response` 和多源顺序编译。

未知或语义未实现的参数明确报错。现代入口尚不支持通用链接、DWARF ELF输出和全部GCC参数。通过 `--ark-compat` 保留原Ark CLI的私有对象、IPO、IR/LTO、partial link、scatter、MDK导入等入口；这些不是GNU链接器或GCC LTO兼容实现。

```powershell
& 'C:\Program Files\Wasmtime\bin\wasmtime.exe' run --dir=. build/wasm/xhcc.wasm --ark-compat --help
```

## 配置包与规则引擎

`packages/<id>/package.ini` v3提供工具别名、目标、CPU、ABI、include、参数方言及可选规则文件。复制目录即可注册新工具/目标配置，不需逐包新增主入口分支。`options.ini`描述flag/value/joined参数映射，可独立模拟编译器参数名称。

RV32使用共享字段编码VM，R/I/S/B/U/J布局在外部 `encoding.rules`；标量32位算术、位运算、移位、比较及有符号/无符号除法取模的IR选择与寄存器模板在 `selection.rules`，v2每条规则最多8步且兼容v1。规则按需读取有界缓存。当前是**后端数据化第一阶段**：其余IR选择、ABI、通用寄存器分配、汇编助记符及重定位仍有C实现，任意全新ISA还不能只写目标名就实现。外部规则变体测试会在主程序SHA256不变时改变实际指令和执行结果。配置版本不匹配或规则越界必须失败。

ARM、i386、AMD64和C也使用同一个XHTEMPLATE引擎读取外部发射模板，ARM额外迁出227条Thumb编码规则，x86迁出opcode/REP/REX表，C迁出ABI契约和帧/栈政策。CPU名称/别名/能力与目标数据布局也由cpus.rules和target.rules提供，ARM实际发射按外部指令特性需求检查SP/DP/DSP能力（现代-c/-S均执行），主前端按现有引擎契约消费并验证。所有目标都已有真实包规则路径，但复杂ABI、优化、部分选择及剩余重定位算法仍编译在主程序中。详细清单见 `docs/backend-externalization.md`，不能将当前状态等同于全部后端算法完全外置。

外部资源按需读取；取消的子Wasm/共享stage实验不属于生产构建。协议与映射格式见 `docs/package-protocol.md`。

## RAM 与 RTOS

当前默认单个非共享 wasm32 linear memory initial=max=4 MiB，含arena、C栈、静态区和libc堆。整项任务还包括Wasm字节码、解释器数据、宿主栈和文件系统开销，所以不能用4 MiB linear memory宣称整机总占用达标。NuttX/WAMR待实际配置和真机测量。

默认arena1MiB；源32KiB、预处理64KiB、汇编128KiB、机器码32KiB、每层include16KiB/8层。大工程会显式超限，应按测量调整。ARM liveness工作区已按需分配，普通任务不固定占其约1.5MiB。

Binaryen进行Oz与指令降级，WABT关闭相关扩展严格验证，Wasm仅使用WASI Preview1文件/标准流/参数等接口。Wasmtime进程RSS不是MCU内存证明。

## 验证与来源

```powershell
D:\miniconda3\python.exe tools/test.py
```

执行测试需GNU ARM工具链、Unicorn、pyelftools（可安装到build/test-deps）。测试包括GNU ARM混合链接，RV32/i386/AMD64标准ELF双布局执行，LP64、CPU/softfp、内联汇编、配置包和外部规则。若无full-target clang，额外LLVM异编译器混链测试明确跳过。

基于MIT授权arkcc，来源见 `NOTICE.md`，许可保留于根目录 `LICENSE`。源码已整理为xhcc自有部件，无vendor目录。
