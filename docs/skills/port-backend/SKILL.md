---
name: port-backend
description: 为 xhcc 移植或扩展后端配置包、共享规则契约及必要的引擎支持，并验证标准目标文件、真实执行、资源预算与四应用 SD 部署。适用于已有 ISA 的包变体和需要诚实界定范围的新 ISA 移植。
---

# xhcc 后端移植

本 skill 是仓库文档，不安装到全局 profile。执行时从本文件定位仓库：`../../..` 是 xhcc 根目录。下列所有资源链接均相对此 skill 所在目录，不依赖仓库之外的指南或脚本。命令在 xhcc 根目录执行；工具链可安装在外部，其路径是可覆盖的环境参数。

## 1. 先读当前合同和源码

不要把本文快照当作能力清单的最终权威。先阅读并记录当前版本：

- [包协议 v3](../../package-protocol.md)、[架构](../../architecture.md)、[后端边界审计](../../backend-engine-boundary.md)、[外置状态](../../backend-externalization.md) 和 [目标清单](../../targets.md)。
- 最接近的包清单：[ARM](../../../packages/arm/package.ini)、[RV32](../../../packages/riscv32/package.ini)、[i386](../../../packages/x86/package.ini)、[AMD64](../../../packages/x86_64/package.ini)、[C 输出](../../../packages/c/package.ini)。逐个读取实际规则文件，不仅看字段名。
- [模板接口](../../../include/xh_template.h)、[语义选择接口](../../../include/xh_semantic.h) 与 [语义选择合同](../../semantic-selector-v1.md)、[编码/标量选择接口](../../../include/xh_rules.h)。
- [目标布局接口](../../../include/xh_target_rules.h)、[目标描述合同](../../target-descriptor-v1.md)、[CPU 接口](../../../include/cpu.h)、[ABI 决策接口](../../../include/xh_abi.h)、[IR 计划与标量使用合同](../../abi-plan-ir-v2.md)。
- [对象描述接口](../../../include/xh_object_rules.h)、[重定位接口](../../../include/xh_relocation.h)、[重定位 VM 文档](../../relocation-vm-v1.md)。
- [构建入口](../../../Makefile)、[构建脚本](../../../tools/build.py)、[回归入口](../../../tools/test.py)、[make 子集](../../make-subset.md)、[SD 安装脚本](../../../tools/deploy_sd.py)、[资源预算](../../resource-budget.md)。

检查生产消费者，而不是仅确认 parser 存在。在 [引擎注册](../../../src/backend/registry.c)、[核心编译流程](../../../src/core/compiler.c)、[包加载](../../../src/packages.c) 及对应后端中追踪每个字段实际如何影响输出。若文件位置变化，先搜索并读取新位置，再更新引用；不要凭旧文件名臆造接口。

## 2. 决定移植级别和验收范围

先写一份可检查的范围说明，列出 ISA、端序、pointer/long/int 宽度、浮点、ABI、对象格式、汇编方言、运行库和支持的 IR 家族。

**已有引擎的配置包变体**：仅当现有引擎、IR wire、布局验证、寄存器/操作数和 ABI 合同能表示需求时，才允许只增加包。CPU 自定义别名、预定义宏、编码字段、有限语义选择、模板和已有 operand shape 的指令名称映射可以复用已有机制。当前 [RV 发射规则](../../rv-emission-rules.md) 的 `isa.mnemonics.r` 将 18 个 R-format mnemonic 映射到外部 encoding key/`rr_r`；复制包新增 `plus R_ADD rr_r` 已在不变主程序下实际返回 42。这是已有引擎可扩展映射的真实能力，不应写成“不能增加 mnemonic”；另有六条 immediate 和三条 shift-immediate 使用外部 `rr_i`/`rr_shamt` 映射、signed12/unsigned5 字段约束；不变主程序下新增 `newplusimm` alias 已实际返回 42。五 load、三 store、六 branch 的 `rd_mem`/`rs_mem`/`rs_rs_label` 也已外部映射，14 个编码有独立 oracle，新增 load/store/branch aliases 同主程序执行返回 42。既有这些正常指令的名称映射覆盖不能扩张为所有机器 grammar：pseudo lowering、system constants、符号/标签处理等仍有 native 机制。它们仍受已有 operand grammar、目标 ISA 与 ABI 合同约束，不是任意新 ISA；不为这次映射迁移虚构新的 manifest capability。

