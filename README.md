# MuOS — 从零编写的 x86 32 位内核(Windows 本地开发)

在 Windows 上原生开发(不依赖 WSL / Linux)的教学型操作系统内核。当前版本包含:

- **完整启动链**:Multiboot1(QEMU `-kernel`)+ 自研 El Torito 光盘引导
- **硬件抽象**:GDT / IDT(256 项全覆盖)/ PIC / PIT / PS/2 键盘 / 串口 / VGA 文本模式
- **内存管理**:multiboot mmap(E820)解析、物理页位图分配器、全 RAM 恒等映射分页、内核堆(kmalloc/kfree/kcalloc)
- **RAM 文件系统** + 全屏文本编辑器
- **"MuOS 7" 桌面**:Win7 风格文本模式 GUI(任务栏、开始菜单、可拖拽窗口)
- **46 项内核自检**(启动时自动运行,或 Shell 中输入 `test`)

演进目标见 **[ROADMAP.md](ROADMAP.md)**:以 Linux ABI 兼容(int 0x80 + ELF32,运行静态 i386 Linux 二进制)为主线;NT/Win32 兼容为长期远景。

## 项目结构

```
muos/
├── src/             内核源码(20 个 .c + 3 个 .s)
│   ├── boot.s       Multiboot1 入口(入口=镜像首字节,jmp 跨过内联 multiboot 头)
│   ├── boot_cd.s    El Torito 光盘引导(实模式→保护模式→搬运内核→跳转)
│   ├── isr_stubs.s  中断/异常汇编桩
│   ├── kernel.c     内核主入口,初始化序列
│   ├── mm.c         mmap 解析 / 位图分配器 / 分页
│   ├── kheap.c      内核堆(first-fit 自由链表)
│   ├── string.c     freestanding 字符串/内存函数(标准名)
│   ├── fs.c editor.c win7.c shell.c task.c test.c ...
├── include/         头文件(与 src 配对,muos_ 前缀 include guard)
├── build.ps1        一键构建 / 运行 / ISO / 清理
├── make_iso.py      Python + pycdlib 自制可启动 ISO
├── setup-tools.ps1  自动下载 NASM / QEMU 到 tools\
├── linker.ld        链接脚本(内核加载到 1MB)
├── run.bat          双击:构建 + QEMU 快速启动
├── run_iso.bat      双击:构建 + ISO 启动
├── clean.bat        清理构建产物
└── CODE_LOGIC.md    代码逻辑详解(启动/中断/内存/任务/Shell)
```

工具目录(均被 gitignore,不影响仓库体积):`toolchain\` 交叉编译器、`tools\` NASM/QEMU。

## 快速开始

```powershell
# 环境初始化(自动下载 NASM + QEMU 到 tools\)
.\setup-tools.ps1

# 构建 build\kernel.elf
.\build.ps1

# 构建 + QEMU 运行(最快路径)
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

## 自检套件

内核启动时在进入 Shell 前自动运行 `tests_run()`(src\test.c,VGA + 串口双输出,失败会打印 `SELF-TEST FAILED`),覆盖:

- 物理分配器:分配/释放计数、多页连续分配
- 内核堆:16 字节对齐、哨兵写入、kcalloc 零化、100 块随机分配释放、双重释放防护、超界请求返回 NULL
- 分页:4MB 以上帧的写读 + `mm_virt_to_phys` 恒等校验
- mmap:位图覆盖加载器报告的全部 RAM
- fs / vga / keyboard / pit / task

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
| 下载 qemu-w64-setup-20250424.exe 404 | 上游只保留最近几个版本 | setup-tools.ps1 已指向当前版本;旧版本号需手动更新 |

## 参考资源

- [OSDev Wiki](https://wiki.osdev.org/) — 交叉编译器、Multiboot、VGA、PIT、PS/2
- [Multiboot 0.6.96 规范](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [Linux x86 boot protocol / i386 System V ABI](https://refspecs.linuxbase.org/elf/abi386-4.pdf) — Linux ABI 兼容工作的规范基础
- 代码细节与设计意图:[CODE_LOGIC.md](CODE_LOGIC.md);演进计划:[ROADMAP.md](ROADMAP.md)
