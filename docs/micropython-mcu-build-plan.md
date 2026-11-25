# MicroPython 原 Makefile 的 MCU 构建验收

目标是从真实仓库的原 Makefile 编译 MCU 固件，而不是仅扫描大文件或用手写源列表替代。当前尚未达到该里程碑。本文基于本地 MicroPython 的只读审计；不同版本必须重新核对开关与生成链。

## 工作副本约束

原始 `D:/micropython/micropython` 仅作只读参考，不在其中生成文件或修改端口配置。实际构建使用 SD 下保持完整目录结构的 `sd/micropython` 副本，复制现有子模块源码与生成工具资源，排除 `.git` 元数据。复制来源与校验结果记录在 SD 外层清单；本次副本已核验 62,060 个文件、2,078,336,738 字节，size/SHA256 无差异，源 HEAD 与干净状态保持不变。本计划不把机器本地清单提交为通用源码。所有禁用冻结的可审阅配置变更、预生成文件、对象、固件和日志均留在副本，不能破坏原仓库。

## 首个候选与可跳过的宿主工作

首候选为 `ports/minimal/Makefile CROSS=1`，它实际面向 STM32F405、Cortex-M4、soft-float。这与现有 ARM M4 目标族匹配，但不是编译兼容性证明。

用户允许用变量/参数跳过宿主工具，只编 MCU 端；也允许宿主 Python 预生成。必须使用原工程真实开关或清楚的受检 artifact 策略，不能假设所有生成步骤都能跳过：

- `MICROPY_USE_COMPILER_PLUGIN=none` 可避开插件探测。
- `MICROPY_ROM_TEXT_COMPRESSION=0` 可关闭文本压缩生成分支。
- `FROZEN_MANIFEST=` 只影响 manifest，不会删除 minimal 固定的 `_frozen_mpy.c`。
- `QSTR_AUTOGEN_DISABLE=1` 不是禁用所有 qstr 规则和对象依赖的总开关。
- `mpversion.h` 仍有 FORCE 依赖，预先存在文件不等于原 make 不再执行规则。
- minimal 使用已有 `tests/assets/frozentest.mpy` 经 mpy-tool 生成，不必先构建 mpy-cross；其他端口的 manifest 可能需要它，已有工具可用 `MICROPY_MPYCROSS` 指定。

冻结 Python 代码本身是可选产品功能，不是 MCU C 工具链必须执行的步骤。首工程可采用无冻结代码配置；若所选端口没有现成开关，允许提供可审阅、明确记录的最小端口/Makefile 配置补丁关闭冻结，不能宣称该配置是上游 Makefile 无修改兼容。仍须从实际工程构建入口推进，不用手列源文件替代。`mpy-cross` 可由宿主 GCC/Clang 构建或使用匹配版本现成工具；只编 MCU 部分不要求在 MCU 上构建宿主工具。qstr、配置和版本等被实际 C 源码引用的生成结果仍需提供。

预生成产物须记录 MicroPython 版本、端口配置、生成命令、输入与输出摘要。缺失或过期必须报错；宿主步骤与 MCU 编译执行分别记录。当前执行器不运行 shell，不能把原生成器中的 shell/pipe/sed/touch/重定向自动视为受支持。

## 两条合作路线与主验收

允许宿主 GNU make 调用主机 Python 生成器，并通过 CC/LD 变量使用 xhcc/xhlinker。该路线保留原构建图和生成链，用来验证真实 MCU C、ABI、对象和链接能力；GCC 风格选项仍是受检子集，不能宣称直接替换所有 GCC 参数即可成功。

主要验收仍是 xhccm 读取项目 Makefile 并驱动 MCU 对象和固件链接。宿主生成结果可以预先提供，或通过未来明确的宿主调用接口协作；必须报告哪些步骤在宿主完成，不能把 GNU make 构建成功代替 xhccm 完整工程验收。当前 xhccm 不运行任意 shell/Python 命令，未知命令继续明确拒绝。

## 按依赖排序的缺口

1. **Make 入口语义**：`lastword`、`MAKEFILE_LIST`、路径函数、quoted conditional、else-if 和 `info` 等。
2. **大图与长列表**：核心约 132 个对象，当前 make 128 词/2047 字节/64 KiB 状态不足。用顺序解析 journal、分页索引、流式 word iterator 和活动 DFS，不通过提高常量冒充有界。
3. **规则和作用域**：`define/call/vpath`、模式查找及生成头依赖图，正确保持 include、`:=` 与递归变量求值时机。
4. **生成阶段**：明确宿主执行/委派或可信预生成策略，不能由缺失文件直接略过；保持原 Makefile 为入口。
5. **编译器命令**：原 `-g`、`-ffunction-sections` 等当前未实现项须逐个支持或在明确工程配置中移除，不能静默忽略。
6. **真实 TU 语言/ABI**：启动 naked 函数、NLR、扩展 asm、alloca 等需要实际原源文件编译验收。
7. **链接规模**：当前标准 linker 上限 32 个选中对象、512 sections、8192 symbols、512 KiB requested heap/output，尚不足覆盖原核心 132 个对象。需要逐对象/成员读取和受测的规模设计，而非一次读入全库。
8. **脚本与打包**：已有受检 `-T`，应逐项验证原 F405 脚本；map/cref/GC、运行库、size/objcopy/DFU 等命令仍需明确接入范围。

