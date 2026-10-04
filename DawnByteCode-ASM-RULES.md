# DawnASM 汇编规则（已迁移）

本文档是早期草稿，语法已过时（寄存器命名、`intervm` 传参、变量/数组、`loop` 语法糖、
变址缩放等都与当前实现不一致）。

**最新、权威的语法请看：[DawnASM-Grammar.md](DawnASM-Grammar.md)。**

工具链入口不变：

- 汇编器：`dasm <src.dasm> <out.dwn>`
- 运行器：`rundwn <prog.dwn>` / `runpack <pkg.pack>`
- 虚拟机内核：`API-LIB/Bin/dawnvm/dawnvm.c`（解释器）、`dwn.h`（编码定义）
