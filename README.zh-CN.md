# 曦和编译器 · XH Compiler Collection

[English](README.md) · 简体中文

**项目定位**

> 可在 MCU 上运行的跨 OS 模块化编译器集合：多后端交叉编译生成可烧录固件，兼容 make/gcc 语法并接入 make/CC，开发态可由 RTOS harness 调用，通过复制目录添加后端。

曦和编译器（`xhcc`）当前以 WASI Preview1 command 运行，已具备桌面 SD 部署模拟及有界的 make/GCC 兼容接口。外部包按需提供配置、代码生成规则、头文件和运行库资源；复制目录可扩展已有引擎，任意新 ISA 仍可能需要引擎改动。MCU 运行和 RTOS harness 集成是部署方向，NuttX/WAMR 真机内存尚未测量。现代通用链接尚不完整，历史镜像生成接口也不等于已完成从编译到固件烧录的完整流程。

两个独立应用共享一份 `packages/` 目录：

- `xhcc.wasm`：直接编译驱动，不含 make 实现。
- `xhccm.wasm`：同一驱动加有界的进程内 make 子集。编译配方直接调用驱动，不启动 shell 或子进程。

每个应用都包含共享 C 前端、IR 和目标引擎，不采用逐后端子 Wasm。默认 4 MiB 上限仅约束每个应用的 **Wasm 线性内存**，完整 MCU RAM 占用仍需在目标运行时实测。

## 构建与快速使用

除特别说明外，命令均在仓库根目录运行。需要 Python 3、WASI SDK、Binaryen 和 WABT；桌面运行使用 Wasmtime。将工具加入 `PATH`，或配置工具位置：

```sh
# 先将 WASI_SDK_PATH 设置为本机 WASI SDK 目录。
# 若工具不在 PATH 中，另设 WASM_OPT 和 WASM_VALIDATE。
python tools/build.py --app both --target all
python tools/xhcc.py --list-tools
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py armcc --cpu Cortex-M4 -S examples/add.c -o build/add.s
python tools/xhcc.py riscv32-unknown-elf-gcc -c examples/add.c -o build/add-rv.o
python tools/xhcc.py x86_64-linux-gnu-gcc -c examples/add.c -o build/add-x64.o
```

[构建脚本](tools/build.py) 默认使用 `--app both --target all`。生产输出是 `build/wasm/xhcc.wasm` 与 `build/wasm/xhccm.wasm`；中间构建及 map 位于 `build/wasm/dev/all/` 及其 `xhccm/` 子目录。`--app xhcc`、`--app xhccm` 可单独构建。指定单个目标的开发构建位于 `build/wasm/dev/<target>/`，不会替换生产多目标模块。

安装 GNU make 后，根 [Makefile](Makefile) 同样默认构建双应用：`make all`、`make xhcc`、`make xhccm`。可覆盖 `PYTHON`、`WASI_SDK_PATH`。脚本提供 `--sdk`、`--wasm-opt`、`--wasm-validate`，并读取 `WASI_SDK_PATH`、`WASM_OPT`、`WASM_VALIDATE`。其 SDK 和 Windows 工具回退路径是本地开发默认值，需要按本机环境配置。

