# AMD64 SysV aggregate lowering：外部规则与共享机制方案

本文是待实施设计，不是兼容性声明。仅面向 Linux/ELF System V AMD64；i386、Microsoft x64 和 C++ 非平凡对象另需独立契约。本文没有修改生产源码，没有构建或运行新的 reference compiler。

## 当前边界与立即可见的风险

当前 xhcc 的大于 8 字节 aggregate 通过一个地址参数及 hidden result pointer 传递，小 aggregate 采用已有 private wire。已通过的 packed、sret、varargs、混合参数测试是 xhcc 调用 xhcc，不能证明 GCC/Clang aggregate ABI 互操作。使用 x86_64 GCC 工具别名输出 ELF 不会自动赋予其完整 SysV aggregate ABI；现在不能把带 aggregate 参数或结果的 xhcc 对象与 GCC/Clang 对象混合并假定正确。scalar、pointer 和其他已测路径的结论应分别保留。

生产接口 `XhX86AbiClass` 只有 bank、words、width、alignment、indirect；18 条记录以 kind/size/alignment/variadic/index 范围匹配。这能描述现有 wire，但不足以表达一个对象内多个 eightbyte 的不同 class、SSEUP 与前一个 SSE 的共享寄存器、多个 class 的合并、寄存器耗尽后的整对象回退、返回值双 bank 分配和 MEMORY 参数的按值栈布局。仅增加若干 size 区间无法修补这些缺口。

实施前应让工具及文档明确区分 native SysV lowering 与 legacy private wire。legacy 应继续可用，禁止在同一调用边界悄悄选择两种 wire。是否对暂未实现的 native aggregate 边界报错，应由主产品变更显式决定；不能借迁移删除旧能力。

## 规范依据与 reference 取得方式

最终规则必须以固定版本的 AMD64 psABI 分类、传参与返回章节为依据，并记录 PDF/hash/版本日期。权威项目入口是 [x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)，参考实现应同时记录 LLVM 或 GCC 的完整 target compiler 版本。本次在线获取规范失败，下面给出的是需要对照固定规范复核的设计和待测向量，未声称已逐条核验最新版文本。

当前配置的 WASI SDK clang 只保证 Wasm 工具链可用，不能假定能组装或生成 AMD64 reference 对象。已有 targets gate 在 full-target clang 缺失时会跳过混合对象检查。后续先查询 clang 的 targets，再试编译一份 `--target=x86_64-unknown-linux-gnu -ffreestanding -fno-pic -fno-pie -O0 -c` 文件；成功、ELF machine 正确且反汇编可检查，才能称其为 reference。可在后续经授权安装 MSYS2 完整 LLVM/GCC；本文没有安装工具。

reference 生成要同时保留源文件、命令、stderr、版本、对象及汇编。GNU as、LLVM MC 或 compiler 汇编输出可帮助解释分类，真实双向跨对象调用才是兼容性 gate。禁止把现有 xhcc 的错误输出当唯一 oracle。

## 共享类型图机制所需输入

分类应在 IR 降低前获得完整的 ABI 类型图，而非仅 `argbytes` 的 size/FP/slot64 位。现有 Type/StructField 已有成员偏移、bitfield 宽度/位偏移、array base/count、packed 与 alignment 属性；需要审计它们是否足以恢复所有有效存储片段、union 分支与自然对齐信息，并测试序列化往返。尤其不能把 packed aggregate 的较小总 alignment 当作“所有成员天然对齐”。

建议共享只读图节点接口包含：

- 语义叶类别及精确位宽、存储大小、自然/有效 alignment；不使用后端寄存器名字。
- array 元素、计数与 stride；struct 的成员位偏移与长度；union 的所有候选成员。
- bitfield 的实际存储位区间及容器信息、padding、零宽分隔后的最终布局。
- 函数原型、具名/匿名参数、variadic 状态、返回类型；indirect call 也必须可达同一原型描述。

共享 walker 负责显式工作栈、循环检测、offset overflow 检查和已布局成员的遍历。嵌套深度、展开次数、class 数组容量均有硬上限，超限报错。array 遍历受候选寄存器对象大小阈值限制；一旦确定 MEMORY 就不展开巨大数组。禁止把完整源程序类型树复制为第二套无界树。

## 外部分类描述：有限关系表，而非任意代码

