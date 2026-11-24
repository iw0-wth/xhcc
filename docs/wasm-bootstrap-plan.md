# WASM 后端与自举准备计划

当前结论：xhcc 的核心是 C，可作为 WASI 程序运行；**目前没有 wasm32 输出后端，也没有完成 WASM 自举**。宿主 Clang 把编译器编成 WASM，与 xhcc 自己生成 WASM 是两件分别验收的事。本计划描述以后要实现的闭环，不用完成百分比代替证据。

文中依据均为本地代码。构建可同时保留有 Python 和无 Python 两条路径；Python 测试、部署、报告工具可以继续存在。Python 解释器、Python 包或 Python 生成器不得成为核心编译运行及无 Python 必要构建步骤的依赖。

## 现有边界与代码依据

| 范围 | 已有实现与边界 |
| --- | --- |
| 可运行载体 | [构建脚本](../tools/build.py)用宿主 WASI SDK Clang 的 `--target=wasm32-wasip1` 编译 C，再经 Binaryen lowering 和 WABT 验证；这不是 xhcc 的输出目标。 |
| 输出目标注册 | [registry](../src/backend/registry.c)注册 arm、riscv32、x86、x86_64、c；前端 stage 的元数据表也是这五种，没有 wasm32。 |
| 后端接口 | [ArkBackend](../include/backend.h)的主要入口是 IR→汇编文本、汇编文本→字节以及 link。WASM 二进制/对象接入需要设计适配，不能把这些接口的存在视为已有 WASM 发射器。 |
| 目标数据模型 | [target descriptor 实现](../src/backend/rules/target_rules.c)验证已有五个 engine 的固定 ABI/宽度/IR 表示，未知 engine 被拒绝；[契约说明](target-descriptor-v1.md)也明确元数据不会新增 ISA/ABI。 |
| ABI | [有限 ABI 关系](../include/xh_abi.h)与[参数位置计划](../include/xh_abiplan.h)是有界机制；后者当前的持久化标量消费是 opt-in AMD64 路径，不能自动成为 WASM 调用约定。 |
| 编译管线 | [compiler](../src/core/compiler.c)执行预处理、词法、解析、语义、IR、发射、汇编和写出；[pipeline](../include/pipeline.h)统一 AST/IR pass 的 full/lite 门控；[IR](../include/ir.h)包含机器后端的 32 位字、64 位值拆分、复数和 ABI 标记。 |
| 对象与链接 | [对象说明](../object/README.md)、[object descriptor](../include/xh_object_rules.h)及[ELF 链接器](../src/link/elf_link.c)处理既有 ELF/ARKOBJ 路径；ELF class、machine 和 relocation 映射不等于 WASM module/object 支持。 |
| 外部资源 | [package loader](../src/packages.c)检查已有目标/ABI及有限 capability 列表；复制包资源可以复用 contract 的组织方式，但新增 wasm32 ISA 仍须 native engine C 实现与注册、验证和驱动集成。单添一个 INI 或 rules 文件不能完成。 |

## Stage0：已有宿主工具链构建四个 WASM 应用

Stage0 的编译器来源是宿主 WASI SDK Clang。四个应用为 `xhcc.wasm`（编译与链接）、`xhccm.wasm`（内置 make）、`xhccn.wasm`（去掉链接器的编译入口）、`xhlinker.wasm`（独立链接器）。它们的当前实现仍只输出已有目标，不输出 WASM。

有 Python 路径保留：从仓库根目录运行 `python tools/build.py --app all-apps --sdk <WASI_SDK_PATH>`，或使用[主 Makefile](../Makefile)的既有入口。[脚本](../tools/build.py)的实际差异包括：xhccm 对 driver 定义 `XH_DRIVER_NO_MAIN=1`；xhccn 的 driver、compiler、IPO 单元定义 `XH_WITH_LINKER=0` 并排除 link 源；xhlinker 只链接其入口、ELF reader/linker、ARM relocation、ld-script、relocation VM 和文件系统单元。它不是四次简单替换输出文件名。