Python [桌面适配器](tools/xhcc.py) 负责发现包并启动编译器，Wasm 编译器本身不依赖 Python。Wasmtime 不在 `PATH` 时可设置 `WASMTIME` 或传入 `--wasmtime`。也可直接运行：

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
```

## SD 部署与 make

先构建双应用，再部署：

```sh
python tools/deploy_sd.py
python tests/test_sd.py
```

[部署脚本](tools/deploy_sd.py) 默认写入 `../sd/xhcc/`，可用 `--sd <directory>` 覆盖。安装内容包括并列的 `xhcc.wasm`、`xhccm.wasm`，唯一的共享 `packages/` 树、生成的 make 示例和 `installation.json` 清单。以下命令须在**部署目录内**运行，并将此目录映射为 guest 的 `/`：

```sh
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhcc.wasm arm-none-eabi-gcc -c examples/make-demo/first.c -o examples/make-demo/first.o
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo -B CC=riscv32-unknown-elf-gcc CFLAGS=-O0 all
```

使用 make 的 `-C` 时，`XHCC_PACKAGE_ROOT` 应保持为 guest 中的绝对路径。`xhccm make [TOOL] ...` 可通过开头工具名注入默认 `CC`；命令行 `CC=...` 优先于普通 makefile 赋值，显式 `override` 赋值则优先于命令行。配方中明确写出的编译器名直接选择对应工具。[SD 测试](tests/test_sd.py) 通过桌面 WASI 预开放目录验证直接编译、make、多源、增量和切换 `CC`，属于部署模拟，不是 NuttX/WAMR 真机验证。

make 子集支持变量赋值、`override`/`undefine` 指令与替换引用、include、有界条件语句、二十四个文本/控制/诊断函数（含惰性求值的 `if`、局部作用域的 `foreach`、`value`、`flavor`、`findstring`、`join`、`wildcard`、`origin`、`error`、`warning`）、显式/模式/后缀规则、order-only 依赖、自动变量、依赖文件、增量/强制构建以及少量字面文件命令。显式目标与模式专属变量可向前置依赖继承，退出作用域恢复绑定。匹配模式按由宽到窄的特异性应用（stem 由长到短），显式目标变量最后应用；命令行值仍优先，使用 `override` 时除外。普通多个显式目标可共享配方文本，各目标独立检查 mtime，并使用各自的 `$@` 与局部变量。grouped `&:` 目标、多目标变量声明和多个模式目标共享配方仍不支持。`-j1` 为串行执行，其他并行数明确拒绝。不支持 shell、递归 make、环境变量导入/export、通用链接或完整 GNU make。准确语法、命令与限制见 [make 子集文档](docs/make-subset.md)。

显式声明 `.DELETE_ON_ERROR:` 后，未忽略的配方失败会删除新建或 mtime 改变的普通目标文件。默认行为、忽略的失败、未变化文件、目录及 `.PRECIOUS` 精确/`%` 匹配均保留目标；变化检测使用 mtime 快照，不比较内容哈希。

## 目标和语言范围

机器目标的 `-c` 生成标准 ELF 可重定位对象（`ET_REL`）；C 后端生成供另一 C 编译器处理的 C 源码。

| 目标 | 当前范围 | 主要边界 |
| --- | --- | --- |
| ARM | Cortex-M3/M4/M7/M33、Thumb-2、base AAPCS、受检 soft/softfp | 无 hard-float 参数 ABI；M0/M0+/M1/M23 需要 Thumb-1 lowering |
| RV32 | RV32IM、ILP32、软件浮点辅助调用 | 无 RV64 或硬浮点 ABI |
| i386 | ILP32、当前标量 SysV 子集及 xhcc 聚合 wire | 不保证完整 GNU 聚合 ABI |
| AMD64 | LP64、标量 SysV/SSE 和已测变参场景；可选有限小结构体及特定 16 字节 `long`/`double` 双字段结构体 | 完整原生 SysV 聚合互操作尚未实现 |
| C 输出 | IR 转为 C 文本 | 不是 wasm32 代码生成后端 |

详细范围见 [目标与架构边界](docs/targets.md)。编译器本身运行于 Wasm；将输入 C 直接编译为 wasm32/WASI 需要另一目标后端，目前尚未实现。

前端继承 arkcc 的 C99 实现，带 ARMCC 扩展和有限 C11 关键词子集：`_Alignas`、`_Alignof`、`_Static_assert`、`_Generic`、`_Thread_local`、`_Atomic`。这不代表完整 C11。原生 TLS 明确拒绝，原子操作受目标及运行库限制。基本 asm 和 ARMCC asm 函数按目标支持；GNU 扩展 asm 目前接受一个 32 位 `=r` 输出、最多两个 `r/i` 输入及 `memory/cc` clobber，更广泛约束明确拒绝。详见 [功能对照](docs/feature-parity.md) 与 [对象库文档](object/README.md)。

### AMD64 实验性 ABI 计划

`-frecord-sysv-abi` 记录版本化参数/返回计划，`-c --private-object --obj-ir` 可保留计划供检查。`-fuse-sysv-abi-plan` 进一步启用**实验性 caller/callee 消费路径**，支持认证标量，以及有限的自然对齐结构体具名参数与返回值：1–8 字节整数/指针结构体（含递归满足条件的嵌套结构体），单 SSE 片段的纯浮点结构体 `{float}`、`{float, float}`、`{double}`，以及严格自然对齐的 16 字节 LP64 双字段形状 `{long, long}`、`{long, double}`、`{double, long}`、`{double, double}`（`long` 必须为 64 位 `TYPE_LONG`），以及严格自然对齐、32 位字段位于偏移 0 和 4、合并为一个 INTEGER 槽的 8 字节 `{int, float}` / `{float, int}` 形状。两个选项仍默认关闭。use 模式校验一个版本 2 源类型资格认证（`SCALAR_VERIFIED=1`、`SMALL_INTEGER_AGGREGATE_VERIFIED=2`、`SMALL_SSE_AGGREGATE_VERIFIED=4`、`INTEGER_PAIR_AGGREGATE_VERIFIED=8`、`MIXED_PAIR_AGGREGATE_VERIFIED=16` 或 `SINGLE_SLOT_MIXED_AGGREGATE_VERIFIED=32`）、ABI 身份和规则指纹。签名采用所需最高认证，不按位 OR 合并，不能仅凭片段布局猜测资格。这些扩展使用 `abi.plan-int-pair.v1`、`abi.plan-mixed-pair.v1` 和 `abi.plan-small-mixed.v1`。

执行测试覆盖标量整数、浮点、指针、寄存器/栈溢出参数及标量变参，也覆盖有限结构体的参数/返回、GP 寄存器耗尽后的栈传递和指针字段。精确尺寸的 IR 读写保持结构体字节范围，页边界及 canary 测试覆盖 1/2/3/5/6/7 字节场景及栈传递。纯浮点结构体测试覆盖参数/返回、八个 SSE 参数寄存器用尽后的栈传递、GP/SSE 混合及 canary。同一签名可组合合格的整数结构体与 SSE 结构体。认证的 `{int, float}` / `{float, int}` 场景通过恒等传递保留负零及 qNaN payload 位的测试，并覆盖 GPR 边界与 canary；其他未认证的小型混合字段形状仍拒绝。已支持的 16 字节双字段形状使用两个 GP/SSE 片段，任一所需 bank 不足时将整个参数回退到栈，保留未用寄存器供后续参数使用。返回寄存器序号按 bank 独立分配（RAX/RDX 与 XMM0/XMM1），内部结果缓冲仍独立保留。测试覆盖溢栈、尾参数寄存器、多个存活结果缓冲和 canary。覆盖包含 **xhcc caller → Clang callee 与 Clang caller → xhcc callee 双向互操作**。use 模式拒绝 packed 结构体、位域、union、数组、其他大于 8 字节的结构体（包括其他 16 字节形状）、变参聚合实参及其他未支持聚合形状。其余聚合计划仍仅供记录检查，不代表完整原生 SysV 聚合互操作。IR 容器 v2 持久化计划表和独立引用，读取 v1 时按无计划处理。双翻译单元测试还将整数及混合 16 字节双字段形状保存为私有 IR 并重载，经 legacy LTO 和 Clang/LLVM 后在 Unicorn 中执行。合同与测试证据见 [ABI 计划文档](docs/abi-plan-ir-v2.md)。

## 命令行兼容范围

现代驱动支持 `-E/-S/-c`、仅语法检查、C 语言/标准选择、O0/O1/O2、目标/CPU/ISA/ABI 选项、include 路径与宏、依赖生成、诊断、有界 response 文件和多源顺序编译。未知或语义未实现的参数明确报错。

`arm-none-eabi-gcc`、`x86_64-linux-gnu-gcc`、`armcc` 等工具别名选择参数接口，不代表完整实现 GNU GCC 或商用 ARM 编译器。尚不支持通用链接、DWARF ELF 输出和全部 GCC 参数。`--ark-compat` 保留历史私有对象、IR/LTO、IPO、partial link、scatter、MDK 导入等接口；这些不表示 GNU linker 或 GCC LTO 兼容。

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm --ark-compat --help
```

