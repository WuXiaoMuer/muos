# MuOS — 从零编写的 x86 32 位内核(Windows 本地开发)

在 Windows 上原生开发(不依赖 WSL / Linux)的教学型操作系统内核。当前版本包含:

- **完整启动链**:Multiboot1(QEMU `-kernel`)+ 自研 El Torito 光盘引导
- **硬件抽象**:GDT / IDT(256 项全覆盖)/ PIC / PIT / PS/2 键盘 + PS/2 鼠标 / 串口 / VGA 文本模式
- **内存管理**:multiboot mmap(E820)解析、物理页位图分配器、全 RAM 恒等映射分页、内核堆(kmalloc/kfree/kcalloc)
- **Ring3 用户态**:TSS + `int 0x80` 系统调用(Linux i386 编号)+ ELF32 加载器,可在 0x08048000 沙箱内运行 C 程序;ELF 可来自内核内嵌(`run`)或 RAM FS(`exec hello.elf`);用户态故障只杀进程,不 panic 内核
- **Linux ABI 起步集**:`exit/read/write/open/close/lseek/getpid/getuid/getgid/brk/uname/mmap/mmap2/munmap`(文件 fd 带偏移,mmap 仅匿名映射)
- **RAM 文件系统**:16 槽 × 64KB(足够存放静态 i386 ELF)+ 全屏文本编辑器
- **"MuOS 7" 桌面**:Win7 风格文本模式 GUI —— 任务栏 + 实况 RTC 时钟/内存、开始菜单、窗口 z-order、鼠标点击/拖拽/最小化/最大化/关闭、聚焦后键盘可交互的 Calculator
- **57 项内核自检**(启动时自动运行,或 Shell 中输入 `test`)

演进目标见 **[ROADMAP.md](ROADMAP.md)**:以 Linux ABI 兼容(int 0x80 + ELF32,运行静态 i386 Linux 二进制)为主线;NT/Win32 兼容为长期远景。

## 项目结构

```
muos/
├── src/             内核源码(25 个 .c + 3 个 .s)
│   ├── boot.s       Multiboot1 入口(入口=镜像首字节,jmp 跨过内联 multiboot 头)
│   ├── boot_cd.s    El Torito 光盘引导(实模式→保护模式→搬运内核→跳转)
│   ├── isr_stubs.s  中断/异常/系统调用汇编桩 + Ring3 进入/返回
│   ├── kernel.c     内核主入口,初始化序列
│   ├── mm.c         mmap 解析 / 位图分配器 / 分页 / 用户沙箱映射
│   ├── kheap.c      内核堆(first-fit 自由链表)
│   ├── mouse.c      PS/2 鼠标(8042 aux、IRQ12 共享路由、文本光标)
│   ├── rtc.c        CMOS 实时时钟(任务栏 HH:MM:SS)
│   ├── tss.c        TSS(esp0 = Ring3 进入 Ring0 的 8KB 栈)
│   ├── elf.c        ELF32 ET_EXEC 加载器(段装入 + bss 清零 + 范围校验)
│   ├── syscall.c    int 0x80 分发(stdio + 文件 fd + uname + 匿名 mmap)
│   ├── user.c       用户沙箱、初始栈、进入/退出、故障收尸
│   ├── string.c     freestanding 字符串/内存函数(标准名)
│   ├── fs.c editor.c win7.c shell.c task.c test.c ...(fs:16×64KB)
│   │   editor: 128×79、10KB 存盘缓冲、PgUp/PgDn;win7: z-order/鼠标/RTC
├── user/            Ring3 C 程序(start.s / runtime.c / hello.c / linker.ld)
│                    hello 演示文件 syscall、uname、mmap、brk 与键盘 read
├── include/         头文件(与 src 配对,muos_ 前缀 include guard)
├── build.ps1        一键构建 / 运行 / ISO / 清理(含用户程序构建与 ELF 嵌入)
├── make_iso.py      Python + pycdlib 自制可启动 ISO
├── setup-tools.ps1  自动下载 NASM / QEMU 到 tools\
├── linker.ld        链接脚本(内核加载到 1MB)
├── run.bat          双击:构建 + QEMU 快速启动
├── run_iso.bat      双击:构建 + ISO 启动
├── clean.bat        清理构建产物
└── CODE_LOGIC.md    代码逻辑详解(启动/中断/内存/用户态/Shell)
```

