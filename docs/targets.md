# 目标与后端名称

包ID表示目标家族/实现，不以默认CPU命名。ARM包为`arm`；默认`cpu=cortex-m3`只决定省略-mcpu时的值，不限制包只能编M3。

| 目标/架构 | 已验证范围 | 边界 |
| --- | --- | --- |
| ARMv7-M | Cortex-M3，Thumb-2，base AAPCS | 非ARM/A32状态 |
| ARMv7E-M | Cortex-M4/M7，soft及受检softfp | hard VFP参数ABI未实现 |
| ARMv8-M Mainline | Cortex-M33当前指令/ABI子集 | 非完整CMSE/TrustZone安全扩展声明 |
| ARMv6-M | 尚未开放M0/M0+/M1 | 需Thumb-1选择与合法化，不可只换CPU表 |
| ARMv8-M Baseline | 尚未开放M23 | 不支持当前主Thumb-2代码生成路线 |
| ARMv8.1-M/MVE | 未实现 | 不能以M33验证替代 |
| Cortex-A/R、AArch64 | 未实现 | 需其他状态/ABI/指令规则 |
| RV32 | RV32IM，ILP32和软件浮点辅助调用 | 非RV64或硬浮点ABI |
| i386 | 当前32位自有wire/标量SysV子集 | 聚合传参不等于完整GCC兼容 |
| AMD64 | LP64、标量SysV/SSE参数及已测varargs | 完整SysV聚合eightbyte分类未实现 |
| c | IR生成C文本，Wasm构建时前端pointer32/slot64 | 不是IR生成Wasm后端 |

`arm-none-eabi-gcc`或`armcc`是参数接口名称，不代表完整复制对应商用/GNU编译器的全部行为。不能把可解析CPU描述等同于该指令集已实现。

## WebAssembly 的两个角色

xhcc编译器本身是WASI Preview1 command模块，C前端、IR和后端引擎都在该模块中执行。当前`c`目标输出可交给另一个C编译器的C源码。

将输入C程序直接生成为wasm32/WASI程序，需要独立wasm32目标后端：结构化控制流或dispatch lowering、Wasm指令、线性内存和栈帧、函数表/间接调用、数据/重定位与导入ABI。现有C前端/IR可以复用，但该目标尚未实现。实现起步无需额外编译器源码；WASI SDK、WABT、Binaryen和Wasmtime可作参考及验证工具。