**新 ISA 或超出现有合同的 ABI**：当前注册、汇编 grammar、lowering、wire 和布局约束并非任意 ISA 插件系统。需要扩展共享描述/VM 合同或增加引擎实现时，明确列出主程序改动与重建要求。不要声称仅放一个目录即可支持任意 ISA，也不要为了这个说法把公共 C 前端复制到包中。

遇到难以搬出的目标专属 C 分支，定位其输入事实、选择政策、结果及副作用，再判断：

1. 目标无关的类型/图遍历、布局计算、checked arithmetic、ELF 序列化保留共享实现。
2. 目标决定的寄存器 bank、ABI 分类阈值、指令形状、编码和补丁公式应尝试用明确记录表达。
3. 现有 schema 无法表达时，改进有界共享合同及其消费者，并加入版本、预算、错误和阴性测试；不要假装把模板文本搬走就搬走了决策。
4. 暂未覆盖的分支在结果中逐项报告。不要以规则数量或目录位置代替完成度。

依据公开 ISA/ABI/ELF 文档自行实现逻辑，记录规范来源和边界。不得从 GPL 编译器复制实现代码或把受其约束的代码改名混入项目。公开规范中的规则与自行实现的算法应有可追溯依据；沿用仓库已有文件时保留其适用许可证声明。

## 3. 建立纯数据包

选择最近的生产包，用 copytree 复制配置、头文件及必要的 runtime 源码，排除 Wasm、目标文件、可执行文件和缓存。不要复制公共前端或旧子 Wasm 架构。

PowerShell 示例，仓库根目录运行；先选择实际 Python 路径：

```powershell
$Py = 'C:\Users\wth09\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
& $Py -c "from pathlib import Path; import shutil; source=Path('packages/riscv32'); destination=Path('packages/my-rv-board'); assert source.is_dir(); assert not destination.exists(); shutil.copytree(source,destination,ignore=shutil.ignore_patterns('*.wasm','*.o','*.obj','*.elf','*.exe','*.pyc','__pycache__'))"
```

示例名称仅表示同 RV32 引擎的板级变体，不能用它暗示新 ISA 已实现。修改新目录的 `package.ini`：

- `version=3`；`id` 必须等于目录名；`tools` 必须全局唯一，不与来源包重复。
- `target`、`cpu`、`abi`、可选 `engine` 选择实际受支持合同；未知或矛盾布局应拒绝。
- 所需资源采用包内相对路径：`include`、`options`、`encoding`、`selection`、`templates`、`object`、`relocations`、`cpus`、`target_description`、`aggregate`。以当前协议与 parser 允许字段为准，不添加猜测字段。
- `requires` 只声明生产消费者实际使用且支持的能力，如 `encoding.fields.v1`、`selection.scalar32.v2`、`codegen.templates.v1`、`semantic.selector.v1`、`abi.decision.v1`、`cpu.descriptor.v1`、`target.descriptor.v1`、`object.descriptor.v1`、`relocation.vm.v1`、`object.elf32`/`object.elf64`。不要声明尚未接入的实验能力。
- 能力名 CSV 非空、无重复、符合包协议语法。宿主只检查语法，编译器决定支持情况。缺省 legacy 接受不是新规则兼容保证。破坏记录意义时升级对应能力/schema，不能沿用旧名称静默解释。

参考 [宿主](../../../tools/xhcc.py) 保留工具之后的完整 argv；编译器参数由包方言/选项处理，不在桌面宿主硬编码参数名和位置。可先用 `--list-tools` 和 `--dry-run --json --trace-load` 检查 alias 与统一主模块选择。

## 4. 按合同接入目标政策

