# GNU 静态柔性尾成员初始化

此扩展支持静态全局对象的初始化：顶层 `struct` 的最后一个字段可以是真正的柔性数组 `[]`，其初始化列表决定该对象的额外存储。它用于 GNU C 静态数据布局，保持声明类型的 nominal `sizeof`，不修改共享 `Type` 的数组长度或 struct 大小。

```c
extern void a(void);
extern void b(void);
extern void c(void);
extern void d(void);
extern void e(void);
struct S { int head[5]; void (*slots[])(void); };
struct S mp = {{1, 2, 3, 4, 5}, {a, b, c, d, e}};
```

ARM 上 `sizeof(struct S)` 是 20；`mp` 的对象存储及 ELF symbol size 是 40。五个地址槽的 relocation 位于对象内偏移 20、24、28、32、36。相同类型的不同全局对象可以有不同的尾部长度，共享类型仍保持原大小。

## 存储推导及边界

`ark_agg_storage_type()` 根据已有 initializer 文本创建 arena 内的私有存储视图；`ark_agg_storage_size()` 返回实际对象 extent。数组下标 designator 按最高已初始化下标加一推导长度，元素间距使用声明元素的大小。实际字节填充和地址初始化遍历使用对应的完成后视图，并检查缓冲区边界。未初始化的尾部元素、字段间隙保持零填充。

extent 为 nominal size 与“尾字段偏移加已推导元素数量乘元素 stride”二者的较大值，不额外按 struct alignment 扩大尾部。例如 `struct { int head; char tail[]; } x={1,{65}}` 在 ARM 上占 5 字节，ELF symbol size 为 5；后续对象独立满足其起始地址对齐要求。

本扩展只开放顶层 struct 的最后真正柔性 `[]` 字段。它没有普遍放开所有非正数组长度，也不开放 non-tail、嵌套柔性对象、自动存储对象或动态数组初始化。显式 GNU `[0]` 使用 `Type.is_zero_array` 与 `[]` 区分；非空 `[0]` 初始化仍由数据填充路径拒绝。

前向 extern 声明可能保留不完整的 tagged struct 类型节点。当随后出现对象定义时，IR 合并路径使用定义声明的完成后类型刷新 `IrGlobal.type` 和对应 `Symbol.type`，再进行字段和地址初始化遍历。此刷新同时覆盖普通全局声明和声明块中的全局对象。

## 写出与 private IR

ARM 全局存储分配、标准 ELF 和 GNU assembly listing 使用对象 extent。RV32 与 x86/x86_64 聚合数据写出及目标 ELF symbol size 也使用该推导结果。C backend 保留声明的柔性 `[]` 和 initializer 文本，由 GNU C 的对象布局规则重建存储。

没有新增 `IrGlobal` wire 字段。现有 private IR 保存 nominal Type、initializer 文本和地址引用，当前 reader/consumer 可从文本重新推导柔性尾部 extent，因此不依赖未序列化的额外字段。

显式 GNU `[0]` 的新 Type 标志无法由现有 v1/v2 private IR 类型记录准确表达。private IR writer 遇到该类型明确报错并要求使用标准 ELF 对象，不会静默丢弃标志；旧 v1/v2 reader 路径将该标志初始化为零。柔性 `[]` 与显式 `[0]` 的 private IR 行为必须区别对待。此处尚未新增 flexible initializer 的 private IR roundtrip 专项验证。

## 验证与尚待验证项

独立生产源码测试 `tests/test_flexible_static_init.py` 覆盖 nominal 20、storage 40、五个地址槽、零填充、稀疏 designator、长度溢出拒绝、短缓冲拒绝、显式 `[0]` 非空拒绝、char 尾部 extent 5 和字符串尾部 extent 7。

生产编译器测试通过 `test_driver.command()` / `XHCC_COMMAND` 选择构建产物。ARM 用例与 GNU ARM 对象比较初始化字节、symbol size 和五条 `R_ARM_ABS32`。跨对象写出用例覆盖 ARM、RV32、x86_64 的双实例 extent、稀疏 designator、nominal sizeof、char 尾部五字节存储和下一对象起始对齐。这两项生产测试在追加 forward extern 回归前的新模块上均已通过。GNU ARM 独立 oracle 也确认 40 字节对象、五条 relocation 及五字节 char 尾部对象。

forward extern 回归已追加到 ARM 生产用例。最小复现中，旧模块在 opaque typedef 后先 extern、再完成 struct 并定义对象时失败；extern 位于完整 struct 后则通过。相应 IR 合并修复在新 expanded 模块上的 ARM GNU oracle 回归已通过；完整测试共三项通过、无跳过。

真实 MicroPython attempt-3 中 `parse.c` 已通过编译，而 `objint.c` 的 `mp_type_int` 仍报告地址初始化无法放入类型。attempt-4 的新模块仍有该错误。随后使用独立私有 IR probe（复用同一 build metadata 的 flags、其余对象与链接参数，不覆盖正式产物）检查真实翻译单元：`mp_obj_type_t` nominal size 20、全部字段和 `slots` 偏移 20 均正确；存储视图测量失败，是因为传入的 initializer 文本已有错误字段 `.slot_index_`，尾部还包含未正确展开的 `_MP_OBJ_TYPE_SLOT_TYPE_,make_new,...`，而不是 GNU 输出中的完整字段名和 cast 指针值。因此当前真实剩余失败指向预处理宏展开/拼接，不是缺少柔性存储布局或 forward extern 刷新。证据保留于 `build/flex-mpy-debug/result.log`；探针基于 expanded/all/xhccn 正式模块 SHA256 `e47f6d8f4a629756bde519e569a6a4311d9f0dd3da160d92418250534b3501e5`（920071 字节），配置为 4 MiB arena、16 MiB linear memory、128 KiB stack。forward extern 修复解决的是生产回归测试已捕获的独立真实缺陷，不是本次完整 MicroPython 翻译单元的最终阻塞原因。这里不宣称真实 `objint` 已经通过。原 MicroPython 源码及其宏保持只读，未为适配编译器修改。
