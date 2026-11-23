# Typeclass v1：独立有界类型图分类原型

## 状态与边界

这是独立测试的分类引擎，不是生产 ABI lowering。当前编译器调用路径没有接入它，package manifest 没有引用 aggregate.rules；它不会改变 xhcc 现有 aggregate private wire，也不能证明与 GCC/Clang 的 native SysV aggregate ABI 互操作。

当前包描述普通整数、指针、float、double 组成的两片段候选对象。没有实现 long double、向量、复杂数、bitfield 精确位投影、寄存器预算/回退、参数位置分配、返回值重建或 va_arg aggregate。未知叶类别报错，不能按相同大小猜测类型。未来需要把分类结果连接到版本化 ABI plan、事务式 bank allocator 和 caller/callee lowering，才能开展双向 native 混链验证。

本文规则向量是原型子集的自主设计预期，不是 GCC/Clang oracle 的实测输出。

## 文件与 API

接口定义在 [xh_typeclass.h](../include/xh_typeclass.h)，实现位于 [typeclass.c](../src/backend/rules/typeclass.c)，外部策略为 [aggregate.rules](../packages/x86_64/aggregate.rules)，独立测试为 [test_typeclass.py](../tests/test_typeclass.py)。

```c
int xh_typeclass_set_path(const char *target, const char *path);
int xh_typeclass_select(const char *rule_target, const char *prefix,
                        const XhTypeclassGraph *graph, unsigned root,
                        XhTypeclassResult *out);
void xh_typeclass_release(void);
```

`set_path("x86_64", path)` 配置共享模板目标别名 `x86_64.aggregate`；target 长度最多 21 字符。随后调用 `select("x86_64.aggregate", "typeclass", ...)`。路径不会自动从 manifest 或目标名称推导；调用者必须显式配置。

select 返回 0 表示有效分类，包含 MEMORY；返回 −1 表示损坏规则、非法图、未知类型或容量错误，输出清零。有效寄存器候选结果有 0–2 个片段，每个片段包含 byte offset、size 和 class ID。MEMORY 结果为 `memory=1,count=0`，不是错误，也不表示应该传一个地址参数。

## 图结构与校验

Node 包含 `form,kind,size,alignment,first_edge,edge_count`。form 为 GROUP 或 LEAF；叶 kind 的语义由外部 leaf 表约定。Edge 包含 child node index 和相对于 parent 的 byte offset。输入数组由调用者持有，引擎不保留其地址。

GROUP 不区分 struct、union 或 array 的源码拼写。struct 以各成员最终 offset 表示；union 的候选成员以重叠 offset 表示；array 可用重复 edge 引用同一 child node，并提供各元素 offset。该 v1 接口不执行 C 类型布局，也不接受未经布局的类型。

固定上限：128 nodes、256 edges、32 层显式遍历栈、1,024 次节点访问、2 个输出片段。共享子图可以重复访问；只禁止当前递归路径形成环。超限报错，不截断。即使 root 已超过 register-candidate 大小限制，仍遍历可达图检查环和未知叶，跳过的只有片段投影。

每个 node 的 alignment 必须为 1–4096 的二次幂。所有 node 的 edge range、child index 和 child 对 parent 的 byte extent 均校验；`offset + child.size` 必须落在 parent.size 内。可达叶的绝对 extent 也必须落在 root 内。LEAF 不得带子边。叶长度可为零；零大小 root 产生零片段。未访问节点的结构边界仍校验，但不根据 leaf kind 分类它们。

## 外部规则格式

文件使用现有 `XHTEMPLATE 1`，不新增文本解析器。prefix 默认示例为 `typeclass`，所有 record 值为固定个数的非负十进制整数，无表达式、跳转或 eval。

| Key | 字段 |
| --- | --- |
| `typeclass.config` | `version unit size_limit class_count empty_class memory_class` |
| `typeclass.leaf.count` | leaf record 数量 |
| `typeclass.leaf.N` | `kind class_id natural_alignment` |
| `typeclass.merge.count` | 必须等于 `class_count * class_count` |
| `typeclass.merge.N` | `existing_class incoming_class result_class` |
| `typeclass.cleanup.count` | cleanup record 数量 |
| `typeclass.cleanup.N` | `predicate input_class action output_class` |

version 必须为 1。unit 为 1–16 字节，size_limit 必须大于零且不超过 2×unit。class_count 为 1–8，empty 与 memory class 必须不同且在范围内。leaf 最多 32 条，kind 为 0–255、不得重复，natural_alignment 为 1–4096 的二次幂。cleanup 最多 16 条。每个数字不得超过 65535。

merge 必须覆盖整个有序 class 对矩阵；缺键、重复 pair 或无效 class 均拒绝加载。记录顺序不要求是矩阵行序；pair 自身决定位置。合并不是引擎内置的整数优先分支。