## 当前真实工程进展

SD 副本新增可选 `XHCC_MCU_ONLY=1` profile 关闭冻结，默认 profile 不变；补丁、原文件摘要和生成命令保存在 SD 证据目录，不改原仓。经原 Makefile 的宿主 Python/GNU 工具生成了六个顶层头，219 个输入和 49 个生成 artifact 摘要可校验；版本信息固定到复制源提交。

内置 make 已补 `MAKEFILE_LIST`、`lastword/abspath/info`、quoted conditionals/else chains、变量开头的目标规则；变量成功替换会释放旧 owned 值，40 项回归通过。原 minimal 入口仍在完整 OBJ 列表展开处触发 2047 字节限制；`OBJ=` 仅用于定位后续 `vpath` 缺口，不是有效工程验收。

实际非空 `py/obj.c` 在同一显式 `-O0` MCU profile 下由 GNU ARM 编译成功（该批 text=2864 字节）。xhcc 首次缺标准头和端序宏；补齐 assert/端序及 alloca 包含接口后，实际 `-E` 已成功输出 24961 行，随后 parser 在位域宽度 `8 * sizeof(size_t) - 1` 等表达式失败。下一批已修复受支持的位域常量表达式并通过六项专项；attempt-3 的同一 `obj.c` 已越过该诊断，但编译与 `-fsyntax-only` 都在 IR 生成前触发 `memory pool exhausted`，仍未产生 xhcc 对象。`alloca` 的类型和参数识别已有测试，但执行保持明确拒绝，直到动态栈活跃值与 ABI 对齐验证完成。不能把头文件可包含等同 builtin 已可执行。

独立 expanded 开发配置的 attempt-4（2 MiB arena／8 MiB linear）已越过 OOM，但函数内 `MP_STATIC_ASSERT_NONCONSTEXPR` 的 `sizeof(char[...])` 变长 type-name 被拒绝，尚无对象。该宏有意接受非 ICE 的 extern 对象地址比较，不能用定义别的编译器宏跳过。函数内 VLA `sizeof` 已实现受支持的运行时路径并通过六项专项；后续真实取证显示 parser、sema、IR 均完成，RAM 优化回收不可达分析临时 CFG 后，attempt-8 同预算首次完成 opt。当前 emit 申请 16 KiB 失败，仍无对象；详细阶段数据见[资源预算](resource-budget.md)。

先以主机 GNU make＋Python＋xhcc/xhlinker 推进真实 TU 和固件链，再逐步完善 xhccm 的流式长列表/大图。用户允许独立 expanded 开发 profile 临时增加 arena 与线性内存以先跑通，再按阶段测量优化回目标；默认受限配置及 SD 产物必须保留，不能用扩大预算后的成功冒充原 RAM 目标已达成。每次新 TU 尝试保存独立 artifact 和编译器摘要，不能覆盖旧失败证据。

## 下一批实施顺序

- 完成默认流式前端的旧功能兼容回归与输出故障清理。
- 从原 minimal Makefile 做依赖发现与 GNU dry-run 对照；此步骤不是完整构建。
- 经原生成器在宿主预生成必要文件，冻结版本/配置/摘要，制定 MCU 端拒绝陈旧 artifact 的合同。
- 从一个原 TU 扩展到全部 MCU 对象，逐项修复语言、ABI、flags 和容量问题。
- 使用原链接脚本完成对象链接与打包，验证符号、重定位、初始化及固件启动。
- 真机验证 STM32F405 UART REPL、异常路径及源/头/配置变化后的增量构建。

资源分项测量：文本窗口/scratch、宏、稳定 token、AST/IR、make journal/cache/活动图、链接 metadata、临时磁盘及运行时。大注释小 AST 的流式测试只是前置；不能由它推导整个仓库在 4 MiB 任务 RAM 内可构建。

参考：[流式前端](streaming-frontend-plan.md)、[make 子集](make-subset.md)、[链接器](elf-linker-first-stage.md)、[归档计划](elf-archive-plan.md)、[总体规划](progress-and-roadmap.md)。
