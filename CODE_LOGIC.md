# MuOS v0.2 代码逻辑文档

## 一、项目结构

```
muos/
├── boot.s              # Multiboot1 启动入口 (QEMU -kernel)
├── boot_cd.s           # El Torito 启动器 (ISO -cdrom)
├── kernel.c            # 主内核：初始化 + 模块装配
├── linker.ld           # 链接脚本：内核加载地址 1MB
│
├── types.h             # 基础类型: uint8_t~uint32_t, bool_t, size_t
├── io.h                # I/O 端口宏: inb/outb/io_wait（内联汇编）
│
├── vga.h / vga.c       # VGA 文本模式 (80×25, 0xB8000)
├── serial.h / serial.c # COM1 串口输出 (0x3F8)
│
├── gdt.h / gdt.c       # GDT 全局描述符表
├── idt.h / idt.c       # IDT 中断描述符表 (256项)
├── isr.h / isr.c       # ISR 异常处理器 (C 层)
├── isr_stubs.s         # ISR/IRQ 汇编桩 + 调度器钩子
├── pic.h / pic.c       # 8259A PIC 重映射/掩码/EOI
├── irq.h / irq.c       # IRQ 分发器 (含伪中断检测)
├── pit.h / pit.c       # 8253 PIT 定时器 (100Hz)
├── keyboard.h / keyboard.c  # PS/2 键盘驱动 (IRQ1 + polling)
│
├── mm.h / mm.c         # 物理内存管理 (位图 + 分页)
├── task.h / task.c     # 多任务调度 (抢占式轮转)
├── shell.h / shell.c   # 交互式 Shell
│
├── build.ps1           # PowerShell 构建脚本
├── make_iso.py         # Python ISO 生成脚本
└── CODE_LOGIC.md        # 本文档
```

---

## 二、启动流程

### 2.1 boot.s（QEMU -kernel 方式）

```
Multiboot1 头 (0x1BADB002, flags=0x03)
  ↓
start:
  cli                     ; 关中断
  mov esp, stack_top      ; 16KB 内核栈
  push ebx                ; multiboot_info 指针
  push eax                ; multiboot magic (0x2BADB002)
  call kernel_main        ; 进入 C 代码
```

CPU 由 Multiboot loader（QEMU）带入 32-bit 保护模式。栈位于 `.bss` 段 16KB 空间。

### 2.2 boot_cd.s（ISO -cdrom 方式）

| 步骤 | 操作 |
|------|------|
| BIOS 加载 | El Torito no-emulation，BIOS 将 boot.img 加载到 0x7C00 |
| 16-bit 实模式 | 清段寄存器，设栈 0x7C00 |
| A20 | `in al,0x92; or al,2; out 0x92,al` |
| GDT | 3 项（null / 32位代码段 / 32位数据段），0-4GB 平坦模型 |
| 切保护模式 | `mov cr0,eax; or al,1; mov cr0,eax` → far jmp |
| 复制内核 | `rep movsb` 将内核 flat binary 从 0x8C00 复制到 0x100000 |
| 跳转 | `mov eax,0x2BADB002; xor ebx,ebx; jmp 0x08:0x100000` |

内核 flat binary 由 `i686-elf-objcopy -O binary` 生成，紧接在 4KB bootloader 之后。

### 2.3 kernel_main 初始化序列

```
1. serial_init()     → COM1 38400 8N1
2. vga_init()        → 清屏，默认色 LightGrey/Black
3. gdt_init()        → 5段：null/内核CS/内核DS/用户CS/用户DS
4. idt_init()        → 256项：ISR(0-31) + IRQ(0x20-0x2F)
5. pic_init()        → 重映射 IRQ:0x20 主片/0x28 从片
   pic_disable()     → 掩码全部 IRQ
6. mm_init()         → 位图分配器（内核后内存）
   mm_enable_paging()→ 前 4MB 恒等映射，写 CR3 → CR0.PG
7. pit_init(100Hz)   → PIT 通道0 方波发生器
   irq_register(0)   → 注册 timer_handler + 解掩码 IRQ0
8. keyboard_init()   → 注册 kbd_irq_handler + 解掩码 IRQ1
9. task_init()       → 初始化调度器链表
   task_create()     → 创建 shell 任务
10. 内联汇编跳转      → 手动恢复 shell 任务栈帧 → iret
```

---

## 三、中断体系

### 3.1 完整调用链

```
硬件中断
  ↓ CPU 自动压栈: EFLAGS, CS, EIP (+ErrorCode)
  ↓ IDT 查表 (编号 n)
  ↓
isr_n / irq_n（汇编桩）
  cli
  push 0 (dummy err_code) / push n (int_no)
  jmp isr_common / irq_common
  ↓
common handler:
  pusha                          ; 保存 8 个通用寄存器
  push ds, es, fs, gs            ; 保存段寄存器
  mov ax, 0x10                   ; 切到内核数据段
  mov ds/es/fs/gs, ax
  push esp                       ; 传递 registers_t* 给 C handler
  call isr_handler / irq_handler ; → C 层分发
  add esp, 4
  ↓
  [IRQ 分支: 检查 need_reschedule, 按需切换任务]
  ↓
  pop gs, fs, es, ds
  popa
  add esp, 8                     ; 跳过 err_code + int_no
  iret                           ; 恢复 EFLAGS, CS, EIP
```