## 外部包和后端开发

[包清单协议](docs/package-protocol.md) 注册工具别名、目标、CPU、ABI 合同、头文件、选项映射和规则资源。现有引擎实际消费外部模板、语义选择、编码、CPU/目标布局、ABI 数据、对象描述和有界重定位规则。规则变体可在编译器模块 SHA-256 不变时改变发射指令与实际执行行为。

五个引擎均已有真实外部规则消费者。部分复杂选择、ABI lowering、优化和布局算法仍是应用中的 C 实现。新包可以配置已有引擎；任意新 ISA 可能需要扩展共享合同和引擎并重建。详见 [外置化状态](docs/backend-externalization.md) 与仓库 [后端移植指南](docs/skills/port-backend/SKILL.md)。

## 内存与验证

默认构建声明一个非共享 wasm32 线性内存，initial = maximum = **4 MiB**，其中包括静态数据、C 栈、arena 和 libc 堆。运行时引擎结构、字节码、宿主栈、WASI 缓冲区和文件系统缓存均增加完整任务 RAM。该上限和 Wasmtime 进程 RSS 都不能证明 4 MiB MCU 部署达标，仍需 NuttX/WAMR 配置及真机峰值内存测量。

默认 arena 为 1 MiB，源文件、预处理、汇编、机器码缓冲与 include 深度有明确上限，较大输入可能触发超限诊断。外部资源按需加载。构建时通过 Binaryen 降级和 WABT 验证约束 Wasm 特性；存储、map 与 RAM 的区别见 [资源测量文档](docs/resource-budget.md)。

```sh
python tools/test.py
python tests/test_sd.py
python tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
```

SD 测试前须完成构建与部署。集成测试使用 GNU ARM 工具、Unicorn、pyelftools；相关异编译器混链检查还需合适的 Clang/LLVM。缺少参考工具时部分检查会跳过，跳过不构成互操作证据。回归入口包含真实编译、ELF 检查、执行、规则变体、阴性场景和标量 ABI 计划。资源报告描述生成当时的构建产物，不是项目固定规格。

## 许可与实现来源

初始 C 前端、IR 和 ARM 发射器源自 MIT 授权的 arkcc，见 [来源声明](NOTICE.md) 与保留的 [MIT 许可](LICENSE)。目标扩展依据公开 ISA、ABI 和对象格式规范独立实现，通过外部工具验证兼容性，不将 GPL 编译器实现代码复制混入本项目。
