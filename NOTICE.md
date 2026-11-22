# 源码来源

C 前端、IR 与 ARM 发射器的初始源码来自本机参考工程 `../arkcc/`，复制于 2026-11-22。
代码现按 xhcc 自有部件组织在 `src/frontend/c`、`src/ir`、`src/backend/arm`、`src/core` 和 `src/diagnostics`，没有 vendor 层。
原作者版权与 MIT 授权全文保留于根目录 `LICENSE`。
参考仓库基线提交：`e9699cc4caed5dcb5ea949291e943384be47b854`；复制时工作区干净。xhcc 的初始导入提交记录复制基线及后续修改。

xhcc 在此基础上开发受限内存配置、WASI 平台、命令兼容层、外部包协议与标准 ARM ELF 输出。
