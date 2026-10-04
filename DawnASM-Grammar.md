# DawnASM 语法（v4）

> 按你的答复定稿，`dasm` 与正式文档随后对齐。

---

## 0. 核心规则

1. 参数（源操作数）写在 `{}` 里，逗号分隔 —— **但只有一个参数的指令不写 `{}`**（见第 5 条）。
2. **`->` 表示返回值/目标**：
   - 有结果要写回的指令：`{源...} -> 目标`
   - 调用/系统调用：`{参数...} -> 返回值`
3. **省略 `->` 时，结果写回 `{}` 的第一个参数。**
4. **没参数、没返回值的**（`jmp`、用现有标志的 `j<cc>`、`nop`、`hlt`、`ret`）：直接写指令后面，**不用 `{}` 也不用 `->`**。
5. **只有一个参数的指令不写参数列表 `{}`**：
   - 单源（`mov` `ldi` `ld` `ldv` `not` `neg` `inc` `dec` `fneg` `fabs`）→ `dst, src`
   - 单参（`push src`、`pop dst`、`call 函数`、`intervm id`）→ 直接写参数
   - 多参数才用 `{} ->`

---

## 1. 寄存器

```
<宽度><类型><编号>
```

| 部分 | 取值 | 说明 |
|---|---|---|
| 宽度 | `E` `X` `H` `L` | 32 位 / 16 位 / 高 8 位 / 低 8 位 |
| 类型 | `I` `F` `P` | 整数 / 浮点 / 指针 |
| 编号 | `0`–`f` | 十六进制 |

示例：`EI0`、`EF3`、`EP1`、`HVa`

- **指针类一律用 `EP`**：栈指针 `EPf`，帧指针 `EPe`。
- 指针不做算术语义上的运算；内存寻址的 base 必须是 `EPn`。
- 64 位用寄存器对：`EI0:EI1`（低位:高位，编号连续）。

---

## 2. 变量（运行时在堆上申请）

```
var-i i, sum              ; 整数变量
var-f floatArray[6]       ; 浮点数组，6 个元素
var-p ptr                 ; 指针变量
var-i buf[64]             ; 整数数组
```

- 声明形式：`var-<类型> 名字[, 名字...]`，数组加 `[长度]`。
- 类型：`i` 整数、`f` 浮点、`p` 指针。
- 变量在**运行时申请**（堆上），**不是寄存器**，所以**不能直接写进 ALU 指令**。
- 收发只走两条指令：
  - `ldv dst, 变量` / `ldv dst, 数组[下标]` —— 变量值读进寄存器
  - `stv 变量, src` / `stv 数组[下标], src` —— 寄存器值写回变量
- 下标是寄存器（如 `buf[EI1]`），按元素宽度自动算偏移。
- 数组元素宽度默认 4 字节，可用 `:ob/:tb/:fb/:eb` 指定。
- 只写名字不带下标 → 取的是**首地址**（要指针寄存器接：`ldv EP0, buf`）。
- 汇编器按名字算出堆地址，不用手写 `[EPn+...]`，避免寻址搞乱。

例子：

```
var-i buf[64]

method main {
    stv buf[EI1], EI0        ; buf[EI1] = EI0
    ldv EI2, buf[EI1]        ; EI2 = buf[EI1]
    ldv EP0, buf             ; EP0 = buf 首地址
}
```

---

## 3. 注释 / 段 / 标签 / 数据

- 注释：`;` 或 `#`，到行尾。
- 段：`section .text`（代码，VA 从 0 起） / `section .data`（数据，紧随 .text）。
- 标签：`名字:`，可一行多个。
- 数据：`db` `dw` `dd` `dq`；预留：`resb` `resw` `resd` `resq n`。

---

## 4. 方法

```
method 名字 {
    ...
}
```

- 指令必须写在 `method` 内，不能裸露、不能嵌套。
- 入口固定 `main`。
- 参数与返回类型暂不写（留给高层语言）。

---

## 5. 指令

### 5.1 无参数 / 无返回值

`nop`　`hlt`　`ret`

### 5.2 单源指令（`dst, src` 形式）