无 Python 路径从仓库根目录运行 `make -f Makefile.nopy all`，或选择 `xhcc`、`xhccm`、`xhccn`、`xhlinker` 单应用目标，见 [Makefile.nopy](../Makefile.nopy)。它由 GNU make 直接调用 Clang、Binaryen `wasm-opt` 和 WABT `wasm-validate`；必要构建不能先调用 Python 生成源清单、响应文件、头文件或规则文件。可覆盖 `WASI_SDK_PATH`、`WASI_CC`、`WASM_OPT`、`WASM_VALIDATE`、`MEMORY_BYTES`（默认 4194304）；Windows 使用 MSYS2 GNU make、`WINDOWS_BASH` 和 POSIX `mkdir/cp/mv`，运行库搜索路径如需配置可用 `DLL_DIR`。源文件由 GNU make 的递归 wildcard 直接发现。模块及 map 写入 `build/nopy/<app>/`，成功模块复制到 `build/wasm/<app>.wasm`。具体选项以构建文件为准。两条路径都应显式固定 SDK/tool 版本、C 宏、源清单、头文件快照和 lowering 选项，避免把“可选 Python”误写成“禁止 Python”。

当前[脚本](../tools/build.py)使用 C99、`-Oz`、强制包含[xh_small](../include/xh_small.h)，禁 bulk memory/sign extension/nontrapping float conversion/SIMD/atomics；链接栈 128 KiB，默认固定初始/最大线性内存 4 MiB。它再做 lowering 并禁 reference types、multivalue 等扩展进行验证。未来后端须明确采用何种同等可运行子集，不能仅凭现代桌面运行时可加载就通过 MCU/WASM MVP 验收。

本次协作的 Stage0 实测记录（由 targets 代理执行）：命令 `D:/msys64/usr/bin/make.exe -f Makefile.nopy all DLL_DIR=D:/miniconda3/Library/bin` 已完成四应用的直接 Clang 编译、lowering 与 validation；后续改动后的增量构建再次通过。`DLL_DIR` 在该机器仅提供 SDK 所需动态库的搜索目录，构建没有调用 Python 解释器；这份记录不等同于已在移除整个 Python 安装的隔离机器验收。四模块的最大线性内存均为 4 MiB。四应用执行测试覆盖 xhccn/xhccm 编译、xhcc/xhccm/独立 linker 脚本路径的 ELF/bin 一致性及 xhccn 拒绝链接。此记录证明现有宿主构建与运行路径，不证明 xhcc 已具 WASM 输出或自举能力。

Stage0 完整验收（包含超出上述实测的隔离与复现条件）：

- [ ] 两条构建路径在独立空输出目录构建四个应用，记录工具版本、命令、退出码和四个产物摘要；不得拿历史产物冒充本次结果。
- [ ] 无 Python 路径在 PATH 中没有 Python、且不读取 Python 生成中间件的环境完成；记录直接子进程调用和所需工具清单。
- [ ] 四个模块通过约定 WASM 特性验证，并在项目实际 WASI Preview1 运行环境执行各自的编译/make/独立链接最小用例。
- [ ] 核对两条路径的源文件选择和逐单元宏一致，尤其 xhccn/ xhccm/ xhlinker 的差异；差异必须说明用途。

## 当前前端能否编译自身 C：已发现的具体缺口

答案尚不能肯定。C99 子集、由 Clang 成功构建、已有某个头文件、解析一个单元成功，都不能证明能正确编译自身全部 C。下面是静态核对发现的实际条目，不是已经执行过的全量自编译失败清单。

