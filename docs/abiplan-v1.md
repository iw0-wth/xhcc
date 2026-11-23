# ABI plan v1：独立事务式参数分配原型

此原型消费 [typeclass-v1.md](typeclass-v1.md) 的复制结果及外部 bank 描述。它尚未接入生产调用 lowering、前端 IR、LTO 或标准对象，不改变当前 xhcc aggregate private wire，不证明 GCC/Clang aggregate 互操作。

接口位于 [xh_abiplan.h](../include/xh_abiplan.h)，实现为 [abiplan.c](../src/backend/rules/abiplan.c)，策略位于 [aggregate.rules](../packages/x86_64/aggregate.rules)，测试为 [test_abiplan.py](../tests/test_abiplan.py)。version=1 的 C 结构是进程内数据，不是序列化格式。

## 策略与缓存

`xh_abiplan_load(target,prefix,&policy)` 通过共享 XHTEMPLATE 读取：

- `abiplan.config`：`version GP_capacity FP_capacity stack_slot call_alignment max_alignment sret_bank sret_count class_count`。
- `abiplan.class.N`：`bank demand share_previous`，bank 0/1 为独立资源，2 为 NO_CLASS 跳过位置。每个普通片段 demand=1；SSEUP 风格片段 demand=0、share_previous=1，沿用紧邻前一个同 bank 片段的寄存器索引。无前驱或不同 bank 拒绝，不臆造寄存器。

v1 两 bank 各最多 32 slots，class 最多 8 种，stack slot 为不超过 64 的二次幂，call/max alignment 为不超过 256 的二次幂。每个分类最多两个片段。默认策略 GP=6、FP=8、stack slot=8、call alignment=16、最大参数 alignment=16，sret 预留 GP 第一个 slot。

策略加载到调用者持有的值结构，不保留模板指针；typeclass 输出也是复制结构。同一个 aggregate alias 的 typeclass→abiplan 串联安全。共享模板仍只有一个目标缓存，切换 alias 会使其他模块的 raw pointer 失效；生产集成前必须处理该生命周期边界。修改同路径文件需显式释放模板缓存。

## 事务与结果

`begin(policy,sret,&state)` 初始化版本化状态，可选预留 hidden result pointer 的 bank slots。`argument` 先以局部副本试算整个对象的两 bank 需求；只在所有需求都满足时提交。任一 bank 不足则整个参数按值放到 stack，两个 bank 的已用计数均不变。后续参数能继续使用此前未提交的另一 bank。

MEMORY 分类无需伪造片段，直接产生 by-value stack plan。这与当前 private wire 的“传对象地址”不同。输出含对象 size、effective alignment、stack offset/extent，或至多两个 register 位置及各自 object offset/size。NO_CLASS 片段不输出位置，其后真实片段保留原对象 offset。

栈位置从 outgoing argument area 的低地址起规划：参数起点向上对齐到 `max(argument_alignment,stack_slot)`，extent 向上取 stack_slot 整数倍。`finish` 只返回整体向上对齐到 call_alignment 的预留大小，padding 在高地址端；本原型不生成 push、copy、frame 指令。接入时 caller/callee 必须使用同一约定并与固定 native reference 对照。

参数最多 64 个。invalid version/class、重叠或越界片段、非法 alignment、SSEUP 首片段、资源状态损坏、算术溢出和参数数目越界均失败；失败时 state 与 output 按字节保持不变。MEMORY、register-bank 不足引发的 stack fallback 是成功结果，不是错误。

## 已执行测试

独立小型 Wasm harness 共 4 tests 通过，不构建主编译器：

- GP 耗尽后 mixed 参数整对象上栈且不消耗 FP；随后 double 使用 FP slot 0。
- FP 耗尽后的对称回退，随后 integer 使用 GP slot 0。
- sret 预留第一个 GP；mixed 参数使用下一 GP 和首 FP。
- MEMORY 24 字节参数后放置 alignment16、size9 的参数，offset=32、extent=16、最终 stack size=48。
- SSEUP 沿用前一个 FP index，无额外资源消耗；NO_CLASS 不分配寄存器且保留后续 object offset。
- 非法输入/overflow/64 参数边界的状态与输出原子性；外部容量变更真实改变 placement。
- 实际 typeclass mixed 图结果输入 allocator，验证两个独立模块通过复制数据协作。

这些位置记录为自主设计向量，未运行 reference compiler 双向混链。

## IR 持久化审计与后续集成

当前 [ir.h](../include/ir.h) 的 `IrInst.aux2` 和 `IrProgram.argbytes` 表示旧 wire size/flags，不能重解释为 fragment plan。应新增由 program pool 持有的 plan side table、call plan ID，以及 function 参数/结果 plan ID；64 参数、每参最多两个片段是 v1 明确边界。直接调用、仅声明 callee、函数指针及 variadic 边界必须取得完整类型事实。

[irser.c](../src/ir/irser.c) 已有 argbytes 写入/读取和 merge 时 base 调整机制；新 plan 需要独立版本、ABI identity 或规则指纹、call arg count、stack size、sret 资源及每片段显式字段。使用固定 u32 little-endian 字段与长度校验，不能 memcpy C struct（其中 padding/host size 不是稳定协议）。reader 必须检查完整 run 范围、整数溢出、fragment count 与 object extent；merge/LTO 需给 plan IDs 单独重定位并拒绝不一致 ABI identity。

本轮未改 IR 或 serializer。后续仍需返回值 register plan、call/callee lowering、MEMORY 按值复制、aggregate varargs 重建、序列化往返、双向 native 互操作，以及约 4MiB 预算的集成峰值测量。版本号和独立引擎通过不等于这些步骤已完成。