工具目录(均被 gitignore,不影响仓库体积):`toolchain\` 交叉编译器、`tools\` NASM/QEMU。

## 快速开始

```powershell
# 环境初始化(自动下载 NASM + QEMU 到 tools\)
.\setup-tools.ps1

# 构建 build\kernel.elf
.\build.ps1

# 构建 + QEMU 运行(最快路径);进入 Shell 后输入 run 运行内嵌 Ring3 C 程序
.\build.ps1 -Run

# 构建 + 生成 muos.iso + 光盘启动(需要 python + pycdlib:pip install pycdlib)
.\build.ps1 -Iso -Run

# 清理
.\build.ps1 -Clean
```

工具查找顺序:PATH → 仓库内 `toolchain\` / `tools\` → 常见安装位置(含 `X:\qemu`)。也可双击 `run.bat` / `run_iso.bat`。

### 交叉编译器

Windows 的 MinGW gcc 面向 PE 格式,不能编译裸机内核,需要 `i686-elf-gcc`:

1. 从 https://github.com/lordmilko/i686-elf-tools/releases 下载 `i686-elf-tools-windows.zip`
2. 解压到仓库根目录的 `toolchain\`(build.ps1 会自动找到),或解压到任意目录后加入 PATH

### 手动构建(等价命令)

```powershell
nasm -f elf32 src\boot.s -o build\boot.o
nasm -f elf32 src\isr_stubs.s -o build\isr_stubs.o
i686-elf-gcc -m32 -ffreestanding -O2 -g -Wall -Wextra -fno-exceptions -fno-stack-protector -nostdinc -Iinclude -fno-tree-loop-distribute-patterns -c src\kernel.c -o build\kernel.o
i686-elf-gcc -nostdlib -T linker.ld -Wl,--no-warn-rwx-segments build\boot.o build\isr_stubs.o build\*.o -o build\kernel.elf
qemu-system-i386 -kernel build\kernel.elf -m 256M
```

## 两条启动路径

| 路径 | 入口 | 说明 |
|------|------|------|
| QEMU `-kernel` | Multiboot1 头 | QEMU 直接加载 ELF,boot.s 传 magic/mb_info 给 kernel_main |
| 光盘 `-cdrom` | El Torito → boot_cd.s | 自研引导扇区:实模式 → A20 → 平坦 GDT → 保护模式 → 拷贝 flat kernel 到 0x100000 → 跳转 |

**布局约束**:内核入口 `start` 必须位于镜像第一个字节(boot.s 用 `jmp real_start` 跨过内联的 multiboot 头)。ISO 路径把 flat binary 拷到 0x100000 后直接跳过去,若 multiboot 头排在最前就会把魔数当指令执行。Multiboot1 规范只要求头在镜像前 8KB 内,`jmp` 跨头方案同时满足两条路径。

ISO 路径没有 multiboot 信息(EBX=0),内存管理回退到 mem_upper 默认值(32MB)——见 ROADMAP。

## Ring3 用户程序

内核启动时把 `user/hello.c` 编译成静态 ELF32(i386,链接地址 `0x08048000`),再作为二进制块嵌进内核镜像。Shell 中输入:

```text
muos> run                 # 运行内核内嵌的 hello
muos> exec hello.elf      # 从 RAM FS 读取并运行同一个 ELF
muos> run fault           # 故意写 0xC0000000,验证用户态 Page Fault 只杀进程
```

hello 演示:CS 寄存器(确认 0x1B)、argc/argv、C 计算、brk 堆、`uname`、RAM FS 打开/读取/创建/写入/回读、匿名 `mmap`、键盘 `read`、正常 `exit`。

- 用户地址空间是固定的 8MB 沙箱 `0x08048000`(经典的 Linux i386 ET_EXEC 地址),从物理分配器整段预留并置 U/S 位;内核其余页保持 supervisor,用户无法读写内核。
- 进入 Ring3 使用 TSS.esp0 切换内核栈;`int 0x80`(DPL3 门)按 Linux i386 ABI 分发(eax=号,ebx/ecx/edx 传参);退出/故障时恢复 `user_enter` 保存的内核上下文返回 Shell。
- 已实现调用:`exit(1) read(3) write(4) open(5) close(6) lseek(19) getpid(20) getuid(24) brk(45) getgid(47) geteuid(49) getegid(50) getppid(64) old_mmap(90) munmap(91) uname(122) mprotect(125) mmap2(192)`;未知调用返回 `-ENOSYS` 并在串口记录编号,便于用真实二进制驱动后续补齐。
- 文件 fd 从 3 开始,映射 RAM FS 槽位并维护字节偏移;`open` 支持 `O_CREAT/O_TRUNC`。`mmap` 只支持 `MAP_PRIVATE|MAP_ANONYMOUS`(沙箱 bump 分配,文件映射/`MAP_FIXED` 拒绝)。
- 沙箱要求物理 RAM > 128MB;ISO 路径因无 multiboot 信息回退 32MB,`run`/`exec` 会明确提示内存不足。

## 自检套件

内核启动时在进入 Shell 前自动运行 `tests_run()`(src\test.c,VGA + 串口双输出,失败会打印 `SELF-TEST FAILED`),覆盖:

- 物理分配器:分配/释放计数、多页连续分配
- 内核堆:16 字节对齐、哨兵写入、kcalloc 零化、100 块随机分配释放、双重释放防护、超界请求返回 NULL
- 分页:4MB 以上帧的写读 + `mm_virt_to_phys` 恒等校验
- mmap:位图覆盖加载器报告的全部 RAM
- fs / vga / keyboard / pit / task
- RAM FS 偏移 API:write_at 追加、fd size、read_at 偏移校验、truncate
- 内嵌 ELF:存在性、ELF32/i386/ET_EXEC 头校验、入口点位于用户沙箱

## 踩坑备忘

| 问题 | 原因 | 解决 |
|------|------|------|
| QEMU 报 `Error loading uncompressed kernel without PVH ELF Note` | 新版 QEMU 不能 `-kernel` 加载 multiboot2 ELF | 用 boot.s 的 Multiboot1 裸 ELF 方案 |
| ISO 启动黑屏/三重故障 | multiboot 头被链接到镜像最前,0x100000 处是魔数不是代码 | boot.s 已改为入口 `jmp` 跨头;头仍在前 8KB 内 |
| 链接神秘报 `undefined reference to memcpy/memset` | `-ffreestanding` 下 GCC 仍会为结构拷贝/清零隐式生成这两个调用 | 提供 string.c(标准名实现) |
| string.c 自己的循环被编译成调用自己 | GCC 在 -O2 可能将字节循环识别为 memcpy/memset 模式 | 全局加 `-fno-tree-loop-distribute-patterns` |
| 中断处理里裸 `sti` 破坏调用方临界区 | cli/sti 应成对保存/恢复 EFLAGS(pushfl/popfl),而不是盲目开中断 | keyboard.c 的 kbd_lock/kbd_unlock 模式 |
| 物理分配器发 4MB 以上帧却 Page Fault | 分页只恒等映射了前 4MB(旧版) | mm_init 现在恒等映射全部 RAM |
| multiboot mmap 字段读出来是垃圾 | mmap_length/mmap_addr 在偏移 44/48,截断的结构体少定义了 syms[4] | 用 include\multiboot.h 的完整定义 |
| panic 寄存器全是乱码 / 中断随机错派甚至野跳转 | `registers_t` 字段序与 isr_stubs.s 压栈序不一致(段寄存器压栈序 ds→es→fs→gs,gs 在最低地址) | 结构体必须按 gs,fs,es,ds 开头,与汇编严格对齐;irq 分发处再加 `irq < 16` 边界防护 |
| 下载 qemu-w64-setup-20250424.exe 404 | 上游只保留最近几个版本 | setup-tools.ps1 已指向当前版本;旧版本号需手动更新 |
| 进入 Ring3 第一条指令就 #GP | `iret` 不会重载 DS/ES/FS/GS,仍留着 DPL0 的 0x10 | `user_enter` 在 iret 前把 DS/ES/FS/GS 置 0x23,返回时恢复 0x10 |
| `int 0x80` 里 read 键盘死锁 | 中断门自动清 IF,键盘环形缓冲永远进不了数据 | `keyboard_getchar` 内部 `sti; hlt; cli` 等待(本次直接复用) |
| 用户页置了 PTE_USER 仍不可访问 | U/S 检查同时看 PDE,页目录项还是 supervisor | `mm_user_map_range` 同时置 PDE_USER/PTE_USER 并重载 CR3 |
| 用户程序退出后内核栈/寄存器错乱 | 同特权级 iret 不会恢复 ESP/SS | `user_enter` 保存 callee-saved + ESP,`user_return_to_kernel` 恢复后 ret |
| 用户 ELF 地址与内核物理页冲突 | 恒等映射下 VA=PA,沙箱页必须先物理预留 | `mm_reserve_range` 一次性占用 8MB 沙箱,分配器不再发放 |
| 编辑器底栏右侧出现别的字符串 | `draw_help` 按 i=0..78 无条件索引字符串,越过 `\0` 读到 .rodata 下一条字面量 | 先 `strlen`,超界补空格(见 editor.c) |
| 键盘/鼠标中断偶发丢事件 | kbd_irq_handler 和 irq.c 都发 EOI(双重 EOI 会提前放行低优先级 IRQ) | EOI 只在 irq.c 发一次;IRQ12 走同一 8042 路由并解蔽 IRQ2 级联 |
| 鼠标数据被当键盘扫描码 / 鼠标无反应 | 8042 输出缓冲共用,状态位 bit5 才标识 aux;未解蔽 IRQ12/IRQ2 | keyboard.c 统一 drain,按 bit5 分流 `mouse_handle_byte`;初始化时同步读掉 ACK |
| 任务栏/窗口阴影写坏下一行 | `fd/ft` 未裁剪,最大化窗口的 x+w+1 越过 79 会回卷到下一行 | 所有绘制统一裁剪到 0..79/0..24,阴影不覆盖任务栏 |

## 版本

当前 **v0.4**:在 v0.3(启动/中断/内存/单任务 Shell)基础上加入 Ring3 用户态、TSS、Linux i386 `int 0x80` 调用与 ELF32 加载器;随后扩展到 RAM FS 16×64KB、`exec hello.elf`(从 FS 加载 ELF)、文件 fd(open/close/lseek/read/write)、`uname` 与匿名 `mmap`。首个用户 C 程序 `user/hello.c` 可由 `run` 或 `exec hello.elf` 启动。

桌面侧同步完善:PS/2 鼠标真实驱动 + 文本光标、窗口点击/拖拽/最小化/最大化/关闭、z-order、开始菜单鼠标操作、CMOS RTC 实况时钟、聚焦后键盘交互的 Calculator;编辑器修复 1KB 存盘截断并支持 PgUp/PgDn;自检 57 项。

## 参考资源

- [OSDev Wiki](https://wiki.osdev.org/) — 交叉编译器、Multiboot、VGA、PIT、PS/2
- [Multiboot 0.6.96 规范](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [Linux x86 boot protocol / i386 System V ABI](https://refspecs.linuxbase.org/elf/abi386-4.pdf) — Linux ABI 兼容工作的规范基础
- 代码细节与设计意图:[CODE_LOGIC.md](CODE_LOGIC.md);演进计划:[ROADMAP.md](ROADMAP.md)