| 条目 | 本地事实 | 自编译前必须验证/补齐 |
| --- | --- | --- |
| 输入容量 | [xh_small](../include/xh_small.h)默认 `ARK_SRC_BUF_SIZE=32*1024`、预处理输出 64 KiB、include buffer 16 KiB、嵌套深度 8；[compiler](../src/core/compiler.c)会拒绝超长源文件。实际[ARM 后端](../src/backend/arm/backend_arm.c)约 645 KB、[irgen](../src/ir/irgen.c)约 335 KB、[parser](../src/frontend/c/parser.c)约 194 KB，远超默认源缓冲。 | 对自编译 profile 逐单元测量原文、预处理展开、IR、发射和链接峰值；实现可行的流式/动态缓冲或明确提高预算并在设备上验证，不能忽略默认限制。 |
| 系统头缺失 | [packages](../src/packages.c)直接包含 `dirent.h`、`sys/stat.h`；[make](../src/make/make.c)还需要 `unistd.h`；[文件系统](../src/platform/wasi/flash_fs_host.c)需要 `fcntl.h`。现有 target include 资源没有这些文件。 | 提供与目标 libc/WASI ABI 配套的 bootstrap sysroot 或替换平台适配层；列出目录枚举、stat、chdir/getcwd、open/close 的实际可用契约。不能认为现有 package headers 已覆盖自身构建。 |
| stdio 声明缺口 | 已有[stdio](../packages/c/include/stdio.h)，但没有文件系统单元用到的 `ferror`、`fdopen`、`rename`、`remove` 声明，也没有 package loader 用到的 `feof` 声明。 | 补全正确的声明及实现/适配，验证 EOF/error、独占创建、临时文件替换及失败清理。 |
| errno 与平台 ABI | 已有[errno](../packages/c/include/errno.h)，但缺文件系统用到的 `ENAMETOOLONG`；其数值注明为 Ark 自有值，`errno` 声明为普通外部整数。 | 按选定 WASI libc 的 errno 存取和编号建 contract，不能与 SDK libc 混搭后默认为 ABI 一致。 |
| 时间与布局 | [time](../packages/c/include/time.h)只声明宿主实现，定义 `time_t` 为 long；[platform](../src/platform/wasi/platform_host.c)实际调用 time/localtime。[stddef](../packages/c/include/stddef.h)给出 32 位 size_t/ptrdiff_t。 | 审计目标 sysroot 的 time_t、struct tm、size_t、ptrdiff_t、FILE 等布局；匹配实现或提供自身 runtime。32 位 pointer 不足以推出所有库类型的布局。 |
| 变参 | 已有[stdarg](../packages/c/include/stdarg.h)实现 ARM/RV/i386、AMD64 和 C 后端内部槽位约定，没有 WASM C ABI 专用契约；日志和诊断实际用 vfprintf/vsnprintf 一类接口。 | 定义 WASM `va_list`、默认提升、对齐、aggregate 传递及与 libc 的互操作，做跨对象调用执行测试。 |
| long double | [类型枚举](../include/type.h)没有独立 long double；[lexer](../src/frontend/c/lexer.c)明确将 L 后缀浮点常量按 double 表示，[parser](../src/frontend/c/parser.c)也没有独立扩展浮点类型建模。已有[stdlib](../packages/c/include/stdlib.h)却声明 strtold。 | 不得声称支持目标 ABI 的扩展精度；若 bootstrap 选定源码不需要它，应固定支持子集并拒绝依赖扩展精度的路径；若需要，则实现类型、常量、运算和 ABI。 |
| long double complex | 已有[complex](../packages/c/include/complex.h)包含 `_Complex long double`/CMPLXL，而[类型实现](../src/frontend/c/type.c)的 complex 基类仅为 float/double。普通 float/double complex 已有部分实现，不能笼统写所有 complex 都不支持；x86/x64 还会显式拒绝复数操作。 | 把 long double complex 视为未具备正确类型/ABI支持的条目；逐源码与头展开确定是否进入 bootstrap 闭包，不能用声明可解析代替语义支持。 |
| 其余 C 语义闭包 | [parser](../src/frontend/c/parser.c)已有 VLA、属性、部分 C11 等代码，[IR](../include/ir.h)还携带函数指针、aggregate、64 位/浮点和 ABI 信息。 | 对精确源码清单逐 TU 通过 preprocess→parse→sema→IR，再用实际执行覆盖递归、函数指针表、struct 返回/按值传参、switch/goto、64 位、浮点、字符串/静态初始化。当前没有此清单的全通过证据。 |

不能直接把 SDK Clang 的整套系统头交给该前端就认定可用：必须检查 builtin、attributes、预定义宏、类型宽度和 libc ABI。也不能以现有 package headers 名义遮住上述声明缺口。应保存逐 TU 的首次失败位置、诊断和修复后结果；尚未运行的检查标记为待验收。