逐项记录“包记录 → 共享消费者 → 生成行为 → 测试”映射：

| 部分 | 工作与边界 |
| --- | --- |
| CPU/目标 | CPU 表允许同 ISA 配置与别名；目标描述在前端解析前建立宽度、能力和宏。严格核对 engine/ABI/data model，不能只改 ELF machine 掩盖错误指令。 |
| 模板 | 通过共享模板 VM 提供文本/参数替换。模板存在不代表 IR 调度、寄存器分配、控制流和调用算法都已外置。 |
| 语义选择 | 将规范化 family/operation/width/sign/form 映射模板与 pop/push；校验实际栈效果。未匹配的操作必须明确失败或列出有测试的兼容 fallback。 |
| 编码 | 使用有界字段编码、符号范围和对齐验证。Thumb 半字顺序、x86 变长字段等要按实际目标接口处理，不把一个 32 位 scatter VM 宣称为任意编码器。 |
| ABI | 核对参数/返回、变参、聚合、间接传递、栈对齐、callee-saved、运行库和跨 TU。关系表不能自行改变前端耦合 wire 表示；有限 schema 不等于通用平台 ABI。`aggregate=aggregate.rules` 有两个 x86_64 opt-in：`-frecord-sysv-abi`/`abi.plan-record.v1` 仅记录 typed IR 元数据；`-fuse-sysv-abi-plan` 消费版本 2 `SCALAR_VERIFIED` 标量 caller/callee 计划，校验 ABI 身份和规则指纹，支持已实现的整数/FP/指针及溢栈、标量变参。需要后者的包声明 `abi.plan-scalar.v1` 并由编译器能力表验证；声明不默认启用选项。另有 `abi.plan-small-int-struct.v1` 保留自然对齐 4/8 字节整数/指针结构合同，v2 扩展为 1..8 字节；两者均要求 builder flag 2 认证，依赖不规则尺寸的包须声明 `abi.plan-small-int-struct.v2`。仅 x86_64 use 路径可消费 `IR_AUX_AGGREGATE_EXACT` 的 1..8 字节精确传输，序列化与跨引擎 guard 必须验证；其余聚合仍被 use 模式拒绝。这个窄子集不等于完整 GCC/Clang SysV 聚合互操作，默认选项仍关闭。 |
| 对象 | 生成真实标准 ELF32/ELF64，交叉检查 class/machine/endian/flags/attributes、section、symbol、relocation。不得输出自定义 ARK 容器却称标准 ELF；C 输出包应明确输出 C 源码而非伪目标文件。 |
| 重定位 | 共享有限 VM 验证跨度、范围、对齐、指令形状、配对上下文并原子提交。枚举支持公式；外部标准 ELF relocation 留给 GNU/LLVM linker，不能称已经实现全部公式。 |

共享模板/语义/编码/ABI/对象/补丁 parser 应统一错误行为。测试未知版本、重复项、缺字段、未知能力、错误效果、整数溢出、非法寄存器、越界与错位、截断、缺文件。失败必须有资源路径/规则上下文诊断，不静默成功或留下可被误用的有效输出。

保持有限记录数、有限输入字节、有限输出和有界执行。按 [模板](../../../include/xh_template.h)、[编码](../../../include/xh_rules.h)、[语义](../../../include/xh_semantic.h) 等当前 header 的上限设计；不要凭本文扩大 parser 上限。按需加载实际大小的表，在路径/目标/模板 generation 变化时失效并释放旧缓存；不要静态装入所有包，也不要保留悬空模板指针。4 MiB 是当前 Wasm 线性内存目标，完整 MCU RAM 还要加 runtime、WASI、文件系统和任务栈实测，不能从文件大小推断。

## 5. 构建四个独立应用

修改源代码/共享 schema 后统一重建，避免并发测试脚本覆盖公共产物。仅数据变体的 same-SHA 实验必须在基线构建完成后进行，实验中不得重建。

以下工具路径是当前 Windows 工作环境示例，可通过变量与脚本选项覆盖。先验证存在，不下载或安装未授权软件：

