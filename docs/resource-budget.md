# 资源预算报告

使用以下命令生成当前构建的 JSON 明细与 Markdown 摘要，并运行 map 解析的小型 fixture 校验：

```text
python tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
```

默认读取生产主模块 `build/wasm/xhcc.wasm`、对应开发构建 map `build/wasm/dev/all/xhcc.map` 和 `packages/`。可用 `--wasm`、`--map`、`--package-root` 指定其他构建或资源快照。工具不启动编译器，不构建 Wasm，也不修改包。

JSON 包含主 Wasm 文件大小、SHA-256、Wasm MEMORY 节声明的初始与最大页数及字节数；每个包的文件列表、总字节数、规则文件字节数、`rule` 记录数量与 INI section 数量；map 中已存在的数据、栈、堆边界符号及资源相关静态 section。`rule_records` 专指以 `rule ` 开始的记录，不能与 CPU section 或编码表行数混为一谈；不同格式另列 `ini_sections` 和非注释行数。

工具从 wasm-ld map 的线性地址列计算 `.data/.rodata/.bss/.ark_buf` 的最高结束地址，不把代码文件偏移或 GLOBAL 表中的索引当作内存地址。缺少 `__stack_low/__stack_high/__heap_base` 等符号时报告 unknown，不根据构建默认值猜测。map 应与被统计 Wasm 来自同一次构建；后链接优化可能改变静态布局，严格核对最终执行模块时还需要导出的边界符号或运行时观测。

2026-11-23 的一次本地报告得到主 Wasm **882,704 字节**、全部外部包资源 **1,111,871 字节**、map 里静态数据最高结束地址 **2,346,536**。这些是该文件快照的结果；后续构建应重新生成报告，不能作为固定规格。生成的 JSON 保存模块哈希以区分快照。

文件大小表示存储、Flash 或传输成本，不能推算运行 RAM。Wasm 的线性内存上限不包含运行时引擎结构、解释器/JIT 本地代码、WASI 宿主缓冲区、任务栈和文件系统缓存。外部包文件也不会全部同时驻留，文件字节数不能直接当作资源解析缓存大小。

`resource_related_sections` 给出 map 中 CPU、规则、编码、选择器、模板和选项映射相关的静态 section。它们已包含在静态数据范围内，不应再次加到该范围上；section 名称筛选可能漏掉共享或通用命名的缓冲区，因此它不是完整缓存审计。实际 arena 峰值使用编译器 `--memory-report` 在代表性输入上测量，完整 MCU RAM、宿主 RSS 及其峰值需要在指定 NuttX/WAMR 或其他目标运行环境上实测。

## 真实 MicroPython TU 的阶段取证与优化顺序

独立开发配置（2 MiB arena、8 MiB linear）的 `py/obj.c` attempt-7 使用固定 MCU-only 配置与经摘要校验的生成头。它尚未输出对象；阶段数据不能解读为完整固件 RAM：

| 阶段 | used 起点 | used 终点 | 净增量（字节） |
| --- | ---: | ---: | ---: |
| preprocess | 0 | 189088 | 189088 |
| parse | 189088 | 1552496 | 1363408 |
| sema | 1552496 | 1663224 | 110728 |
| IR | 1663224 | 2078432 | 415208 |

IR 后只余 18720 字节。`ir.unreachable` 在 used/high=2095432 时申请 2352 字节失败；剩余 1720 字节，不表示再增加 632 字节即可完成整个编译。该取证未改 MicroPython TU，没有扩大到下一档预算。

优化先处理生命周期：独立相同 CFG 的 12 次可达分析表明，返回前回收末尾私有队列可把残留占用从 4800 降为 0、峰值从 4800 降为 400 字节，不增加 heap。整段只读 CFG 分析临时区也已使用受检 mark/rewind 回收：独立实际源码对照中，12 次函数分析的警告逐字相同、IR 不变，used 从 1736 降到 8 字节（先前活跃哨兵），high 从 1752 降到 168 字节；未迁移到 heap。真实 TU 同预算回放结果另记，不能由小 fixture 推断整 TU 已可编译。随后测解析阶段的标识符重复、AST/类型尺寸和符号表重建。流式文件/预处理传输已降低文本缓冲，不会自动回收仍被后续阶段引用的 AST/IR。

attempt-8 在相同 2 MiB arena／8 MiB linear、同一真实 TU 与配置下已完成 opt：used 2078432→2080792，high=2083344。随后 emit 在 used=2089336 时申请 16384 字节失败（剩余 7816 字节），仍无对象。这证明临时分析回收带来了实际阶段推进，不能称整个编译已达内存目标；下一步定位发射阶段缓冲并评估分块输出。

函数级编译、持久 IR 外存和分阶段 arena 分区是后续方案；必须先证明跨函数符号、类型、优化及诊断引用的生命周期，不能直接按函数清空共享池。以上计量仅是 arena，规则解析堆、文件缓冲、临时登记节点和运行时开销另计。

比较资源外置前后时，应保持相同编译器、优化参数、构建 profile 和输入功能，分别保存 JSON，再比较主模块 `file_bytes`、声明线性内存、静态数据结束地址以及各包资源大小。单纯主模块变小不能证明总 RAM 下降。
