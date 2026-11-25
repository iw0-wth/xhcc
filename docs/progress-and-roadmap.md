# 曦和编译器：进度与后续任务

此文是能力与验收清单，不是完成百分比。实际边界以实现、测试和各接口合同为准。

## 已落地并持续回归

- 四个独立 WASI Preview1 应用：xhcc（编译＋ARM/RV32 标准链接）、xhccm（编译＋链接＋make）、xhlinker（独立标准链接）、xhccn（编译期排除对象链接器）；共用外部 packages 目录。整体 Makefile 保留 Python 构建路径，[Makefile.nopy](../Makefile.nopy) 提供实际验证的无 Python 构建路径。
- ARM Thumb-2、[受限 ARMv6-M Thumb-1 局部标量子集](arm-thumb1.md)、RV32IM、i386、AMD64 与 C 文本输出；现代驱动按包工具别名派发。已有引擎的包变体可通过复制目录分发；任意新 ISA 仍可能需要引擎开发。
- 现代机器目标的 `-c` 输出标准 ELF ET_REL（ARM/RV32/i386 为 ELF32，AMD64 为 ELF64），含标准符号、节和重定位。外部 GNU/LLVM 混链及 CPU 执行验证已接入回归。
- 显式 `--private-object` 的 ARKOBJ 对象和私有 IR/LTO 是独立 legacy 路径，不是标准 ELF 对象。
- 外部模板、语义选择、编码、重定位、目标/CPU 描述和有界 ABI 计划已被实际消费者使用。所有后端都有真实外部规则路径，但仍有目标政策留在 C 中。
- AMD64 可选 ABI 计划支持已认证的标量、若干小整数/浮点/混合结构和明确的 16 字节双字段布局；具体范围见 [ABI 合同](abi-plan-ir-v2.md)。不是完整 SysV 聚合 ABI。
- make 支持工具/变量注入、目标与模式变量、依赖文件、增量构建等有界子集。具体语法见 [make 范围](make-subset.md)，不运行 shell，不是完整 GNU make。
- 模拟 SD 部署、直接编译、make、增量重建、CC 切换与资源哈希验证已接入；四个 Wasm 线性内存上限均为 4 MiB。
- 真实 IR 重载执行、计划布局/函数源签名检查、诊断 pragma 的 TU 隔离和 arena 峰值统计修复均有独立回归。
- 新增[语言中立 IR core v1](ir-core-v1.md)：独立类型/操作码、校验与编码，非 C 合成程序经现有后端执行验证。当前只覆盖 straight-line i32/u32 子集，五个现有引擎的整数 IR_BINARY 消费者已接入显式中立操作码边界；旧 IR 的 C Type/token 依赖尚未全部迁移。
- 链接器新增受检 ASSERT 与 requested-heap 报告，类型范围不扩；[ARKOBJ 与旧链接器定位](arkobj-and-legacy-linker.md)区分格式、实现和内存实测。

## 链接器的准确状态

arkcc 有 legacy 内置链接器；xhcc/xhccm 保留私有对象链接、部分镜像输出及私有 IR LTO 路径。共享标准链接核心现由现代驱动、make 进程内链接入口和 xhlinker 消费 ARM/RV32 显式 `.o`，读取受检 `.ld` 的 ENTRY/MEMORY/SECTIONS 子集，输出标准 ET_EXEC 或按 Flash LMA 排列的 binary。GNU ld/objcopy 地址与字节对照、实际执行测试已验证这一子集。

这仍不是完整 GNU ld。当前支持 ARM Thumb/base soft-float 的六类 REL，以及 flags=0、ILP32、RV32IM 的受检 RELA 子集（含 PCREL HI/LO anchor 与不缩短指令的 RELAX 标记）。不自动将 `.c` 编译并链接，不支持归档自动提取、完整 linker script、运行库搜索、RVC/硬浮点及 i386/AMD64 标准链接。可烧录镜像需要用户提供正确板级脚本、启动代码、vector table、初始化逻辑与所需 runtime；不能把文件格式正确当作任意板可直接烧录。xhccn 不包含上述对象链接实现。

## 按依赖推进的任务

| 优先级 | 工作 | 完成验收 |
| --- | --- | --- |
| P0 最高优先 | [MicroPython MCU 工程构建](micropython-mcu-build-plan.md) | 默认文件文本传输已改[流式前端](streaming-frontend-plan.md)，大 TU 是前置；接着推进原工程 Makefile、受检预生成/可选冻结配置、大图与多 TU/链接规模。原仓库只读，实际操作仅在 SD 完整副本 |
| 已验证，持续维护 | 原子输出、make 重复依赖和 RV32 系统指令外置 | 本批故障注入、GNU 行为对照、规则变体同 SHA 执行、完整回归及 SD 测试均通过；不代表完整 GNU make 或完整目标 ISA |
| 已实现首版，继续扩展 | [ARM/RV32 共享标准 ELF 链接器](elf-linker-first-stage.md) | 显式对象、符号解析、受检脚本布局、ARM REL/RV32 RELA、ET_EXEC/binary 已接入；后续扩展目标/重定位/归档/运行库及脚本语法，各项单独验收 |
| P1 | [未来 wasm 后端自举条件](wasm-bootstrap-plan.md) | 双 Python/无 Python 构建入口可用；补 wasm 输出引擎、WASI ABI、模块链接、编译自身源码所需语言/头文件与容量；目前不能自举 |
| P1 | 继续全部后端政策外置 | 逐项移除目标政策分支，改善共享合同；允许标准序列化、通用遍历与有界算法留在共享 C 机制中；规则变更确实影响编码/执行 |
| P2 | 扩展 ABI 类型与调用覆盖 | 为每种布局增加源认证、caller/callee 双向混编、寄存器不足回退、返回/变参及内存边界测试；拒绝未实现形状 |
| P2 | 补 make 的实用兼容与完整工程工作流 | GNU 对照、真实多源增量构建、参数优先级、失败处理；环境/export、递归与并行、shell 和链接支持各自显式规划 |
| P2 | RTOS harness/NuttX/WAMR 真机集成 | 验证 WASI 文件系统与导入、SD 路径、运行时配置、取消/错误回传及任务重入边界 |
| P2 | 测量与优化完整 MCU RAM | 统计线性内存之外的 Wasm 运行时、解释器/编译元数据、宿主栈和 WASI 开销；不能用 arena 峰值或 Wasm 文件大小代替总任务 RAM |
| 持续 | 文档、双语 README、移植 skill、版本发布 | 文档不扩大能力声明；验证过的同一提交同步两个远端，产物与 SD 清单一致 |

## 每批变更的交付顺序

1. 冻结并审阅源码，构建两个应用。
2. 完成对应专项测试与完整回归；期间不替换主程序以保证同 SHA 检查有效。
3. 刷新模拟 SD，验证两应用、共享包和部署哈希。
4. 更新能力边界，提交并核对工作区。
5. 在已授权的发布范围内推送 Gitee/GitHub 并核验提交 SHA。

参考：[后端外置清单](backend-externalization.md)、[资源预算](resource-budget.md)、[移植后端 skill](skills/port-backend/SKILL.md)。