| 写法 | 含义 |
|---|---|
| `mov dst, src` | 寄存器复制 |
| `ldi dst, imm` | 立即数装入 |
| `ld dst, [地址]` | 内存读入 |
| `ldv dst, 变量` | **从变量（堆）读入**，`ldv dst, buf[idx]` 带下标 |
| `not dst, src` | 按位取反 |
| `neg dst, src` | 取负 |
| `inc dst, src` | 自增 |
| `dec dst, src` | 自减 |
| `fneg dst, src` | 浮点翻符号 |
| `fabs dst, src` | 浮点取绝对值 |

写回方向（目标在前）：

| 写法 | 含义 |
|---|---|
| `st [地址], src` | 寄存器写入内存 |
| `stv 变量, src` | **寄存器写回变量（堆）** |

- 省略 `dst` 时原位：`inc EI0`、`not EI0`。
- `stv` 的 `src` 允许直接写立即数：`stv i, 0`。

### 5.3 双源 ALU（`{} ->` 形式）

助记符：`add adc sub sbb and or xor shl shr sar rol ror`

```
add {a, b} -> dst        ; dst = a + b
add {a, b}               ; 省略 -> ，写回第一个参数：a = a + b
cmp {a, b}               ; 只置标志，无返回值
test {a, b}              ; 只置标志
```

- 立即数直接写进 `{}`：`add {EI0, 5}`
- 移位/循环：`shl {a, n} -> dst`

### 5.4 乘除法

助记符：`mul` `imul` `div` `idiv`

| 写法 | 含义 |
|---|---|
| `mul {a, b} -> dst` | 无符号乘法 |
| `imul {a, b} -> dst` | 有符号乘法 |
| `div {a, b} -> dst` | 无符号除法 |
| `idiv {a, b} -> dst` | 有符号除法 |

**结果位宽由 `->` 目标决定**（不再是固定截断）：

- 目标是 32 位寄存器 → 取低 32 位
- 目标是寄存器对 `EI2:EI3` 或 8 字节内存 → 完整 64 位
- 乘法省略 `->` → 写回第一个参数，宽度 = 第一个参数的宽度（即截断）

**除法：商在 `->` 目标，余数固定回填第一个参数。**

```
div  {a, b} -> dst      ; 无符号：dst = a / b（商），a = a % b（余数）
idiv {a, b} -> dst      ; 有符号：同上
```

- 除法**必须显式写 `-> dst`** 接商（不写会和余数一起挤进第一个参数，不许）。
- `mul`/`imul` 没有余数。
- 除零一律得 0。

乘除法**源和目标都支持内存寻址**：

```
imul {EI0, [EP0+4]} -> EI2:EI3     ; 32x32 → 64 位积
idiv {EI0:EI1, EI2} -> EI4         ; 64/32 → 商在 EI4，余数回填 EI0:EI1
```

### 5.5 浮点二元

```
fadd {a, b} -> dst      fsub {a, b} -> dst
fmul {a, b} -> dst      fdiv {a, b} -> dst
fcmp {a, b} -> dst      ; dst = -1 / 0 / 1（NaN 为 0x80000000）
```

寄存器存 IEEE-754 float32 位型；`fadd` 等只做 float32。

### 5.6 栈

| 写法 | 含义 |
|---|---|
| `push src` | 入栈（单参数，不写 `{}`） |
| `pop dst` | 出栈（无参数，直接写返回值） |

### 5.7 分支

| 写法 | 含义 |
|---|---|
| `jmp 标签` | 无条件跳转（无参数、无返回值 → 直接写标签） |
| `j<cc> 标签` | 用**现有标志位**跳转（同上，省略括号和箭头） |
| `j<cc> {a, b} -> 标签` | 比较 `a`、`b` 再跳（融合一条 `cmp`） |

条件码 `<cc>`：