```powershell
$Py = 'C:\Users\wth09\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$Sdk = 'D:\wasi-sdk-34.0-x86_64-windows'
$env:WASMTIME = 'C:\Program Files\Wasmtime\bin\wasmtime.exe'
$env:WASM_VALIDATE = 'D:\wabt-1.0.42\bin\wasm-validate.exe'
$env:WASM_OPT = 'D:\binaryen-version_133\bin\wasm-opt.exe'
& $Py tools/build.py --app all-apps --target all --sdk $Sdk --memory-mib 4
if ($LASTEXITCODE) { throw 'build failed' }
```

[根 Makefile](../../../Makefile) 默认构建四应用，也可分别构建 `xhcc`、`xhccm`、`xhccn`、`xhlinker`。`xhcc` 提供编译与受限链接；`xhccm` 加入进程内 make；`xhccn` 保留编译、排除链接；`xhlinker` 是独立 ARM Thumb ELF 链接器，不带前端、IR/codegen 后端或 make。它们共享外部包目录，并非逐后端 Wasm。用 [四应用测试](../../../tests/test_four_apps.py) 验证真实行为和一致输出，并核对 map 中 xhccn 无链接器、xhlinker 无前端；本轮六项资源/隔离审计已通过，后续构建仍须重跑，不能只凭名称宣称隔离。

[无 Python 构建合同](../../no-python-build.md) 给出 [Makefile.nopy](../../../Makefile.nopy) 的直接 GNU Make→Clang→Binaryen→WABT 路径，不依赖 Python 生成源清单或必要中间件。Windows 可用 `D:/msys64/usr/bin/make.exe -f Makefile.nopy all`，并按机器覆盖 `WASI_SDK_PATH`、`WASI_CC`、`WASM_OPT`、`WASM_VALIDATE`、`MEMORY_BYTES`、`WINDOWS_BASH`、`HOST_TOOLS_DIR`、可选 `DLL_DIR`。实际无 Python 四应用构建及独立四应用功能测试已通过；最终 Python 四应用构建与完整回归也全部通过，两条构建路径的证据分别保留，不互相替代。四个模块各自遵守 4 MiB 线性内存预算，不能据此推断同时驻留只需 4 MiB。宿主 Clang 构建 Wasm 不等于 xhcc 生成 Wasm 或完成自举；未来闭环参见 [Wasm 自举计划](../../wasm-bootstrap-plan.md)。

## 6. 用真实执行和阴性用例验收

先跑小范围规则测试，再按 [完整回归入口](../../../tools/test.py) 串行运行要求的 gates；该入口中的测试可能构建/覆盖产物，不得与另一构建并发。保留命令、退出码、SHA、工具链版本和结果，未运行的测试明确列出。

- [host](../../../tests/test_host.py)、[包选项](../../../tests/test_package_options.py)、[能力](../../../tests/test_package_capabilities.py)：纯配置复制 alias，无包内 Wasm；raw argv、未知能力与坏清单失败。
- [CPU](../../../tests/test_cpu_rules.py)、[目标布局](../../../tests/test_target_rules.py)、[宏顺序](../../../tests/test_predefine_order.py)、[ABI](../../../tests/test_abi_rules.py)：布局/宏与真正消费者一致；实验库测试通过不代表生产已接入。
- [语义缓存](../../../tests/test_semantic_cache.py)、[字段/选择](../../../tests/test_rules.py)、[对象](../../../tests/test_object_rules.py)、[补丁](../../../tests/test_relocation_rules.py)：真实 C VM 和坏输入边界。
- [ARM](../../../tests/test_arm_rules.py)、[ARM relocation](../../../tests/test_arm_relocation.py)、[RV](../../../tests/test_rv_templates.py)、[x86](../../../tests/test_x86_rules.py)、[C 输出](../../../tests/test_c_rules.py)、[跨目标](../../../tests/test_targets.py)：真实编译、链接、执行，而非仅断言字符串。