## Stage1：未来 wasm32 输出 native engine

本计划把“宿主 Clang 构建的、带新 WASM 后端的种子编译器”称为 stage0+。Stage1 的门槛是这个 C 实现的 native engine 能从 C/IR 生成可运行 wasm32 模块；不是把 stage0 模块重新打包，也不是借 C 输出再让外部 Clang 生成 WASM。

### 指令与结构化控制流

- [ ] 在 registry、编译开关、CLI/package target 白名单、target descriptor 验证和 driver 中增加真实 wasm32 engine。资源可以复制已有版本化 contract 的形式，但需要新增本机 C 发射/序列化逻辑。
- [ ] 为 IR 的整数/浮点运算、符号与无符号比较、扩展/截断/转换、load/store、call/return、locals 与临时值制定 lowering 表；特别处理当前两字 64 位值到 WASM i64/f64 的映射和浮点转换 trap 语义。
- [ ] 把任意标签/跳转 CFG 转为合法 `block`/`loop`/`if`/`br`/`br_if`/`br_table`，支持 switch、break/continue、goto 与不可约 CFG；可先采用 dispatcher，必须测量代码和运行成本。不能逐标签照搬机器跳转指令。
- [ ] 在所有分支合流点保证 WASM operand stack 类型/高度正确，定义 unreachable 与 return 路径；输出验证错误应定位到 IR/源位置。
- [ ] 提供直接调用与带签名检查的 `call_indirect`、table 元素初始化和函数指针表示；不能把线性内存地址当作函数索引使用。

### 数据模型、内存与调用约定

- [ ] 固定 wasm32 little-endian、32 位 pointer/int/long/size_t/ptrdiff_t、64 位 long long、f32/f64 的初始模型，并逐项验证 alignof、struct/union/bitfield、数组、packed 和全局初始化；long double 的策略须显式定义，不能静默冒充完整支持。
- [ ] 规划 linear memory 的静态 data/rodata、零初始化 BSS、C stack、heap、空指针保留区及 stack/heap 碰撞检查；定义 memory 页数、增长策略、固定内存配置、栈全局和 `_start` 初始化顺序。
- [ ] 定义固定参数、结果、sret/aggregate、变参和间接调用 ABI，并与 bootstrap runtime/libc 一致；新增必要 IR ABI表示，不能把现有 ARM/AMD64 计划直接命名为 WASM ABI。
- [ ] 实现 memcpy/memmove/memset、分配器、整数/浮点辅助函数等实际闭包；禁用的 bulk-memory 等指令以受支持指令序列或 C runtime 实现。

### WASI Preview1 与运行环境

- [ ] 为 `wasi_snapshot_preview1` 制定**实际 imports 清单**及精确类型，例如启动所需 args/environ、`proc_exit`，I/O 的 `fd_read`/`fd_write`/`fd_close`/`fd_seek`、路径与文件元数据、目录枚举、`clock_time_get`；随机数等只在实际 runtime 闭包需要时加入。此处是候选需求，不声称所有模块当前都导入这些项。
- [ ] 清单从最终模块的 import section 与实际 libc/平台适配实现核对；Preview1 的 fd/iovec/stat/errno、权限和 preopen 路径使用约定必须固定，并在实际 host 中验证。
- [ ] 明确 `_start`/导出、内存/table/globals 的可见性及失败退出码；文件读写、目录发现、时间与 stdout/stderr 可以运行且不依赖 Python host glue。

### 对象、符号重定位与 module link

- [ ] 选择并文档化 link 路线：支持标准 WASM relocatable object（linking/reloc custom sections）与工具互操作，或实现有版本的私有中间对象加自身 module linker；私有路线不得冒称可供 wasm-ld 消费。
- [ ] 序列化 WASM 类型、import/function/table/memory/global/export/start/element/code/data 等 section，正确计算长度和 LEB128；最终模块必须被独立 validator 接受。
- [ ] 定义符号绑定（local/static/global/weak/undefined）、函数/类型/全局/table/data 索引空间与签名去重，支持跨 TU 函数调用、函数地址表、字符串/数据地址、BSS、addend 与重复/未定义符号报错。
- [ ] 重定位必须同时处理 index 重编号、线性内存 data placement 和可变长 LEB patch/section 长度变化；不能复用 ELF 的“固定字节位置写地址”假设。
- [ ] 最终 module link 合并各 TU 与 runtime、统一 imports、设置入口和内存/table布局；对 archive 提取、初始化函数、dead stripping 的首版支持范围作明确决定。现有[ELF linker](../src/link/elf_link.c)不承担这个能力。
- [ ] 用多 TU C 夹具覆盖互调、静态同名、强弱定义、数据与函数指针、relocation addend，验证链接失败不留下成功产物；标准路线另做与宿主工具的双向互操作。