建议新增独立版本的通用 ABI 描述接口，现有 x86 adapter 作为一个消费者。最小 class 字母表需要 NO_CLASS、INTEGER、SSE、SSEUP、X87、X87UP、COMPLEX_X87、MEMORY，并显式标记当前引擎不支持的叶类别。class 数字由 package schema 声明，不可与机器寄存器编码混用。

外部文件分四部分：

1. **布局参数**：eightbyte 大小、候选分类的容量上限、栈 slot/alignment 限制、支持的 ISA/vector feature，以及参数和返回各自 register bank 顺序。不要笼统把“所有超过 16 字节的对象”写成永远 MEMORY；若未来引入向量，需按选定 psABI 的 post-merge/vector 例外处理。
2. **叶映射**：语义类别/位宽/feature 到一个有界 class 序列和有效 payload bit 区间。普通整数/pointer、float/double 与未实现的 long double/vector 必须分别描述，不能按相同大小推断相同 class。
3. **合并关系**：完整的 `(existing,incoming)->merged` 表；无匹配、重复 pair、非闭包 class 或非法 class ID 拒绝加载。union 使用同一对象位置合并所有候选成员；struct/array 按成员偏移覆盖相应 eightbyte。合并顺序、NO_CLASS 的单位元性质以及规范定义的 MEMORY/INTEGER/浮点冲突规则应由固定表体现。
4. **cleanup 与 placement**：使用固定种类的条件谓词记录，例如 `any(class)`, `class_at(i)`, `previous_class(i)`, `eightbyte_count_in_range`, `has_unaligned_member`, `feature_present`。动作仅允许 `set_class`, `set_all_memory`, `reject_unsupported`。支持常量索引和有界逐槽扫描，不允许 goto、任意算术表达式、递归调用、文件访问或宿主调用。X87UP 的前导约束、SSEUP 的修正及大 aggregate 的 post-merge 条件都应由这些记录表达，顺序固定、次数有限。

walker 只执行“叶映射、投影到槽、merge、cleanup”这些共享操作。它不包含按 x86 类型名称编写的分类分支。对不对齐字段，walker 产生事实，外部规则决定 MEMORY；对 padding 保持 NO_CLASS。bitfield 的边界投影需要单独依据规范及 reference 复核，不能简单把整个容器都分类而宣称正确。

loader 应验证 merge 表全覆盖、所有输出 class 合法、cleanup 操作数量与索引范围、feature gate 和寄存器列表。分类器需要返回带理由的错误状态，区分“有效 MEMORY”与“不支持的类型/损坏规则”。任何错误禁止生成部分 native 对象。

## 分类结果与事务式寄存器分配

建议结果结构为一个有界片段向量，每片段保存 object bit offset、payload bit count、class、bank、bank-slot demand 及与前片段的同寄存器关系。另有 memory/unsupported 状态、按值大小、栈 alignment。现有一个 bank/words 结构不能兼任该接口。

共享 allocator 先试算整个参数的 INTEGER/SSE 资源需求，再一次性提交；任何必要 bank 不足时，按照选定 psABI 把整个参数回退到内存，不能消耗一个 bank 后让另一个片段上栈。回退后后续参数必须仍能使用未被提交的寄存器。SSEUP 共享规则不能被计成独立 XMM demand。寄存器次序和返回 bank 次序来自包，算法只管理游标与事务。

MEMORY 参数应由 caller 在参数区按值复制对象，callee 从对应栈位置读取；它不等同于当前大 aggregate 的一个 pointer 参数。返回 MEMORY 使用 hidden result pointer，并按规范更新首个 INTEGER 参数资源；callee 的返回地址寄存器承诺也需处理。register 返回则根据片段向量从独立结果 bank 取位置，支持 INTEGER/SSE 混合，不借用参数寄存器列表。

栈区域先整体规划，再按 alignment/size 放置与复制。call 对齐、具名/匿名参数及 vector feature 条件必须来自明确描述，不能单靠 `%rsp` 逐参 push。caller 和 callee 必须消费同一份 lowering plan。

## IR 与序列化的必要变化

必须把 aggregate 的“值”与当前被前端提前替换成地址的 private wire 分开。建议在类型可用时产生 ABI plan，保留对象内存位置及每个片段，而不是先在 irgen 中把大对象永久改成一个 pointer，再要求 backend 猜原值。