新增 ABI 子集时先按接口阶段记录候选能力，再由实际互操作测试决定是否宣称支持。已接入的 `abi.plan-small-int-struct.v1` 保留 4/8 字节自然对齐整数/指针结构的兼容合同，`abi.plan-small-int-struct.v2` 扩展为 1..8 字节同类结构；依赖精确不规则尺寸传输的新包要求 v2，不因新二进制同时支持 v1 就把旧能力合同扩大。两者排除 packed、浮点字段、bitfield、union、array、变参聚合及超范围尺寸。计划 `SMALL_INTEGER_AGGREGATE_VERIFIED=2` 必须来自 builder 对原始类型图的认证，不得从单个 INTEGER fragment 猜测。后续扩展仍须验证 capability 注册、caller/callee 消费、非法/伪造计划拒绝、实际 Clang 双向混链；通过前保留 pending，不扩展既有测试证据，不默认启用选项。

已验证的 `abi.plan-small-sse-struct.v1` 仅覆盖自然对齐 F4 单 float、FPair8 双 float 和 D8 单 double；builder 必须同时验证原始类型与 single-SSE 分类。签名认证 flag 仅允许互斥 0/1/2/4/8/16/32，3 等组合或未知值拒绝。签名可同时含既有整数结构与 SSE 结构，并采用最高 flag 4；这不是允许单个结构混合 integer/float 字段，也不是全部 SSE word 组合。packed、bitfield、union、array、变参聚合仍排除。实际 906911 字节根产物在启用 Clang 且无跳过的八项 codegen 测试中全部通过，F4/FPair8/D8 参数/返回三条工具链路线、SSE 八寄存器后溢栈（结果 51）、GP 混合签名（15）和 canary 均通过，返回值包括 2.5、4.5/6.5、8.5。标量与已认证整数结构可同签名共存；这不是单结构混合字段或全部 SSE 组合支持，默认不启用。

`abi.plan-int-pair.v1` 另行认证且仅认证自然对齐 16 字节、offset 0/8 两个 64 位 `TYPE_LONG` 字段的结构，源类型及双 INTEGER 分类认证产生最高 eligible 签名 flag 8。不是全部 16B、long long 字段变体或 mixed-bank 结构。参数 `BYVALUE_ADDRESS=0x80000` 搭配低 16 位 size=16、无 FP-slot 标志，仅限 x86_64 use；它是内部源地址，不是 ABI 指针参数。两 GP 不足则整个 16B 入栈并回滚 GP 分配，后续标量可复用剩余寄存器；结果 RAX/RDX。caller bit36 仅申请内部返回缓冲，计划 `sret=0`，不新增隐藏 RDI 参数。评审时必须把这些内部表示与真实 ABI 显式区分，验证序列化和跨引擎/模式 guard，不能因为得到 buffer 地址就把指针传给外部函数。实际 910138 字节根产物 Clang 九项 codegen 测试全部通过，Pair16 三路线覆盖多 live buffer、嵌套调用、rollback 与 canary；不扩大为完整 SysV。

`abi.plan-mixed-pair.v1` 单独认证自然对齐 16B、offset 0/8 的三类 64 位字段组合：LONG/DOUBLE、DOUBLE/LONG、DOUBLE/DOUBLE，最高 eligible 签名 flag16。保留 flag8 twoLONG 合同，不扩大为其它 mixed fields。返回值按 bank 独立编号：INTEGER 用 RAX/RDX，SSE 用 XMM0/XMM1；DL 的第二个对象 fragment 仍是该 bank 的第一个，因此用 RAX 而非 RDX。参数无法完整入寄存器时两 bank 都回滚；BYVALUE_ADDRESS16、内部 buffer 与 plan.sret0 协议不变。实际 911959 字节生产根产物在 Clang 无跳过的十项 codegen 测试中全通过（6.966 秒），包括 LD/DL/DD 三路线、both-bank rollback、多 buffer 与 canary。默认 off，不等于任意 16B 或完整 SysV。