当前包的 class ID 为 NO_CLASS=0、INTEGER=1、SSE=2、SSEUP=3、MEMORY=4。叶 kind 为 INTEGER=1、FLOAT32=2、FLOAT64=3、POINTER=4；默认没有产生 SSEUP 的 vector 叶。unit=8、size_limit=16，merge 表 25 条。

## 投影与 cleanup

遍历收集叶的绝对 byte interval。每个被 interval 覆盖的片段，使用外部矩阵合并现有 class 与叶 class。padding 初始保持 empty class。union 自然表现为多个叶投影到同一位置。片段 size 为对象尾部的实际剩余字节，不扩大为可越界读写的整 eightbyte。

引擎产生 unaligned 事实的条件：节点绝对 offset 不满足 node alignment，叶 offset 不满足 leaf natural alignment，或 root alignment 小于 leaf natural alignment。后一个条件是当前原型的**保守策略**：packed root alignment=1、double 位于 offset=0 也会触发。此策略尚未经过 native compiler oracle 确认，不能据此声称与全部 psABI packed 规则等价。事实触发后是否 MEMORY 由 cleanup 表决定。

cleanup 按记录顺序、按片段从低到高执行：

| 编码 | 含义 |
| --- | --- |
| predicate 0 | 当前片段等于 input_class |
| predicate 1 | 遍历产生过 unaligned 事实 |
| predicate 2 | 当前片段等于 input_class，且首片段或前一片段不等于 output_class |
| action 0 | 标记整个对象 MEMORY |
| action 1 | 把当前片段 class 改为 output_class |

predicate 2 的 output_class 同时是前驱允许值和替换值，这是 v1 的受限语义，不是通用任意条件语言。当前包记录 `2 3 1 2` 将没有 SSE 前驱的 SSEUP 改为 SSE；默认叶集合使这个规则暂不可达。超过 size_limit 或最终任一片段为 memory class，均产生 MEMORY；cleanup 不能撤回已确定的大小溢出 MEMORY。

## 缓存与指针生命周期

引擎只保留一份规则缓存，以 target、prefix、共享模板 generation 作为身份。`xh_typeclass_release()` 仅释放分类缓存；它不释放模板，也不改变其他模块的 raw pointer。修改已加载同路径文件后，调用者需要显式 `xh_template_release()`，下一次查询将重新加载并观察 generation 变化。

**独立别名不等于独立模板缓存。** 当前模板 VM 一次只驻留一个目标；从 `x86_64` 切到 `x86_64.aggregate` 会使之前返回的模板 raw pointer 失效。此原型没有挂入生产 x86 adapter，避免形成隐蔽悬垂指针。未来集成必须先设计共享表 namespace/cache 生命周期或让全部消费者按 generation 重新取得指针，不能只在调用点插入 select 就宣布完成。

规则缓存是有界小表，图工作栈位于当前调用；没有复制完整类型图。约 4MiB 目标仍需集成后测量峰值内存，不能以规则文件大小替代测量。

## 已执行的 15 个基础向量

I 表示 INTEGER，S 表示 SSE，N 表示 NO_CLASS。这些是 standalone 测试，不含寄存器分配。

| # | 图布局 | 预期片段 class |
| --- | --- | --- |
| 1 | 一个 8 字节 integer | I |
| 2 | 一个 float32 | S |
| 3 | 一个 float64 | S |
| 4 | 一个 pointer | I |
| 5 | 两个 8 字节 integer，offset 0/8 | I,I |
| 6 | 两个 float64，offset 0/8 | S,S |
| 7 | float64 + integer，offset 0/8 | S,I |
| 8 | integer + float64，offset 0/8 | I,S |
| 9 | 三个 float32，offset 0/4/8，共享叶 node | S,S |
| 10 | union 重叠的 float64 与 integer | I |
| 11 | 同片段 float32 与 4 字节 integer | I |
| 12 | 嵌套 group 中的 double，再接 integer | S,I |
| 13 | 两个 array-like edge 引用同一 double node | S,S |
| 14 | 16 字节对象仅 offset 9 有 1 字节 integer | N,I |
| 15 | 零大小 group | 无片段 |

附加测试覆盖 packed offset=1、保守 root alignment不足、24 字节 MEMORY、对齐不满足的 offset=4、循环、超深图、越界 child、非法 alignment、未知叶（含大对象）、完整 merge 矩阵的损坏/缺失/重复项。规则变体测试真实改变 merge 结果和 cleanup 输出，证明外部记录被执行；它们不是有效 SysV ABI 自定义兼容性证明。

最新独立测试运行 4 tests 全通过。测试只构建小 Wasm harness，不构建 compiler image，也不运行 GCC/Clang aggregate 混链。生产接入与 native ABI 验证仍为后续工作。