plan 至少描述 direction、argument/result index、类型大小/alignment、每个片段的 class/offset/width、register/stack/hidden pointer 位置。IR serialization 要有版本和 ABI identity；缺少 native plan 的 legacy IR 不能自动当作 native SysV。IR 文本、LTO、函数指针调用、声明但未定义的 callee 和 variadic call 都需覆盖。若计划与实际 target/rules contract 不一致，在发射前失败。

callee prologue 把寄存器片段重建到局部 aggregate 存储；caller 从该存储按片段载入；register 返回做反向操作。对象尾部不足 eightbyte 时不能越界读写或复制未分配字节；padding 的传输与可观察行为按规范处理。packed MEMORY 路径必须复制精确对象大小。

## 验证向量：待固定规范与 reference 确认

以下都是未来必须执行的向量，不是本次实测输出。不要仅给 `sizeof` 结果；每例都生成单独 caller/callee、显式初始化数据、计算可核对的整数 checksum，并双向混链。

| 向量 | 需要区分的行为 |
| --- | --- |
| `struct { long a,b; }` | 两个 integer 片段，参数与返回 |
| `struct { double a,b; }` | 多个浮点片段及独立寄存器需求 |
| `struct { long a; double b; }` 及反序 | 混合 bank、返回寄存器次序 |
| `struct { float a,b,c; }` | 同 eightbyte 中多个 leaf、尾片段宽度 |
| 嵌套 struct、2/3 元素数组 | offset 投影与递归合并 |
| `union { long i; double d; }` | 同位置不同 leaf 的 merge |
| packed `struct { char c; double d; }` | 真实字段不对齐及 MEMORY 路径 |
| 三个 `long` 的 struct | 大对象按值栈传递与 hidden result |
| bitfield 跨 byte/eightbyte、零宽分隔 | 类型布局与分类投影一致性 |
| 先用尽 INTEGER，再传 mixed aggregate，再传 double | 整对象 rollback 后 SSE 资源仍可用 |
| 先用尽 SSE，再传 mixed aggregate，再传 long | rollback 不泄漏 INTEGER 资源 |
| sret 函数再接 mixed 参数 | hidden pointer 占用及两 bank 游标 |
| direct、declared-only、function pointer 三种调用 | 同一原型使用相同 plan |
| named 与 `va_arg` aggregate | register-save/overflow 区重建及耗尽边界 |
| 两个不同 TU 同名 static aggregate helper | 符号绑定及 relocation 与 ABI 独立 |

如引擎未来支持 `long double`、128/256/512-bit vector 或 `_Complex`，另加 X87/X87UP/COMPLEX_X87、SSEUP 及 feature-dependent 参数向量；未支持前应明确拒绝，不用普通 double 或 integer 猜代替。

每个向量在至少两个 link text/data 地址执行，包含符号+addend pointer 成员；reference caller→xhcc callee、xhcc caller→reference callee、reference 自调用、xhcc 自调用分别记录结果。用 LLVM ld 或 GNU ld 生成标准 ELF 后交 Unicorn 执行，必要时再用真实主机执行交叉核验。只检查对象能链接不算通过。

## 外规则变体与验收步骤

1. 冻结规范和 reference 版本；先验证类型布局，尤其 packed/bitfield/union。
2. 独立验证 bounded classifier 的 leaf/merge/cleanup，比较 reference 汇编，但不把寄存器名差异直接当失败。
3. 连接 shared allocator 与 ABI plan，用资源耗尽向量验证 rollback。
4. 逐类开启 caller/callee/return 的 native lowering；保留 legacy wire 的独立测试，防止旧功能退化。
5. 执行全部双向混链及 varargs/indirect/LTO tests。未覆盖的类型继续报告 unsupported，文档不能称完整 ABI。
6. compiler SHA 不变，等价合并表重排/域拆分应保持结果；一个语法合法但违反声明 ABI 的 class merge 变体必须在契约校验时拒绝。若允许实验 ABI，必须采用不同 ABI identity 且双方显式匹配，用隔离对象证明规则改变实际 lowering，不能冒充 SysV。
7. 验证缺表、重复 merge、非法 class、递归/容量溢出、plan 版本错配全部失败且不输出有效对象。

4MiB 预算要求单目标规则惰性加载，class 向量与 traversal 工作栈按有界上限分配，并按目标切换释放缓存。测量 peak allocator/linear memory，不把规则文件大小当作驻留 RAM 证据。

完成标准是上述支持类型的双向 native ABI gate 通过、private wire 明确隔离、外部表实际驱动分类/分配且坏表失败。本文本身不改变当前 aggregate 兼容性边界。