`abi.plan-small-mixed.v1` 仅认证自然对齐 8B、offset 0/4 的 INT32/FLOAT32 或 FLOAT32/INT32 原始字段组合，并要求 classifier 合并为 single INTEGER。最高签名证明级别 flag32 不是 OR，也不能因另一类型具有相同 placement shape 就放宽。浮点字段以原始位随 GPR/栈复制，不做数值转换。实际 911911 字节根产物 Clang 十一项 codegen 全通过无跳过，三路线 IF/FI identity 保持负零 0x80000000 和 qNaN 0x7fc12345，GPR fits/spill、canary、多 copies fixture 全返回 0。默认 off，非任意小混合结构或完整 SysV；后续扩展须有独立源类型证明与阴性边界。

ABI 计划改动还需依据 [计划 codegen 测试](../../../tests/test_abi_plan_codegen.py) 验证双 TU、Clang 双向混链、超范围 aggregate use 拒绝与过期指纹拒绝。实际 906421 字节根产物已在启用 Clang 的七项测试中全部通过：保留标量、4/8 字节参数/返回、GPR 溢栈、指针字段及三条工具链路线，并覆盖 1/2/3/5/6/7 字节不规则结构的 guard page、canary 与 GPR spill。`IR_AUX_AGGREGATE_EXACT` 的序列化、1..8 合法范围和仅 x86_64 use 消费约束须保留；不要以对齐后的 8 字节访问替代精确长度。该七项结果属于整数结构阶段：其中 float 聚合当时仍拒绝；后续八项结果仅另行认证上段三种纯 SSE 结构。packed、bitfield、union、array、超范围及变参聚合继续拒绝。不能将该结果扩展为 <=16 字节混合聚合、完整 SysV、刷新后的 SD 或实机支持结论；记录模式、标量 use、窄结构 use 和其余聚合未支持须分开验收。

布局评审还必须验证完整分配结果：重复寄存器、stack overlap、sret 预留寄存器和 finish 状态。SSEUP 只有同一参数且带显式 class proof 才可共享寄存器；当前 canonical plan 不含这些 class，重复寄存器应拒绝，不能猜测为合法 SSEUP。该一致性校验与 FNV 规则指纹都不是密码学签名安全。

IR/LTO 验收必须包含真实重载后的正例执行，不能仅测 round-trip 解码和阴性。实际 913259 字节生产根产物十二项 Clang codegen 全通过（6.498 秒）：LTO reload → Clang assemble → LLVM link → Unicorn 执行，entry=1。过期 FNV 阴性要求明确 stale-fingerprint 诊断，并排除模板缺失和文件 missing 等早期失败。重载必须补齐 default rule 注册且保留 custom alias，IPO/codegen 必须通过共享 target descriptor 恢复 pointer/long 宽度；这些初始化错误也应有正例捕获。此次流程验证不新增能力、不扩大 ABI 范围。

函数边界还必须在 native emission 前由持久化 source Type 重建 expected named-signature plan，使用共享 `plan_equal` 分别核对 parameter/result 引用。等价内容位于不同表索引应接受；独立布局合法但属于另一函数签名的计划应拒绝。实际 914446 字节根产物十三项 native codegen 与两项 IR 测试全通过：original 和 equivalent-different-index LTO 重发射后均执行 entry=1；单独替换 parameter 或 result 引用为 double 他函数计划，均得到精确 source signature mismatch 诊断。CALL 实参 Type 未持久化，调用点只能检查布局一致性，不能将函数重建检验扩张为全调用源类型认证或密码学安全。这不增加 schema/capability，也不扩大 ABI 类型范围。