### 3.2 registers_t 栈帧结构（ESP 指向 gs）

```
高地址
  EFLAGS           ← CPU 自动压入
  CS               ← CPU 自动压入
  EIP              ← CPU 自动压入
  err_code         ← 异常自动压入 / ISR 桩 push 0
  int_no           ← ISR 桩 push %1
  ── pusha ──
  edi
  esi
  ebp
  esp_old          (popa 跳过此项)
  ebx
  edx
  ecx
  eax
  ── push seg ──
  ds
  es
  fs
  gs               ← ESP 指向此处
低地址
```

### 3.3 异常处理

`isr_handler()` 在 C 层接收 `registers_t*`，打印异常名、错误码、寄存器 dump、EIP，然后 `for(;;) hlt` 停机。

Page fault (ISR14) 额外输出 CR2（故障地址）。

### 3.4 IRQ 处理

`irq_handler()` 根据 `regs->int_no - 0x20` 计算 IRQ 编号，从 `irq_handlers[16]` 查表调用已注册回调，含 IRQ7/IRQ15 伪中断检测（读 ISR 确认）。

| IRQ | INT | 设备 | 回调 |
|-----|-----|------|------|
| 0 | 0x20 | PIT 定时器 | `timer_handler` |
| 1 | 0x21 | PS/2 键盘 | `kbd_irq_handler` |

---

## 四、内存管理

### 4.1 物理内存布局

```
0x00000000 - 0x000FFFFF  前 1MB（BIOS/VGA/IVT，保留）
0x00100000 - ???         内核映像 (由 linker.ld 设定)
??? - 结束               内核之后：位图分配区
后续                    可用物理页
```

`_kernel_end` 链接符号标记内核结束。内存管理器从该地址开始放置位图。

### 4.2 位图分配器

- 每 1 bit 对应 1 个 4KB 物理页
- 1 = 已用，0 = 空闲
- `mm_alloc_page()`: 扫描位图找空闲帧 → 标记 → 返回物理地址
- `mm_free_page()`: 清除对应位
- `mm_alloc_pages(n)`: 分配 n 个连续页

### 4.3 分页

```
CR3 → Page Directory (1024 项)
         [0] → Page Table 0 (1024 项)
                  [0]: phys 0x000000 | attr (RW, Present)
                  [1]: phys 0x001000 | attr
                  ...
                  [1023]: phys 0x3FF000 | attr
         [1-1023]: 空 (未映射)
```

前 4MB 恒等映射（虚拟地址 = 物理地址），属性 `PAGE_KERNEL`（Present + Read/Write）。

`mm_map_page()` / `mm_unmap_page()` 支持动态映射/解映射，含 TLB 刷新 (`invlpg`)。

---

## 五、多任务调度

### 5.1 数据结构

```
task_t:
  esp        → 任务保存的栈指针（指向中断帧底部的 eax）
  pid        → 进程 ID（自增）
  name[32]   → 任务名
  state      → READY / RUNNING / BLOCKED / DEAD
  stack_base → 分配的内核栈物理地址（释放用）
  next       → 链表指针
```

### 5.2 任务创建

`task_create(entry, name)`:
1. `mm_alloc_page()` 分配 4KB 内核栈
2. `mm_alloc_page()` 分配 task_t 结构体
3. 在栈顶构造完整中断帧（模拟"被中断"状态）:

```
高地址（栈顶）
  return_addr = task_exit    ← 函数返回时用
  EFLAGS    = 0x202          ← IF 置位（中断开启）
  CS        = 0x08           ← 内核代码段
  EIP       = entry          ← 任务入口函数
  err_code  = 0
  int_no    = 0x20           ← 假装是 IRQ0 触发
  gs .. eax = 0              ← 寄存器初始值
低地址（ESP 保存位置）
```

4. 加入链表尾部，设为 `current_task`（首个任务）

### 5.3 上下文切换

发生在 IRQ 通用处理器的 `irq_common` 中：

```
timer_handler() 设置 need_reschedule = 1
  ↓
irq_common (返回路径):
  cmp need_reschedule, 0
  je .no_switch
  ↓
  pusha (再次保存)  ← 保护当前执行上下文
  mov eax, [current_task_ptr]
  mov [eax], esp              ; 保存旧任务 ESP
  call task_schedule          ; 选择下一个就绪任务
  mov eax, [current_task_ptr]
  mov esp, [eax]              ; 加载新任务 ESP
  popa                        ; 恢复新任务上下文
.no_switch:
  pop gs, fs, es, ds
  popa
  add esp, 8
  iret                        ; 回到新任务执行点
```

### 5.4 首次跳转

`kernel_main` 的初始化代码不属于任何任务。初始化完成后，通过内联汇编**手动跳转**到 shell 任务的栈帧：