Stage1 通过标准是生成模块的验证与执行结果均正确、缺失能力有明确诊断，并记录真实资源消耗；仅生成 WASM magic、能列出一个 wasm32 target 或加载资源文件均不算通过。

## Stage2：用 stage0+ 编译自身 C，产出 stage1 编译器

这里 stage1 是由 xhcc 新后端产出的编译器二进制：stage0+ 在 WASI host 中读取自己的 C 源码、bootstrap headers、版本化 target资源和 C runtime，经新 wasm32 engine 与 module linker 生成四个可运行应用。Stage1 开发门槛和本阶段产物名称采用这一约定，验收日志必须记录具体模块摘要以消除歧义。

- [ ] 固定自身 C/头文件/资源/runtime 的完整 source manifest、宏、include 顺序、profile 和工具版本；包括新 WASM 后端及 module linker 自己的 C，而非只编译一个 hello world 或前端子集。
- [ ] 完成上表前端与系统头缺口，保存每 TU 的诊断、内存峰值、对象摘要及链接日志；任何未实现 builtin/ABI 不得隐式回退宿主 Clang。
- [ ] stage0+ 执行必要的全部编译与链接，不调用宿主 Clang/wasm-ld/Python 代做代码生成；若借外部 runtime 预编译对象，须列出来源与摘要并把结果标为“带种子 runtime”的闭环边界，直到 runtime 也进入源码构建。
- [ ] 四个产物通过 validator、WASI imports 白名单和实际 host 执行，执行编译、make、分离编译与链接的回归夹具；验证其产生的程序行为。
- [ ] 主机测试脚本允许使用 Python。它是观测和测试驱动，不因测试工具使用 Python 否认自举；必要编译、模块链接和运行链不得依赖它。

## Stage3：同一份 source 再编译，验证语义与确定性

- [ ] 用 stage1 编译**相同 manifest、头文件、宏、资源与 runtime source**得到 stage2，再用 stage2 重编得到 stage3；保存每轮编译器、对象、module imports 和产物摘要。不要把“重新运行宿主 build”算作此阶段。
- [ ] 在同一输入语料、优化/profile、失败用例和 WASI host 配置下比较 stage1/stage2/stage3 的退出码、诊断类别及关键位置、预处理/IR契约、生成程序输出与可观察副作用。先证明语义一致，再讨论二进制固定点。
- [ ] 在同 source 与确定性配置下要求 stage2/stage3 module 逐字节一致；独立重建再现相同结果。类型/函数/符号排序、LEB编码、数据布局与输出迭代顺序须稳定。
- [ ] 固定或显式输入日期、时间、绝对路径、目录枚举顺序、临时名、随机种子及环境变量；[platform](../src/platform/wasi/platform_host.c)会读取本地时间，[预处理器](../src/frontend/c/preproc.c)支持日期/时间宏，必须验证其确定性策略。
- [ ] 摘要比较对象限于自举闭包的编译器模块和必要构建产物。README、测试报告、header 审计/生成工具或 Python 工具自身不属于必须相等的编译器产物；但参与编译的头文件**内容**必须固定并纳入输入摘要，不能把头工具排除误用成忽略头文件变化。
- [ ] 验收附完整可重跑命令和失败判断，报告尚未闭合的 runtime/工具种子。所有条目通过后才声明对应边界内完成 WASM 自举；此前保持“规划/待验收”状态。

本次文档只做代码阅读与计划整理，不执行主构建，不修改四应用源码，不提交或推送。上述 checkbox 是未来验收项，不代表本次已运行或已通过。