| cc | 别名 | 含义 |
|---|---|---|
| `e` | `z` `eq` | 等于 |
| `ne` | `nz` | 不等于 |
| `ig` | `g` `gt` | 有符号大于 |
| `il` | `l` `lt` | 有符号小于 |
| `ige` | `ge` | 有符号大于等于 |
| `ile` | `le` | 有符号小于等于 |
| `ug` | `a` `gtu` | 无符号大于 |
| `ul` | `b` `ltu` | 无符号小于 |
| `uge` | `ae` `geu` | 无符号大于等于 |
| `ule` | `be` `leu` | 无符号小于等于 |
| `s` / `ns` | | 符号位 |
| `o` | | 溢出 |
| `al` | | 总是 |

### 5.8 调用

| 写法 | 含义 |
|---|---|
| `call 标签` | 普通调用，**不支持参数和返回值** |
| `call-method {方法, 参数...} -> 返回值` | 调 `method`，支持参数和返回值 |
| `call-method {方法, 参数...}` | 没返回值就省 `->` |

### 5.9 系统调用

| 写法 | 含义 |
|---|---|
| `intervm {调用号, 参数1, 参数2} -> 返回值` | 三个源槽，空的槽填 `NULL`；`-> 返回值` 可省 |
| `intervm 调用号` | 不带参数、不要返回值时的简写 |

- 第一个槽是系统调用**号**或**名字**（见下表），其余槽是参数；
- 约定：参数 1 走 `EI0`，参数 2 走 `EI1`；返回值写回 `EI0`。
  写代码时别让自己的数据占着 `EI0`/`EI1`（类似 x86 的 eax/ebx 易失约定）。
- 例子：
  - `intervm {PRINT, msg, NULL}` —— 打印字符串
  - `intervm {17, 0, NULL}` —— 退出，状态码 0
  - `intervm {3, buf, len} -> EI0` —— 读输入
  - `intervm {ITOA, buf, EI2} -> EI3` —— 整数转字符串

**系统调用表**（名字大小写不敏感，`_` 可写可不写；地址参数是 DawnVM 内存地址）：

| 名称 | 号 | 参数 | 返回 | 说明 |
|---|---|---|---|---|
| `PUTC` | 1 | (char) | - | 输出一个字符 |
| `PRINT` | 2 | (str) | - | 输出 NUL 结尾字符串 |
| `READLINE` | 3 | (buf, size) | len | 读一行，回显 |
| `READLINES` | 4 | (buf, size) | len | 读一行，不回显 |
| `PRINT_I32` | 5 | (i32) | - | 十进制打印有符号整数 |
| `PRINT_U32` | 6 | (u32) | - | 十进制打印无符号整数 |
| `PRINT_HEX` | 7 | (u32) | - | 十六进制打印 |
| `PRINT_F32` | 8 | (f32) | - | 打印浮点（6 位小数） |
| `GETKEY` | 9 | () | key | 读一个键，无回显 |
| `STRLEN` | 10 | (str) | len | 字符串长度 |
| `STRCMP` | 11 | (a, b) | -1/0/1 | 字符串比较 |
| `STRCPY` | 12 | (dst, src) | dst | 字符串复制（含 NUL） |
| `ATOI` | 13 | (str) | i32 | 字符串转整数 |
| `ITOA` | 14 | (buf, i32) | len | 有符号整数转字符串（写 NUL） |
| `UTOA` | 15 | (buf, u32) | len | 无符号整数转字符串（写 NUL） |
| `FTOA` | 16 | (buf, f32) | len | 浮点转字符串（6 位小数，写 NUL） |
| `EXIT` | 17 | (code) | - | 结束程序 |
| `SETMODE` | 18 | (mode) | - | 切换运行模式：0 普通 / 1 包（UI） |
| `TOUPPER` | 19 | (char) | char | 单字符转大写 |
| `TOLOWER` | 20 | (char) | char | 单字符转小写 |
| `STRUPR` | 21 | (str) | str | 字符串原地转大写 |
| `STRLWR` | 22 | (str) | str | 字符串原地转小写 |
| `SORT_I32` | 23 | (buf, count) | - | i32 数组升序排序 |
| `SORT_I32D` | 24 | (buf, count) | - | i32 数组降序排序 |
| `SORT_F32` | 25 | (buf, count) | - | f32 数组升序排序 |
| `SORT_F32D` | 26 | (buf, count) | - | f32 数组降序排序 |
| `MEMZERO` | 27 | (ptr, count) | - | 把 count 字节清零 |
| `ATOF` | 28 | (str) | f32 | 字符串转浮点 |
| `PRINTC` | 29 | (str, attr) | - | 带颜色输出，`attr = fg \| bg<<4`（仅包模式） |
| `GOTOXY` | 30 | (row, col) | - | 移动文本光标（仅包模式） |
| `CLEAR` | 31 | () | - | 清屏（仅包模式） |
| `GETMODE` | 32 | () | mode | 读取当前运行模式 |