```c
mov esp, first_task->esp
sti                          // 开中断
pop gs/fs/es/ds
popa
add esp, 8
iret                         // → shell_run()
```

此后 `kernel_main` 的 boot 栈被永久废弃，调度完全在任务栈之间切换。

### 5.5 调度策略

`task_schedule()`: 从链表找下一个状态为 READY 或 RUNNING 的任务（**轮转**），更新 `current_task` 和 `current_task_ptr`（汇编可见）。

---

## 六、键盘驱动（混合 IRQ + polling）

### 6.1 初始化

- 清状态变量（shift/ctrl/alt/caps/扩展标志）
- 清环形缓冲区 `kbd_buffer[256]`
- `irq_register_handler(1, kbd_irq_handler)` → 解掩码 IRQ1

### 6.2 中断路径

```
按键 → IRQ1 → kbd_irq_handler(regs)
  → inb(0x64) 查状态 bit0
  → inb(0x60) 取 scancode
  → kbd_process_scancode(scancode)
     → 查表 scancode_lower[128] / scancode_upper[128]
     → 处理 0xE0 扩展前缀
     → 处理 release (bit7)
     → 处理修饰键 (shift/ctrl/alt/capslock)
     → kbd_buf_put(char) → 环形缓冲
```

### 6.3 Polling 回退

`keyboard_getchar()` 在等待 `hlt` 之前先轮询 0x64 端口，捕获可能因 PIC 或 IRQ 问题而遗漏的按键。两条路径共享 `kbd_process_scancode()`，写入统一缓冲区。

```
while (kbd_head == kbd_tail):
    if (inb(0x64) & 0x01):
        kbd_process_scancode(inb(0x60))   ← polling 路径
    if still empty:
        hlt                                ← 等待中断唤醒
```

---

## 七、Shell

### 7.1 运行循环

```
shell_run():
  打印欢迎信息
  for (;;):
    shell_readline():
      打印 "muos> " 提示符
      循环 keyboard_getchar():
        Enter → 结束输入
        Backspace → 退格 + VGA 擦除
        可打印字符 → 回显 + 追加到 cmd_buffer
    shell_execute():
      字符串匹配命令名
```

### 7.2 命令表

| 命令 | 功能 | 实现 |
|------|------|------|
| `help` | 列出所有命令 | 直接 vga_print |
| `clear` | 清屏 | vga_clear() |
| `echo <text>` | 回显文本 | 跳过 "echo " 前缀 |
| `mem` | 内存统计 | mm_get_total/free/used_pages() |
| `tasks` | 任务列表 | task_get_count() + task_list() |
| `time` | 系统运行时间 | pit_get_ticks() → 时/分/秒 |
| `reboot` | 重启 | 加载空 IDT → int $0 (三重故障) |
| `logo` | 显示 MuOS logo | ASCII art + 彩色输出 |

---

## 八、构建系统

### 8.1 build.ps1

- `.\build.ps1`         → 编译 NASM (.s) + GCC (.c) → link → `build/kernel.elf`
- `.\build.ps1 -Iso`    → 额外运行 make_iso.py → `muos.iso`
- `.\build.ps1 -Run`    → QEMU -kernel / -cdrom
- `.\build.ps1 -Clean`  → 删除 build/ + muos.iso

产物全进 `build/` 目录，源码目录保持干净。

### 8.2 make_iso.py

1. NASM 编译 `boot_cd.s` → `boot_cd.bin` (flat binary, 4KB)
2. objcopy `kernel.elf` → `kernel.bin` (flat binary)
3. 拼接 bootloader + kernel → `boot.img`
4. pycdlib 创建 ISO 9660 Level 1
5. El Torito no-emulation 启动，`boot_load_size` = ceil(boot.img / 512)

### 8.3 链接脚本

```
ENTRY(start)     ← boot.s 的 start 标签
. = 0x100000     ← 1MB 加载地址
.multiboot : { *(.multiboot) }   ← Multiboot 头必须在前 8KB
.text : { *(.text) }
.rodata / .data / .bss
_kernel_end = .  ← 内存管理器标记内核结束
```

---

## 九、关键全局变量

| 变量 | 定义位置 | 作用 |
|------|----------|------|
| `need_reschedule` | isr_stubs.s (.data) | 调度标志，timer_handler 设 1，irq_common 检查并清零 |
| `current_task_ptr` | isr_stubs.s (.data) | 指向当前任务的 esp 字段，汇编直接访问 |
| `_kernel_end` | linker.ld | 内核结束地址，mm_init 从此开始放置位图 |

---

## 十、已知局限

- 单处理器（多核需 APIC + TSS）
- 无用户态（任务全在 Ring 0）
- 调度器无优先级 / 无阻塞原语（semaphore/mutex）
- 内存分配器无碎片整理 / 无内核堆
- 页表仅映射前 4MB（超过需动态扩展）
- 键盘仅支持 US QWERTY 布局
- CD 启动器无 Multiboot 信息传递（EBX=0）
