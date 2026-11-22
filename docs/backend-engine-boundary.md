# 后端包与共享引擎的验收边界

本文依据当前源码和 [后端外置化状态](backend-externalization.md) 做静态审计。
本轮没有修改编译器代码、构建或运行测试。规则条目数量不是外置完成度；包数据
必须真正参与选择或编码，同一主程序加载变体包后的行为变化才是证据。

## 当前可以成立的架构描述

生产是一个主 Wasm，只有一份 C 前端和 IR；v3 配置包选择已有引擎并按需加载
模板、选择、编码、ABI、CPU、目标和对象规则。包不是子 Wasm，也没有通用原生
插件入口。包可以修改已有引擎支持范围内的 CPU 别名、目标身份、宏、发射文本、
编码字段、部分操作选择和受限 ABI 政策。这与“同一宿主无需任何新引擎 C 分支，
只增加一个包即可支持任意新 ISA”是不同的完成条件；后一条件**尚未实现**。

C 语言语法和语义由共享前端处理，不需要为满足“所有后端外置”而复制到每个包。
ELF 头、节表、符号表、字符串表、边界检查和通用序列化同样可作为共享实现保留。
应外置的是目标决定的参数和选择，而不是把通用算法逐行翻译成脚本。

## 源码审计清单

下列路径为定位依据；代码仍在迭代，避免用行数替代语义覆盖。

| 区域 | 已由包驱动的目标内容 | 仍在宿主的目标专属决定 | 可保留的共享机制 |
| --- | --- | --- | --- |
| `src/frontend/c`、`src/ir` | `target.rules` 的 pointer/long/IR 字宽、complex/variadic/汇编能力由 core 在前端运行前设置 | 前端 IR 表示和现有参数 wire 协议限定可支持布局；不能只改描述就切任意 ABI | C 词法、预处理、语法、语义、类型图、IR schema、常量折叠、CFG 和目标无关优化 |
| `src/backend/rules/target_rules.c` | 包身份、宏、布局字段 | `xh_target_rules_validate` 按 arm/riscv32/x86/x86_64/c 分支规定精确 ABI/data-model 合同；未知 engine 被拒绝 | 有界解析、版本/重复项/数值验证、缓存和资源释放 |
| `src/backend/registry.c` | 包及命令名称可映射已有目标 | 引擎注册与既有 engine family 入口仍编译在主程序中 | 名称查找、重复注册检测、错误传播 |
| `src/backend/arm/backend_arm.c` | 模板、部分指令形状映射、编码规则、寄存器/ABI 描述、CPU 记录与补丁规则 | IR 到序列的调度、复杂数/聚合/调用合法化、Thumb 指令语法和部分形状选择、目标优化器 | 输出缓冲、有限循环、标签/符号查找、受检地址运算；这些不应暗藏 ARM 专属选择 |
| `src/backend/riscv32/backend_riscv32.c` 与 `rules/selector.c` | 编码布局、scalar32 选择序列、大量文本/宽度规则 | IR/调用/返回/栈布局调度、汇编语法；selector 的 mnemonic 白名单和 `t0/t1` operand 映射仍是 RV 专属 C | 有界规则匹配、序列迭代、范围检查；当前 selector 不能仅因放在 rules 目录就称 ISA 通用 |
| `src/backend/x86_64/backend_x86_64.c` 与 `rules/x86_rules.c` | i386/AMD64 寄存器和 opcode/字段、ABI bank、参数/返回分类与布局记录、运算序列 | IR 调度、汇编指令/操作数 grammar、地址模式选择、聚合与 varargs/sret 的部分算法及现有 wire 合同 | 有界参数迭代、bank 计数、对齐、符号与帧槽存储；只有选择政策来自描述时才算共享机制 |
| `src/backend/c/backend_c.c` 与 `rules/c_rules.c` | 文本、类型/运算拼写、libc 路由、帧政策，且当前已有 `c_semantic`/`semantic.selectors` 运算变体及 pop/push 记录 | opcode 到语义 family/fact 的规范化分支、其余调用/类型/属性/程序布局与栈控制处理 | 类型图结构遍历、CFG 工作队列、checked stack-depth、安全数值/字符串转义；它们不要求变成任意脚本 |
| `rules/encoder.c` | 有限字段散布、寄存器/opcode 位和范围/对齐条件 | 已有汇编器决定调用哪个 encoding key 的部分形状分派 | 受检移位/掩码/拼字段及定宽输出 |
| `rules/relocation.c` | 绝对/PC 相对、成对、分裂立即数补丁公式 | 某目标何处创建何种 relocation、怎样形成 addend/配对上下文，仍有后端 C 准备路径 | 有限算术 VM、越界检查、原子提交、符号查找 |
| `rules/object_rules.c` 及对象写入路径 | ELF machine/class/endian/flags、属性字节、内部键到标准 relocation 的映射 | ISA/ABI 特定 relocation 记录的生成、字段语义及部分数据初始化策略 | 标准 ELF 容器、节布局、符号/字符串表、偏移计算和二进制序列化 |
| `src/core/compiler.c`、预定义宏路径 | 外部 CPU/目标数据；命令宏在全部预设后重放 | 与既有引擎/IR wire 的兼容契约和部分运行时能力接线 | 单 TU 生命周期、选项顺序、输入输出、错误处理、pool 管理 |