- `PRINTC` / `GOTOXY` / `CLEAR` 只在**包模式**（`SETMODE 1`）下生效；
  普通模式下 `PRINTC` 退化成 `PRINT`，另外两个被忽略。

### 5.10 循环语法糖（`loop` / `endloop`）

```
loop{REG, start, <cc>, a, n}
    ; 循环体
endloop
```

等价于：

```
for (REG = start;  REG <cc> a;  REG += n) {
    ; 循环体
}
```

| 元素 | 含义 |
|---|---|
| `REG` | 循环计数器，寄存器（如 `EI0`） |
| `start` | 初值（立即数或寄存器） |
| `<cc>` | 条件码，同 5.7 表（如 `il` 有符号小于、`ule` 无符号小于等于） |
| `a` | 比较目标（立即数或寄存器） |
| `n` | 步长，**有符号**（负数即递减） |

例子：

```
loop{EI0, 0, il, 10, 1}      ; for (EI0=0; EI0<10; EI0+=1)
    ...
endloop

loop{EI0, 10, ig, 0, -1}     ; for (EI0=10; EI0>0; EI0-=1)
    ...
endloop
```

- `start` / `a` / `n` 若是立即数，按 `REG` 的宽度处理。
- 允许嵌套；汇编器自动生成内层标签。

---

## 6. 操作数细节

### 立即数

`0x1F`、`0b1010`、`123`、`'A'`（`\n \t \r \0 \\ \'`）、浮点 `3.14f` / `3.14d`、标签 `msg`、`msg+4`。

宽度后缀（可选）：`:ob`=1、`:tb`=2、`:fb`=4、`:eb`=8（简写 `:o :t :f :e :b`）。

### 内存

```
[EP0]                     基址
[EP0+disp]                基址 + 偏移
[EP0+EIm]                 基址 + 变址
[EP0+EIm*4]               基址 + 缩放变址
[EP0+EIm*4+disp]          基址 + 缩放变址 + 偏移
```

- base 必须是 `EPn`；index 是 `EIn`。
- 变址支持缩放：`*1` `*2` `*4` `*8`（不写默认 `*1`）。
- 访存宽度看目标寄存器：`ld EI0, [EP0]` 读 4 字节，`ld XV0, [EP0]` 读 2 字节。
- 说明：有了变量后，内存寻址基本用不上，但**保留**。

---

## 7. 完整示例

```
; dasm hello.dasm hello.dwn
section .data
msg: db "Hello, DawnASM!", 0x0a, 0

section .text
method main {
    ; 系统调用用 EI0/EI1 传参，计数器别用 EI0/EI1
    loop{EI2, 0, il, 3, 1}
        intervm {2, msg, NULL}
    endloop
    intervm {17, 0, NULL}
}
```

数组 + 变量收发：

```
section .data
msg: db "sum=", 0

section .text
var-i sum
var-i buf[8]

method main {
    stv sum, 0
    loop{EI1, 0, il, 8, 1}
        ldv EI0, buf[EI1]
        ldv EI2, sum
        add {EI2, EI0}
        stv sum, EI2
    endloop
    intervm {2, msg, NULL}
    intervm {17, 0, NULL}
}
```

---

## 8. 确认纪要

- 余数 → 第一个参数；商 → `->` 目标。
- 单参数指令不写 `{}`。
- `loop{...} / endloop` 糖，`n` 有符号，允许嵌套。
- 变量运行时申请、支持数组，声明 `var-<类型>`。
- `call` 无参无返回；`call-method` 才有参数和返回值。
- 变址支持缩放；内存寻址保留但基本用不上。

语法到此定稿，接下来改 `dasm` 和正式文档。