输出与构建流程也要有真实失败注入和独立 oracle。[原子输出测试](../../../tests/test_atomic_output.py) 已在实际 WASI 故障用例中验证普通文件采用临时写入再 replace，写入/提交失败保留旧文件；stdout 不具备此原子替换语义，也不承诺 fsync、崩溃持久性或 I/O 安全隔离。当前 916162 字节构建的主 platform 7 项、make 35 项和 RV system 17 项已通过，完整回归仍按实际 runner 状态报告。[make 合同](../../make-subset.md) 的 `$+` 必须保留重复普通依赖、排除 order-only，重复声明以 recipe 声明依赖优先的 GNU 顺序输出；该行为已用 GNU 对照验证，不能以 `$^` 的去重输出冒充。RV ECALL/EBREAK/FENCE/FENCE.I 四个无操作数映射来自外表，额外操作数拒绝；不变主模块新增零参数 `systemnop` alias 已实际执行返回 42。trap 拼写只检查字节不执行，pseudo lowering 仍保留 native，不能扩大为 CSR、完整 FENCE grammar 或任意 ISA。后续进展按 [进度与路线图](../../progress-and-roadmap.md) 对照当前证据。

标准 ARM ELF 首版已有生产消费者：`xhcc`/`xhccm` 的显式 `.o` 链接路径和独立 `xhlinker` 调用同一 [有界 link core](../../../include/xh_elf_link.h) 与 [脚本解析合同](../../../include/xh_ld_script.h)，不再只是 reader/补丁基础组件。先读 [首阶段范围](../../elf-linker-first-stage.md) 并核对当前源码；若该早期计划仍有“尚未接驱动”的历史句，不可覆盖实际入口及测试证据。仅接受首版 ARM Thumb ELF32、little-endian、EABI5/base soft-float 显式对象；不支持 `.c` 自动编译链接/混合输入、archive 自动提取、完整 GNU ld script 或自动提供 runtime。支持六类 REL 的 ELF 隐式 addend，不能套用私有公式；受限 MEMORY/SECTIONS 脚本、ELF PT_LOAD 与按 Flash LMA 的 `.bin` 需要逐项验收，启动代码、data/BSS 初始化和 runtime 对象由调用者提供。

[借用式 reader](../../../include/xh_elf_read.h) 要求缓冲在视图生命周期内有效且不变；[reader 测试](../../../tests/test_elf_reader.py) 的 4950 检查与 [ARM ELF REL 测试](../../../tests/test_arm_elf_reloc.py) 的四项 GNU 对照是底层证据。实际 core 七项测试已通过 GNU 双布局、startup 符号、重定位执行及 objcopy binary 对照，脚本 parser 六项通过；无 Python 四应用功能测试已验证脚本驱动 ELF/bin 一致及 xhccn 拒绝链接。后续改动仍须测符号决议、节布局、PT_LOAD、两布局执行、聚合内存预算和错误保留旧输出。本轮最终 Python 完整回归全部通过，包括 core 7 项、parser 6 项、driver 21 项、four-app 1 项，以及四应用 SD 脚本/bin 与六项资源审计。四产物 xhcc/xhccm/xhccn/xhlinker 分别为 943305/973598/886226/288576 字节，各自声明 4 MiB 上限；map 确认 xhccn 无 linker、xhlinker 无 frontend。它们是本轮快照而非固定规格，实际设备峰值仍待测量；这些首版功能不增加未定义的 target-independent `requires` 名称，也不宣称任意 GNU 工程链接兼容。

后续脚本/预算验收以新增消费者的真实结果为准：本次四应用 945346/975635/886223/290669 字节快照已构建，ASSERT 消费七项与 parser 八项、SD 和七项资源测试已通过，最终完整 runner 也已全部通过。脚本支持有界最多 32 个 ASSERT；标准案例与 GNU 对照通过，但 `&&`/`||` 的短路求值是本子集明示合同，与 GNU ld 对未定义符号可能采用 eager 查找的行为有差异，不能宣称 drop-in/full GNU script。现代链接和独立链接入口的 `--memory-report` 仅统计 link-core 自有 requested heap：metadata、当前 input、script 和 output；不含 relocation VM/rules heap、runtime、stack、文件系统或 allocator 开销，非 MCU 总 RAM。实测 8 对象/64 globals 加 pointers，两入口峰值同为 58506/524288 bytes；1024-byte 小预算失败、旧输出保留且报告可读的测试已通过。每次扩展应重跑成功与故障报告，不用这一 fixture 推断其它输入峰值。