[旧 C 后端剩余项审计](c-backend-remaining.md) 含早期快照和提案，不能直接当作
当前尚未实现项清单。例如当前 `c_rules.c` 已有有界 semantic selector，
`backend_c.c` 已消费运算变体与对应栈效果；其余 opcode 调度和类型/调用生产
仍需按实际代码逐项判断。

## 两级验收，避免扩大或缩小用户目标

**最低“已有引擎的完整包”验收**要求以下条件同时成立：

1. 包声明明确的 engine/IR/ABI/对象合同和全部必需资源；缺少、未知或矛盾记录
   明确失败，无内置目标默认规则悄悄补齐输出。
2. 已声明支持的操作，其目标专属选择、序列、寄存器/字段、ABI 分配、运行库
   和对象重定位规则由包拥有并被消费。仍硬编码的决定有可定位清单，不用
   “模板全部搬走”替代“决策全部搬走”。
3. 宿主保留的代码只提供已说明的解析/匹配/算术/结构遍历/图分析/序列化机制；
   改名为 shared 或 rules 文件不能使一个 ARM/x86 专属分支自动变为通用算法。
4. 同一主 Wasm SHA256 下更换规则，能改变有代表性的语义、ABI 政策、编码和
   对象字段；输出实际链接/执行得到预期，坏包有阴性测试且不产有效输出。
5. 既有 C 语义、跨 TU、inline asm、FP/long/pointer、参数边界、错误诊断和
   资源限制的回归继续成立，且记录实际 RAM 与只加载当前规则的生命周期。

当前源码已经有大量真实消费路径，但表中仍存在目标专属 C 决策，因此不能
无条件宣布这个较小层级也已经全面完成。每个引擎应按支持能力列出已满足项。

**严格“新增 ISA 仅增加包”验收**则在上述条件外要求：未知的新目标 engine、
指令/操作数 grammar、IR lowering、寄存器/ABI 分类、对象与 relocation 合同
都能由现有通用 schema 表达，新增目标不需要主程序新增 engine C 分支或重建。
当前固定 engine validator、既有汇编 grammar 和 frontend wire 合同明确不满足。
包能给 ARM CPU 改名或添加同 ISA 编码不等于新增 ISA 已实现。

## 无需为外置而继续搬走的内容

以下可留在唯一主程序，前提是输入和政策确实来自包/共享 IR，而没有夹带隐含
目标默认值：

- C 前端、符号/类型图、标准 C 规则和通用 IR/CFG 优化。
- 有界 tokenizer、表查找、schema 验证、模板替换、checked arithmetic 与预算。
- 结构化类型/参数/符号迭代，图遍历、工作列表、区间和通用布局计算。
- ELF 通用 serializer、容器表维护、文件 I/O、缓存释放和错误传播。

反过来，固定 ISA mnemonic 白名单、某 ABI 的寄存器 bank/分类阈值、某目标的
lowering 分支、目标 runtime 语义、位散布和 relocation 公式属于目标政策。
其中政策可外置而迭代器留在 C；这是可验收边界，不要求所有计算都变成脚本。

## 下一步范围

先补缺失的目标决策记录和覆盖测试，明确每个 schema 能表达的形状，再判断
是否需要通用 instruction grammar、ABI 分类和对象合同。扩大 schema 时保持
有界执行、失败可诊断和约 4 MiB RAM 目标。当前不能承诺任意新 ISA 包即插即用，
也不应为追求这一说法无限迁移共享 ELF/C 前端算法。
