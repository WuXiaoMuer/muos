# MuOS - 最小可启动内核（Windows 本地开发版）

在 Windows 上从零构建一个 x86 32位最小操作系统，不依赖 WSL / Linux。

## 项目结构

```
muos/
├── boot.s          ; 裸 ELF 启动入口 + 栈初始化（默认，QEMU -kernel 直接加载）
├── boot_grub.s     ; Multiboot2 启动入口（备用，需 grub-mkrescue 制作 ISO）
├── kernel.c        ; C 语言内核主程序，操作 VGA 文本缓冲区
├── linker.ld       ; 链接脚本，内核加载到 1MB 地址
├── grub.cfg        ; GRUB2 启动菜单配置
├── build.ps1       ; PowerShell 一键构建/运行脚本
└── README.md       ; 本文件
```

## 启动流程

```
QEMU -kernel (裸 ELF)  →  boot.s (设置栈)  →  kernel_main()  →  VGA 显示
```

> 默认方案不使用 GRUB，由 QEMU 直接加载 32-bit ELF。保留 `boot_grub.s` 供以后制作 GRUB ISO 使用。

## 环境准备（Windows）

需要安装 3 个工具：**NASM** + **i686-elf-gcc 交叉编译器** + **QEMU**

### 1. 安装 NASM（汇编器）

1. 访问 https://www.nasm.us/
2. 下载最新 Windows 安装包（如 `nasm-2.16.03-installer-x64.exe`）
3. 双击安装，**勾选"Add to PATH"**

### 2. 安装 i686-elf-gcc（交叉编译器）

这是最关键的一步。Windows 自带的 MinGW gcc 是为 Windows PE 格式设计的，不能直接编译裸机内核。

**推荐：使用 lordmilko 预编译工具链**

1. 访问 https://github.com/lordmilko/i686-elf-tools/releases
2. 下载最新版 `i686-elf-tools-windows.zip`
3. 解压到任意目录，例如 `C:\i686-elf-tools`
4. 将 `C:\i686-elf-tools\bin` 加入系统 PATH

> 该工具链包含：`i686-elf-gcc`、`i686-elf-ld`、`i686-elf-as` 等

**替代方案：MSYS2（如果你已安装）**

```bash
pacman -S mingw-w64-i686-elf-gcc
```

### 3. 安装 QEMU（虚拟机）

1. 访问 https://www.qemu.org/download/#windows
2. 下载 `qemu-w64-setup-xxxx.exe`
3. 双击安装，**勾选"Add to PATH"**

### 验证安装

打开 PowerShell，运行：

```powershell
nasm -v
i686-elf-gcc --version
qemu-system-i386 --version
```

三条命令都能显示版本号，说明环境就绪。

## 编译与运行

### 方式一：PowerShell 一键脚本（推荐）

在项目目录打开 PowerShell：

```powershell
# 仅编译
.\build.ps1

# 编译 + 直接运行（默认裸 ELF 方案，最快）
.\build.ps1 -Run

# 清理构建产物
.\build.ps1 -Clean

# 使用 GRUB/multiboot2 方案编译（需 grub-mkrescue 制作 ISO）
.\build.ps1 -Grub -Run

# 编译 + 制作 ISO + 运行（需要 grub-mkrescue，见下方说明）
.\build.ps1 -Grub -Iso -Run
```

### 方式二：手动命令

```powershell
# 编译汇编
nasm -f elf32 boot.s -o boot.o

# 编译 C 内核
i686-elf-gcc -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-stack-protector -c kernel.c -o kernel.o

# 链接为 ELF
i686-elf-ld -m elf_i386 -T linker.ld boot.o kernel.o -o kernel.elf -nostdlib

# 直接用 QEMU 运行 ELF（开发调试最快）
qemu-system-i386 -kernel kernel.elf
```

如果一切正常，你会看到 QEMU 窗口黑屏后显示：

```
============================
      MuOS Booted!
  Hello from Windows dev!
============================
```

## 制作可启动 ISO（可选）

制作 ISO 需要 `grub-mkrescue` 和 `xorriso`，这两个工具在 Windows 上没有原生版本，有以下方案：

### 方案 A：MSYS2（推荐）

1. 安装 [MSYS2](https://www.msys2.org/)
2. 在 MSYS2 UCRT64 终端中运行：

```bash
pacman -S grub xorriso
```

3. 将 MSYS2 的 `ucrt64\bin` 加入系统 PATH
4. 然后运行 `.\build.ps1 -Iso -Run`

### 方案 B：WSL（如果你改变主意）

```bash
sudo apt install grub-pc-bin xorriso
make iso
```

### 手动制作 ISO

```powershell
mkdir -p iso/boot/grub
cp kernel.elf iso/boot/
cp grub.cfg iso/boot/grub/
grub-mkrescue -o muos.iso iso
qemu-system-i386 -cdrom muos.iso
```

## 代码说明

### boot.s（默认）

- 裸 ELF 入口，QEMU `-kernel` 直接加载 32-bit ELF
- 设置 16KB 栈空间，调用 `kernel_main`
- 内核返回后执行 `cli; hlt` 永久停机

### boot_grub.s（备用）

- 定义 Multiboot2 header（魔数 `0xE85250D6`）
- 供 GRUB 引导使用，需 `grub-mkrescue` 制作 ISO

### kernel.c

- 直接操作 VGA 文本缓冲区（物理地址 `0xB8000`）
- 每个字符占 2 字节：`[属性字节][ASCII字符]`
- 默认属性 `0x07` = 黑色背景 + 浅灰色文字
- 包含简单的 `clear_screen`、`putchar`、`print` 函数

### linker.ld

- `ENTRY(start)` 指定入口点为汇编的 `start` 标签
- `. = 0x100000` 将内核加载到 1MB 处

## 进阶方向

这是 **第一阶段：Hello OS**。后续可以逐步添加：

- **GDT / IDT**：进入保护模式、设置中断描述符表
- **键盘驱动**：读取 8042 键盘控制器扫描码
- **定时器**：PIT 8253 可编程间隔定时器
- **内存管理**：物理页分配器、虚拟内存（分页）
- **简单 GUI**：绘制像素、矩形、窗口装饰
- **多任务**：上下文切换、进程调度

## 踩坑备忘

| 问题 | 原因 | 解决 |
|------|------|------|
| QEMU 报错 `Error loading uncompressed kernel without PVH ELF Note` | 新版 QEMU Windows 无法直接用 `-kernel` 加载 multiboot2 ELF | 改用裸 ELF 方案（boot.s），或制作 GRUB ISO |
| `grub-mkrescue` 找不到 | Windows 无原生版本 | 安装 MSYS2 或改用 `-kernel` 直接加载 ELF |
| QEMU 黑屏无输出 | multiboot header 错误 / 未对齐 | 检查 `boot_grub.s` 中 header 的魔数、校验和、结束标签 |
| 链接报错 `undefined reference to 'start'` | linker.ld 入口与汇编标签不匹配 | 确保 `ENTRY(start)` 和 `global start` 一致 |
| `i686-elf-gcc` 找不到 | 未加入 PATH 或下载了错误版本 | 使用 lordmilko 预编译包，确认 `bin` 目录在 PATH 中 |

## 参考资源

- [OSDev Wiki - GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler)
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [VGA Text Mode (OSDev)](https://wiki.osdev.org/VGA_Hardware)