必须加入与本次变更匹配的 same-SHA 实验：对主 Wasm 做 SHA256；复制包，只改一条有效选择/编码/ABI/对象/补丁规则；用不变主模块生成代码；检查二进制/链接执行行为按预期改变；最后主模块 SHA 必须相同。至少一个坏规则应失败且不产有效输出。不能把规则记录数增加当作生效证据。

对机器码用 GNU assembler/linker 或 LLVM 的对应目标作为独立 oracle，比较实际指令字节和标准 ELF header/sections/symbols/relocation；用 Unicorn 在有支持的目标上执行算术、分支、内存、调用和边界样例。Unicorn 不支持的新 ISA 使用独立模拟器/硬件，说明覆盖限制；不要为了得到绿色结果伪装执行。C 输出交给独立宿主 C 编译器并验证运行。浮点、64 位、聚合、varargs、跨 TU 混编要按声明能力单独覆盖。

```powershell
& $Py tools/test.py
if ($LASTEXITCODE) { throw 'integration gates failed' }
& $Py tools/wasm_audit.py build/wasm/xhcc.wasm
if ($LASTEXITCODE) { throw 'xhcc WASI audit failed' }
& $Py tools/wasm_audit.py build/wasm/xhccm.wasm
if ($LASTEXITCODE) { throw 'xhccm WASI audit failed' }
& $Py tools/wasm_audit.py build/wasm/xhccn.wasm
if ($LASTEXITCODE) { throw 'xhccn WASI audit failed' }
& $Py tools/wasm_audit.py build/wasm/xhlinker.wasm
if ($LASTEXITCODE) { throw 'xhlinker WASI audit failed' }
& $Py tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
if ($LASTEXITCODE) { throw 'resource report failed' }
```

[Wasm 审计](../../../tools/wasm_audit.py) 检查声明内存与导入/特性，构建脚本另用 WABT 做实际验证。记录四应用最终 SHA、线性内存和相关 map。审计不是 NuttX 实机运行证明；能力导入、指令支持与额外 runtime 内存需要目标环境验证。

## 7. 四应用 SD 部署和验收

选择可检查的安装目录，先构建再部署。不要在未确认路径时清空目录。部署脚本会写入四个应用和共享纯数据包，不安装忽略的子 Wasm：

```powershell
$Sd = 'D:\c_compiler_make_on_mcu\sd\xhcc'
& $Py tools/deploy_sd.py --sd $Sd
if ($LASTEXITCODE) { throw 'SD deploy failed' }
& $Py tests/test_sd.py --sd $Sd
if ($LASTEXITCODE) { throw 'SD simulation failed' }
```

也可使用根 Makefile 的 `deploy-sd`、`test-sd` 目标并覆盖 `SD_ROOT`。先确认部署目录有 sibling `xhcc.wasm`/`xhccm.wasm`/`xhccn.wasm`/`xhlinker.wasm` 和唯一 `packages` 目录。保持 guest `XHCC_PACKAGE_ROOT` 与 preopen 一致，尤其 make 的 `-C` 后仍能发现包。

[SD 测试](../../../tests/test_sd.py) 在一个预打开目录中模拟直接编译、make、多源、增量与替换 CC；它是桌面模拟，不是实机支持声明。为新包增加同样的直接与 make 构建、输出格式、增量和失败传播测试。检查部署清单哈希，设备侧再测实际 WASI imports、文件系统、峰值 RAM 与重复命令缓存释放。

## 8. 交付证据

最终报告给出：新增/修改文件；包与引擎/ISA/ABI 边界；新增或升级的 schema/capability 及旧包兼容策略；仍在 C 的目标决定；same-SHA 差异；GNU/LLVM oracle、执行、错误/资源/WASI/四应用部署的实际结果；未覆盖项和实机状态。只交付实际完成的结论，不把建议、静态审计、模板数量或模拟 SD 测试写成已完成的全 ISA/硬件支持。
