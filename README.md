# MuOS — 从零编写的 x86 32 位内核(Windows 本地开发)

在 Windows 上原生开发(不依赖 WSL / Linux)的教学型操作系统内核。当前版本包含:

- **完整启动链**:Multiboot1(QEMU `-kernel`)+ 自研 El Torito 光盘引导
- **硬件抽象**:GDT / IDT(256 项全覆盖)/ PIC / PIT / PS/2 键盘 + PS/2 鼠标 / 串口 / VGA 文本模式
- **内存管理**:multiboot mmap(E820)解析、物理页位图分配器、**higher-half 内核**(链接 0xC0100000 / 物理 0x100000,引导期自举页表,全 RAM 恒等映射 + 0xC0000000 别名)、内核堆(kmalloc/kfree/kcalloc)
- **Ring3 用户态**:TSS + `int 0x80` 系统调用(Linux i386 编号)+ ELF32 加载器,在 0x08048000 起的用户窗口内运行 C 程序(默认 8MB,`build.ps1 -SandboxMB` 可改为 8/12/16/32MB 等 4MB 倍数);每个进程有独立页目录(CR3 切换),**fork 为写时复制**(共享页只读,首次写才复制一页),100Hz 抢占轮转;ELF 可来自内核内嵌(`run`)、RAM FS(`exec hello.elf`)或 Multiboot 模块;支持 ET_EXEC 与 ET_DYN(PIE,`run pie`);用户态故障只杀进程,不 panic 内核
- **进程与信号**:`fork/execve/wait4/waitid`、`clone` 线程(`CLONE_VM/FILES/THREAD/SETTLS`,共享地址空间)、信号 pending/blocked 掩码与 `signal/sigaction/rt_sigaction/sigprocmask`、handler + sigreturn trampoline、默认终止动作、`alarm`;阻塞 `wait4/waitid/poll/read` 在管道/键盘/定时器事件上唤醒
- **Linux ABI**:退出/stdio、文件 fd、`brk`/匿名 `mmap`、`uname`、i386 TLS;多级 RAM FS(`mkdir/rmdir`、子目录路径解析)与普通文件:`open/read/write/lseek/close`、`getdents(64)`、`stat/lstat/fstat(64)`/`statx`、`access`、`getcwd`、`writev/readv`、`unlink/rename`、`utimes` 族;虚拟 `/dev/null|zero|console`、`/proc/{meminfo,uptime,version,cpuinfo,mounts,loadavg,stat,maps}`、`/proc/<pid>/{stat,status,comm,cmdline,maps}` 与 `/etc/mtab`,并由 `statfs/sysinfo` 支撑 `df/free`;`pipe/pipe2` 支撑真实 shell 管道,`/bin/<applet>` 映射到 BusyBox 模块;`/dev/hda` 提供 ATA PIO 块设备(跨启动持久化)
- **ATA 磁盘 + MuFS**:primary master PIO / 28-bit LBA,`/dev/hda` 字节级读写(部分扇区读改写);扇区 0 超级块、元数据表、16×64KB 数据槽,所有文件变更写穿、`fsync/sync` 全量刷新、开机自动挂载(无盘时退回纯 RAM FS);`tools/test_ata.py` 两次启动验证文件内容与裸扇区数据都保留
- **分区与真实文件系统**:`part.c` 解析 MBR 与 GPT(保护性 MBR → GPT 表项,过滤 Basic Data GUID),`fat.c` 读取 FAT32(8.3 + VFAT 长名、簇链、子目录)并可创建/覆盖 8.3 文件(分配/释放簇链、双 FAT 写穿),`ext2.c` 读取 ext2(超级块/组描述符/12 直接块 + 一级间接);检测到分区表时 MuFS 不再格式化该盘;MuFS 元数据提交带写前日志(扇区 9–15,挂载时重放未完成提交)。`tools/test_fs_read.py` 用生成的 MBR/GPT FAT32 与 ext2 镜像验证 `part/fatls/fatcat/fatwrite/ext2ls/ext2cat`,并二次启动验证写入持久化
- **外部 ELF / Multiboot modules**:`qemu -kernel ... -initrd program` 直接加载任意静态 ELF,Shell `mods` / `execmod <n> [args...]`;已验证 **Alpine 静态 i386 BusyBox 1.36.1** 可运行 `echo`、`ls /`、`cat /notes.txt`、`pwd`、`id`、`uname -a`、`wc`、`grep`、`head/tail`、`sort`、`sed`、`cut`、`od`、`md5sum`、`stat`、`du`、`date`、`cp/rm/mv/touch`、`free`、`df`、`dd`、`uptime`、`ps`,以及 **BusyBox shell 执行外部 applet 与管道**:`sh -c 'echo hi | cat'`、`sh -c 'ls | wc -l'`
- **RAM 文件系统**:16 文件槽 × 64KB + 16 目录槽(多级路径,足够存放静态 i386 ELF)+ 全屏文本编辑器
- **"MuOS 7" 桌面**:Win7 风格文本模式 GUI —— 任务栏 + 实况 RTC 时钟/内存、开始菜单、窗口 z-order、鼠标点击/拖拽/最小化/最大化/关闭、聚焦后键盘可交互的 Calculator
- **安全与电源**:SMEP(CPUID 可用时置 CR4 bit20)、弱 ASLR(栈顶 0..7 页随机 + `AT_RANDOM` 驱动库加载基址随机)、ACPI FADT 定位与 S5 关机(`poweroff`) / 8042 复位(`reboot`)
- **88 项内核自检**(启动时自动运行,或 Shell 中输入 `test`)

演进目标见 **[ROADMAP.md](ROADMAP.md)**:以 Linux ABI 兼容(int 0x80 + ELF32,运行静态 i386 Linux 二进制)为主线;NT/Win32 兼容为长期远景。

## 项目结构

```
muos/
├── src/             内核源码(29 个 .c + 3 个 .s)
│   ├── boot.s       Multiboot1 入口(入口=镜像首字节,jmp 跨过内联 multiboot 头)
│   ├── boot_cd.s    El Torito 光盘引导(实模式→VBE 线性帧缓冲→保护模式→搬运内核→跳转)
│   ├── isr_stubs.s  中断/异常/系统调用汇编桩 + Ring3 进入/返回
│   ├── kernel.c     内核主入口,初始化序列
│   ├── fb.c         VBE 线性帧缓冲文本控制台(8×8 字模、居中 80×25 网格)
│   ├── gui.c        像素桌面原型(渐变背景/窗口/任务栏/鼠标箭头,`pixel` 命令)
│   ├── mm.c         mmap 解析 / 位图分配器 / 分页 / 用户沙箱映射
│   ├── kheap.c      内核堆(first-fit 自由链表)
│   ├── mouse.c      PS/2 鼠标(8042 aux、IRQ12 共享路由、文本光标)
│   ├── rtc.c        CMOS 实时时钟(任务栏 HH:MM:SS)
│   ├── tss.c        TSS(esp0 = Ring3 进入 Ring0 的 8KB 栈)
│   ├── module.c     Multiboot -initrd 模块表/范围(外部 ELF 来源)
│   ├── elf.c        ELF32 加载器(ET_EXEC/ET_DYN、bias、动态重定位、PT_INTERP 解析)
│   ├── syscall.c    int 0x80 分发(stdio + 文件 fd + uname + mmap)
│   ├── user.c       用户地址空间、初始栈/auxv、进入/退出、故障收尸
│   ├── proc.c       进程表、页目录切换、抢占调度、信号、阻塞等待
│   ├── ata.c        ATA PIO 块设备(identify/read/write,28-bit LBA)
│   ├── part.c       MBR/GPT 分区表解析(保护性 MBR → GPT 表项)
│   ├── fat.c        只读 FAT32(8.3 + VFAT 长名、簇链、子目录)
│   ├── ext2.c       只读 ext2(超级块/组描述符/直接块 + 一级间接)
│   ├── pci.c        PCI 配置空间访问(0xCF8/0xCFC)+ 设备查找
│   ├── net.c        RTL8139 驱动 + ARP/IPv4/ICMP/UDP/TCP + DHCP + socket 表
│   ├── string.c     freestanding 字符串/内存函数(标准名)
│   ├── fs.c editor.c win7.c shell.c task.c test.c ...(fs:16×64KB)
│   │   editor: 128×79、10KB 存盘缓冲、PgUp/PgDn;win7: z-order/鼠标/RTC
├── user/            Ring3 C 程序(start.s / runtime.c / hello.c / syscall6.s)
│                    linker.ld(ET_EXEC)与 linker_pie.ld(ET_DYN/PIE);
│                    ldmu.c(ld-mu.so 动态链接器)、libmu.c(共享库 libmu.so)、
│                    dyn.c(PT_INTERP + DT_NEEDED 的测试程序)、
│                    tlslib.c(带 TLS 的共享库 libtls.so)、tls.c(TLS 测试)、linker_so/ld 脚本;
│                    hello 演示文件 syscall、uname、mmap、brk、信号、线程、阻塞 poll、PIE、ATA
├── include/         头文件(与 src 配对,muos_ 前缀 include guard)
├── build.ps1        一键构建 / 运行 / ISO / 清理(含用户程序构建与 ELF 嵌入)
├── tools/           构建/测试工具(make_iso.py、qmp_shell.py、test_busybox.py、
│                    test_ata.py、verify_release.py、screendump.py、
│                    make_fs_images.py、test_fs_read.py)
├── make_iso.py      Python + pycdlib 自制可启动 ISO(位于 tools\make_iso.py)
├── setup-tools.ps1  自动下载 NASM / QEMU 到 tools\
├── linker.ld        链接脚本(内核加载到 1MB)
├── run.ps1          构建并启动 QEMU(默认 ISO/VBE,支持 -Kernel/-Disk/-NoBuild)
├── run.bat          双击:构建 + QEMU 快速启动(文本模式)
├── run_iso.bat      双击:构建 + ISO 启动(VBE)
├── clean.bat        清理构建产物
└── CODE_LOGIC.md    代码逻辑详解(启动/中断/内存/用户态/Shell)
```

`tools\` 下的构建/测试脚本随仓库入库;`toolchain\` 与下载的 NASM/QEMU 二进制不入库。

## 快速开始

```powershell
# 环境初始化(自动下载 NASM + QEMU 到 tools\)
.\setup-tools.ps1

# 构建 build\kernel.elf(增量 + 并行,默认按 CPU 核数编译)
.\build.ps1

# 限制并行编译进程数
.\build.ps1 -Jobs 4

# 构建 + QEMU 运行(最快路径,VGA 文本模式);进入 Shell 后输入 run 运行内嵌 Ring3 C 程序
.\build.ps1 -Run

# 构建 + 生成 muos.iso + 光盘启动(VBE 帧缓冲 + 网卡,pixel 桌面需要这条路径)
.\build.ps1 -Iso -Run

# 日常启动脚本(默认 ISO:VBE + RTL8139,pixel 可用;会先做增量构建)
.\run.ps1
.\run.ps1 -Kernel        # 等价于 build.ps1 -Run(无 VBE)
.\build.ps1 -SandboxMB 16 # 用户沙箱改为 16MB(>=8 且为 4 的倍数)
.\run.ps1 -Disk          # 额外挂一个 build\ata_disk.img(ATA/MuFS 测试)
.\run.ps1 -NoBuild       # 不构建,直接启动已有产物

# 清理(只删构建产物,工具脚本在 tools\ 下不受影响)
.\build.ps1 -Clean
```

工具查找顺序:PATH → 仓库内 `toolchain\` / `tools\` → 常见安装位置(含 `X:\qemu`)。`run.bat` 走 `-Run`(文本模式),`run_iso.bat` 走 `-Iso -Run`(VBE,`pixel` 必须用后者或 `.\run.ps1`)。

### 自动测试

```powershell
# 通过 QMP 驱动内核 Shell(支持 run/exec 命令,自动等待 exit/fault 标记)
python tools\test_busybox.py "run net" "run tcp" "run dyn" "run tls"
# 两次启动验证 ATA 与 MuFS 持久化
python tools\test_ata.py
# MBR/GPT + FAT32(含长名/子目录)与 ext2 只读挂载
python tools\test_fs_read.py
# kernel + ISO 两条路径的发布冒烟
python tools\verify_release.py
# 启动 ISO、打开 pixel 桌面并抓屏(输出 build\pixel.ppm)
python tools\screendump.py
```

`test_busybox.py` 若发现 `build\busybox`(静态 i386 BusyBox ELF)会把它作为 `-initrd` 加载,否则只跑内核 Shell 中的内嵌测试程序。

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
| 光盘 `-cdrom` | El Torito → boot_cd.s | 自研引导扇区:实模式 → A20 → **VBE 800x600x32 线性帧缓冲** → 平坦 GDT → 保护模式 → 拷贝 flat kernel 到 0x100000 → 跳转 |

**布局约束**:内核入口 `start` 必须位于镜像第一个字节(boot.s 用 `jmp real_start` 跨过内联的 multiboot 头)。ISO 路径把 flat binary 拷到 0x100000 后直接跳过去,若 multiboot 头排在最前就会把魔数当指令执行。Multiboot1 规范只要求头在镜像前 8KB 内,`jmp` 跨头方案同时满足两条路径。

**Higher-half**:内核链接在 0xC0100000、物理装载在 0x100000(`AT(ADDR - 0xC0000000)`),ELF 入口 `entry_phys = 0x100000`。`boot.s` 在关分页的物理地址上先建自举页目录(identity 0..8MB + `PDE[768]` 映射物理 0..4MB),打开分页后用绝对地址跳到高半区;`mm_init` 再把全部 RAM 别名到 0xC0000000(1GB 上限恰好是 256 个 PDE)。内核代码、静态数据、页表/heap 都在高地址运行。

ISO 路径的引导器自建一个最小 Multiboot info(位于 `0x5600`):先用 BIOS `INT 15h/AX=E820` 收集物理内存图(最多 24 项,按 Multiboot entry 格式写到 `0x5800`),再填 `flags=MEM|MMAP`、`mem_lower/mem_upper`、`mmap_length/mmap_addr`,最后把 EBX 指向该结构。因此 ISO 路径与 `-kernel` 路径使用同一套 E820 解析;只有 BIOS 不支持 E820 时才回退 `mem_upper=32MB`。ISO 引导同时用 BIOS `INT 10h`(VBE 4F00/4F01/4F02)设置线性帧缓冲,并把模式描述符留在 `0x6F00`;内核启动后映射这段显存,把 80×25 文本网格(8×8 字模纵向放大 2 倍)居中画到 800×600。`-kernel` 路径没有 VBE,继续用 VGA 文本模式(两条路径共用同一套 `vga_*` 接口)。

## Ring3 用户程序

内核启动时把 `user/hello.c` 编译成静态 ELF32(i386,链接地址 `0x08048000`),再作为二进制块嵌进内核镜像。Shell 中输入:

```text
muos> run                 # 运行内核内嵌的 hello
muos> exec hello.elf      # 从 RAM FS 读取并运行同一个 ELF
muos> run at              # *at 族测试:mkdirat/openat/fstatat64/renameat/unlinkat
muos> run mmapfile        # 文件 mmap + truncate/chmod/fsync/getrlimit 测试
muos> run poll            # poll/select/pselect6/ppoll 就绪语义测试
muos> run extra           # nanosleep/readlink/chown/getrusage/times 测试
muos> run p1              # 目录 rename/fd 生命周期/poll HUP/waitid 测试
muos> run sig             # 信号:handler+sigreturn、阻塞掩码、SIGTERM/SIGKILL 默认动作
muos> run thread          # clone(CLONE_VM) 两个线程共享全局变量,waitpid 回收
muos> run blkpoll         # 真阻塞 poll:子进程写管道唤醒 / PIT 超时唤醒
muos> run pie             # ET_DYN(PIE) 镜像:内核 bias + R_386_RELATIVE 重定位
muos> run dyn             # 动态链接:PT_INTERP + 共享库 libmu.so(DT_NEEDED/符号解析)
muos> run tls             # 静态 TLS:initial-exec(TPOFF)+ global-dynamic(__tls_get_addr)
muos> run net             # 网卡:raw ICMP echo + UDP DNS 查询(QEMU -nic user,model=rtl8139)
muos> run tcp             # TCP:双连接回显 + getsockname/getpeername + listen/accept 自连接
muos> pixel               # 像素桌面原型(仅 ISO/VBE):渐变桌面、窗口、任务栏、鼠标箭头
muos> run fsata           # MuFS 持久文件:创建/写/fsync/回读(重启后 run fsread)
muos> run ata             # /dev/hda 写入并回读 PERSIST-DATA
muos> disk                # ATA 型号、扇区数与 MuFS generation
muos> part                # 打印 MBR/GPT 分区表(起始 LBA/扇区数/类型)
muos> fatls /             # 只读 FAT32 目录(8.3 + VFAT 长名/子目录)
muos> fatcat /HELLO.TXT   # 读取 FAT32 文件
muos> fatwrite /NEW.TXT OK # 创建/覆盖 FAT32 文件(8.3 名,写穿)
muos> ext2ls /            # 只读 ext2 目录
muos> ext2cat /hello.txt  # 读取 ext2 文件(直接块 + 一级间接)
muos> poweroff            # ACPI S5 关机(无 FADT 时用 QEMU 0x604/0xB004 端口)
muos> run fault           # 故意写 0xC0000000,验证用户态 Page Fault 只杀进程
```

hello 演示:CS 寄存器(确认 0x1B)、argc/argv、C 计算、brk 堆、`uname`、RAM FS 打开/读取/创建/写入/回读、匿名 `mmap`、键盘 `read`、正常 `exit`;`run at` 另外验证以目录 fd 为基准的 `*at` 路径解析,`run mmapfile` 验证文件映射与截断/同步/限额调用,`run poll` 验证 pipe/stdio 的读写就绪位,`run extra` 验证睡眠、资源统计与所有权调用,`run p1` 验证目录改名、打开后 unlink、pipe HUP、超时等待与 `waitid`。`run sig/thread/blkpoll` 覆盖 P2 的进程模型,`run pie` 覆盖 P3 的 ET_DYN 装载,`run dyn` 覆盖 P3 的动态链接(PT_INTERP → ld-mu.so → libmu.so),`run tls` 覆盖静态 TLS(主程序 local-exec、库 initial-exec 与 global-dynamic `__tls_get_addr`),`run ata` 覆盖 P4 的 ATA 块设备(配合 `build/test_ata.py` 做两次启动的持久化验证),`run net` 覆盖 P4 的网卡与 socket ABI(ICMP ping + UDP DNS),`run tcp` 覆盖 TCP 握手/双连接回显/真实 socket 名/accept 自连接,`pixel` 覆盖 VBE 像素桌面原型(仅 ISO 路径)。

- 用户地址空间固定为 `0x08048000` 起 8MB 的**虚拟**窗口(经典 Linux i386 ET_EXEC 地址),但每个进程拥有独立 8MB 物理块和页目录:分配时仅该窗口的 PDE 32–34 指向进程块,其余 PDE 与内核页目录共享恒等映射;调度器切换 CR3 即可切换进程。窗口最后一页是内核提供的只读 sigreturn trampoline(类似 vDSO)。
- 进入 Ring3 使用 TSS.esp0 切换内核栈;`int 0x80`(DPL3 门)按 Linux i386 ABI 分发(eax=号,ebx/ecx/edx 传参);Ring3 进程可被 IRQ0 抢占,`wait4/waitid/poll/read` 阻塞时让出 CPU;退出/故障时若无其他可运行进程则恢复 `user_enter` 保存的内核上下文返回 Shell。
- 已实现调用(按用途分组):
  - 进程/stdio:`exit(1)`、`exit_group(252)`、`fork(2)`、`vfork(190)`、`clone(120,支持 CLONE_VM/FILES/SIGHAND/THREAD/SETTLS/PTID/CTID)`、`waitpid(7)`、`wait4(114)`、`waitid(284)`、`execve(11)`、`pipe(42)`、`pipe2(331)`、`poll(168)/ppoll(309,阻塞)`、`select(82)`、`_newselect(142)`、`pselect6(308)`、`kill(37,真实投递)`、`getpid(20)`、`getppid(64)`、`gettid(224)`、`setpgid/getpgrp/setsid/getpgid/getsid`、`read(3)`、`write(4)`、`readv(145)`、`writev(146)`、uid/gid 及其 32 位变体(24/47/49/50、199-202)、`getgroups(80/205)`;`close(6)` 可关闭 0/1/2
  - 信号:`signal(48)`、`sigaction(67)`、`rt_sigaction(174)`、`sigprocmask(126)`、`rt_sigprocmask(175)`、`sigsuspend(130)/rt_sigsuspend(179)`、`sigreturn(119)/rt_sigreturn(173)`、`alarm(27)`;pending/blocked 位图 + handler + 默认终止动作
  - 文件/目录:`open(5)`、`close(6)`、`lseek(19)`、`llseek(140)`、`sendfile(187/239)`、`dup(41)`、`dup2(63,含 stdio 重定向)`、`fcntl(55/221)`、`access(33)`、`getcwd(183)`、`chdir(12)`、`mkdir(39)`、`rmdir(40)`、`unlink(10)`、`rename(38)`、`chmod(15)`、`fchmod(94)`、`chown(16)/lchown(182)/chown32(212)/lchown32(198)`、`fchown(95/207)`、`fchownat(298)`、`truncate(92/193)`、`ftruncate(93/194)`、`fsync(118)`、`fdatasync(144)`、`sync(36)`、`readlink(85)`;`*at` 族 `openat(295)`、`mkdirat(296)`、`fstatat64(300)`、`unlinkat(301,含 AT_REMOVEDIR)`、`renameat(302)`、`readlinkat(305)`、`fchmodat(306)`、`faccessat(307)`
  - 目录/元数据:`getdents(141)`、`getdents64(220)`、`stat/lstat/fstat(106-108)`、`stat64/lstat64/fstat64(195-197)`、`statx(383)`、`statfs/fstatfs(99/100)`、`statfs64/fstatfs64(268/269)`;`utimes(271)`、`futimesat(299)`、`utimensat(320)` 校验路径后返回成功(时间戳暂未持久化)
  - 内存/其他:`brk(45)`、`old_mmap(90)`、`mmap2(192)`、`munmap(91)`、`mprotect(125)`、`uname(122)`、`sysinfo(116)`、`set/get_thread_area(243/244)`、`modify_ldt(123,ENOSYS)`、`getrlimit(76)/ugetrlimit(191)/setrlimit(75)/prlimit64(340)`、`nanosleep(162)`、`times(43)`、`getrusage(77)`、`time(13)`、`gettimeofday(78)`、`clock_gettime(265/403)`
  - 设备:`/dev/hda`(ATA PIO 块设备,`read/write/lseek/fstat` 与 `S_IFBLK` 语义),`/dev/null|zero|console`
  - 未知调用返回 `-ENOSYS` 并在串口记录编号,便于用真实二进制驱动后续补齐。
- fd 表统一管理 0..10:`0/1/2` 初始为 stdin/stdout/stderr,`dup/dup2` 可把任意文件/设备 fd 重定向到 0..2(或反之);文件 fd 映射 RAM FS 槽位并维护字节偏移,目录 fd 保存绝对路径与目录项序号。文件槽位带引用计数:`unlink` 后数据保留到最后一个 fd 关闭,`dup/fork` 快照都会计数;目录 `rename` 会整棵子树改名(校验目标不存在、禁止移入自身)。
- `mmap` 支持匿名与文件映射:匿名走沙箱 bump 区;文件映射把 RAM FS/`/proc` 快照内容按页拷入沙箱并零填充尾部,`old_mmap` 偏移按字节、`mmap2` 偏移按页;`MAP_FIXED` 在沙箱范围内允许覆盖(动态加载器可自行选址)。映射是**内存副本**,`MAP_SHARED` 写回尚未实现。
- 动态链接:`PT_INTERP` 镜像由内核只映射段(不自行重定位),再把解释器 `ld-mu.so` 映射到 `0x08200000`、以 auxv 的 `AT_PHDR/AT_ENTRY/AT_BASE` 传给用户态链接器;`ld-mu.so` 按 `DT_NEEDED` 从 `/lib` 读 `.so`(SysV `.hash` **与 `.gnu.hash`** 均可解析符号)、应用 `R_386_RELATIVE/32/PC32/GLOB_DAT/JMP_SLOT/COPY/IRELATIVE`,最后跳到 `AT_ENTRY`。`run dyn` 会先把内嵌的 `ld-mu.so`/`libmu.so` 写入 MuFS 再运行(其中 libmu.so 使用 GNU hash 构建,用来覆盖 `.gnu.hash` 路径);解释器路径优先取 MuFS,缺失时回退到内嵌 blob。
- 静态 TLS(i386 变体 II):`ld-mu.so` 解析每个对象的 `PT_TLS`,在 `0x08780000` 的 TLS 区按“主程序最靠近 TP、其余库向下排列”布局,复制 `.tdata`、清零 `.tbss`,把自指针写入 `gs:0` 并通过 `set_thread_area(243)` 设置 GDT 槽 6、加载选择子 `0x33`。支持 `R_386_TLS_TPOFF/IE/GOTIE/LE/IE_32/LE_32/TPOFF32`(负偏移)与 `R_386_TLS_DTPMOD32/DTPOFF32`,并提供 `%eax` 传参约定的 `__tls_get_addr` 入口;`run tls` 实测 initial-exec 与 global-dynamic 两种模型(`TLS-OK`)。符号版本化(`.gnu.version`)仍未实现,也未用真实 glibc/musl `.so` 验证。
- 网络:`pci.c` 用 0xCF8/0xCFC 枚举并找到 RTL8139(10EC:8139),`net.c` 轮询 ISR 处理 32K RX 环与 4 个 TX 描述符;实现了 ARP 缓存/请求应答、IPv4 收发与校验和、ICMP echo 应答、UDP、**DHCP 客户端**(DISCOVER/OFFER/REQUEST/ACK,开机自动配置 10.0.2.x,失败时回退静态 10.0.2.15)以及**TCP**(SYN/SYN-ACK/ACK 握手、PSH 数据、FIN/ACK 关闭、**单在途段 RTO 重传 + 指数退避**、**慢开始/拥塞避免窗口**)和 127.0.0.1 回环;内核内置 port 7 echo 服务用于自测。socket ABI 提供 `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/shutdown/getsockname/getpeername`(以及旧式 `socketcall(102)`),fd 16–23 对应 8 个 socket 槽;`run net` 实测 ICMP ping 与 UDP DNS,`run tcp` 实测两条并发连接的握手/回显、真实 `getsockname/getpeername`,以及 `bind/listen/accept` 自连接的双向收发。`-nic user,model=rtl8139` 才会启用。
- 帧缓冲:ISO 引导在实模式设置 VBE 模式(优先 800×600×32,回退 1024×768×32 / 800×600×24/16),把 PhysBasePtr/分辨率/bpp/pitch 写到 `0x6F00`;内核在开分页后用 `mm_map_page` 以恒等、uncached 方式映射显存,`vga_*` 改为维护 80×25 影子单元数组并按 8×8 字模(纵向 2 倍)渲染;桌面/编辑器不再直接写 `0xB8000`,统一走 `vga_put_cell`。`-kernel` 路径无 VBE,自动保持 VGA 文本模式。
- 像素 UI 原型:`pixel` 命令用 `fb_fill_rect/fb_pixel/fb_text` 画渐变桌面、两个带标题栏的窗口、任务栏,并按 PS/2 鼠标位置绘制/擦除箭头光标(保存光标下像素再恢复);ESC/q 返回文本 shell(先保存再恢复 80×25 影子网格)。这是视觉原型,不是窗口管理器(无 per-window 缓冲、裁剪栈与控件)。
- `poll/select` 的**就绪检查**:文件/设备(含 `/dev/hda`)恒可读写,pipe 读端在缓冲区有数据时 `POLLIN`、无写端时 `POLLIN|POLLHUP`、写端在还有空间时 `POLLOUT`,stdin/console 仅在键盘缓冲已有字符时 `POLLIN`,坏 fd 返回 `POLLNVAL`。`poll/ppoll` 在用户进程里会真正阻塞(`PROC_WAIT_POLL`):唤醒方(pipe 写、键盘 IRQ、PIT 超时)用目标进程保存的 fd 表计算 `revents`,把结果写进它的内存并置 `eax`,再唤醒它;没有其他可运行进程时退回 `sti; hlt` 有界等待。`select/pselect6` 仍是即时扫描 + 有界等待,`pselect6/ppoll` 忽略 sigmask。
- 阻塞 `read`:pipe 读端为空且仍有写端、stdin/console 无字符时,用户进程阻塞在 `PROC_WAIT_READ`;写端写数据或 `close`(最后一次 `pipe_writer_add`)、键盘 IRQ 会替它完成读取(直接写进它的沙箱内存并置 `eax`)。所有阻塞点都可被信号打断,返回 `-EINTR`。
- 信号投递:每个进程有 `sig_pending/sig_blocked` 位图与 `signal/sigaction` 表;返回 Ring3 前或 tick 时投递,handler 运行在用户栈上的 sigframe,返回地址是内核窗口顶页的只读 trampoline(`mov eax,173; int 0x80`)或 libc 的 `SA_RESTORER`;`SIGKILL/SIGTERM/SIGINT/...` 默认终止(状态 128+sig),`SIGCHLD/SIGCONT/SIGWINCH` 默认忽略,`SIGKILL/SIGSTOP` 不可捕获/阻塞。
- 线程:脚本 `muos_thread_start(fn,arg,stack)` 调 `clone(VM|FILES|SIGHAND|THREAD)`,子线程在给定栈上 `call fn(arg)` 后 `exit`;地址空间/页目录按引用计数共享,线程退出只减引用,最后一个退出才释放;`gettid` 与 `getpid` 区分。
- `nanosleep` 在单进程/没有其他可运行进程时用 `sti; hlt` 等 PIT tick(100Hz),整机暂停到时间到期;`times` 返回 tick 数,`getrusage` 返回全零结构,`chown/lchown/fchown/fchownat` 只校验目标(不跟踪所有权),`readlink` 对非符号链接返回 `EINVAL`。
- `*at` 路径解析:绝对路径忽略 dirfd;相对路径在 `AT_FDCWD(-100)` 时按 cwd,在目录 fd 时拼接该 fd 的绝对路径;`fstatat64` 空路径 + `AT_EMPTY_PATH` 直接 stat dirfd,`unlinkat` 的 `AT_REMOVEDIR` 走 rmdir 语义。VFS 暂无符号链接,`readlinkat` 对已存在目标返回 `EINVAL`。
- 路径统一规范化:支持绝对/相对路径、`.` 与 `..`(父目录越界时停在 `/`)。命名空间 = 扁平 RAM FS 根目录 + `/dev`、`/proc`、`/etc` 虚拟目录;`open` 支持 `O_CREAT/O_TRUNC/O_DIRECTORY`。
- 沙箱要求物理 RAM > 128MB;ISO 路径现在通过自建 E820 info 获得真实内存图,内存足够时 `exec` 可正常运行,内存不足时才提示并优雅拒绝。沙箱大小在构建期可配(`-SandboxMB`,默认 8);fork 使用页级 COW(帧引用计数 + 写故障内存复制),但尚无按需分页/页面换出。

## 真实 Linux 二进制(BusyBox)

除内嵌/RAM FS ELF 外,内核支持 Multiboot modules,可直接把外部静态 ELF 作为 `-initrd` 传给 QEMU:

```powershell
qemu-system-i386 -kernel build\kernel.elf -initrd E:\github\muos\build\alpine\busybox -m 256M
```

```text
muos> mods
1 module(s)
  [0] 0x22B000-0x32B000  1049856B  E:\github\muos\build\alpine\busybox
muos> execmod 0 echo hi
hi
muos> execmod 0 ls /
hello.elf
notes.txt
readme.txt
muos> execmod 0 cat /notes.txt
MuOS 7 - x86 Microkernel
Built with GCC 15 + NASM
```

已验证对象是 Alpine v3.20 的 **busybox-static 1.36.1-r31(i386, musl 静态)**。
`busybox-static-1.36.1-r31.apk` 解包后的 `bin/busybox.static`。为跑通它补齐了 i386 TLS(`set_thread_area` + GDT slot 6)、`exit_group`、`set_tid_address`、uid32、`ioctl`、`dup2`、`envp`/`auxv` 初始栈,以及扁平 VFS 的 `getdents64`/`stat64`/`statx`/`writev`/`sendfile64` 等调用。

已实测 applet:`ls`(含 `-l`)、`cat`、`pwd`、`id`、`uname`、`wc`、`grep`、`head`、`tail`、`sort`、`sed`、`cut`、`od`、`md5sum`、`stat`、`du`、`date`、`cp`、`rm`、`mv`、`touch`、`chown`、`sleep`、`free`、`df`、`dd`、`uptime`、`ps`;不存在的路径会正确返回 `ENOENT`。

```text
muos> execmod 0 free
              total        used        free      shared  buff/cache   available
Mem:         262016       13776      248240           0           0      248240
muos> execmod 0 df
Filesystem           1K-blocks      Used Available Use% Mounted on
/dev/root                 1024       192       832  19% /
proc                      1024       192       832  19% /proc
devtmpfs                  1024       192       832  19% /dev
muos> execmod 0 dd if=/dev/zero of=/z.bin bs=16 count=1
1+0 records in
1+0 records out
```

注意:
- BusyBox 静态包约 1MB,OS 模块只接受外部 `-initrd`,不放进 64KB RAM FS。
- RAM FS 已支持多级目录(`mkdir/rmdir`、`mkdir -p`、跨目录 `cp/mv`),虚拟 `/dev`、`/proc`、`/etc`、`/bin` 只读;`*at` 族支持 dirfd 相对路径,目录 `rename` 整棵子树改名。进程是**每进程地址空间 + 抢占并发**:fork 复制 8MB 物理块,子进程立即运行;`clone(CLONE_VM)` 支持线程,`kill` 真实投递信号(含 SIGTERM/SIGKILL 默认终止)。
- BusyBox shell 已能通过虚拟 `/bin/<applet>` → BusyBox 模块执行外部 applet,并跑通管道:`sh -c 'echo hi | cat'`、`sh -c 'ls | wc -l'`(管道读端为空时会阻塞,直到写端写入或关闭)。`pipe/pipe2` 是 8 个 4KB 环形缓冲,管道写满会截断返回部分写入。
- 时间戳来自 CMOS RTC。无磁盘时文件系统是纯 RAM(重启丢失);带 `-drive` 时 MuFS 把命名空间与文件数据写到盘上,`date`/`ls -l`/`stat` 显示真实时间,`touch`/`utimensat` 会实际修改 atime/mtime 并持久化。
- MuFS 布局:扇区 0 超级块(`MUFS` 魔数 + 版本 + generation),扇区 2–8 文件/目录元数据表,扇区 16 起每槽 128 扇区(64KB)。每次 `create/write/truncate/delete/rename/mkdir` 都写穿元数据表,数据只写涉及的扇区;`fsync(118)/sync(36)` 触发全量刷新。磁盘首次使用时自动格式化。
- 首次运行会分配 257 页连续内存做模块副本;256MB 内存下正常。

## 自检套件

内核启动时在进入 Shell 前自动运行 `tests_run()`(src\test.c,VGA + 串口双输出,失败会打印 `SELF-TEST FAILED`),覆盖:

- 物理分配器:分配/释放计数、多页连续分配
- 内核堆:16 字节对齐、哨兵写入、kcalloc 零化、100 块随机分配释放、双重释放防护、超界请求返回 NULL
- 分页:4MB 以上帧的写读 + `mm_virt_to_phys` 恒等校验
- mmap:位图覆盖加载器报告的全部 RAM
- fs / vga / keyboard / pit / task
- RAM FS 偏移 API:write_at 追加、fd size、read_at 偏移校验、truncate
- RAM FS 重命名:改名后旧名消失、新名指向同槽、重名被拒绝、内容保留
- 时间:1970-01-01 / 2000-03-01(闰年)/ 2026-10-29 的 epoch 换算,以及 FS atime/mtime/ctime
- 进程表:pid/ppid 分配、`proc_find`、`proc_reap` 释放槽位;ELF 预校验接口
- 管道 ring buffer:create/write/fill/read 与 FIFO 顺序
- 多级目录:mkdir、子目录文件、子项枚举、非空目录 rmdir 拒绝、空目录删除
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
| Multiboot `-initrd` 的 ELF 魔数变成 BAD | 位图原本放在 `_kernel_end` 后,正好压掉 QEMU 紧贴内核放置的模块头 | 位图移入内核 BSS(`bitmap_storage`),mm_init 先预留模块页 |
| 真实 libc 启动时把字符串当指针,Page Fault | 初始用户栈缺少 envp NULL 与 auxv AT_NULL 终止符 | 按 Linux 布局压 `argc/argv/NULL/envp NULL/AT_NULL/strings` |
| BusyBox 启动即 GPF | musl 需要 i386 TLS:`set_thread_area` + DPL3 GDT 段 | GDT slot 6 TLS 描述符,返回 entry 6(选择子 0x33);`mov %gs` 可用 |
| BusyBox `ls` 退出 0 但没有任何输出 | musl stdio 用 `writev` 刷缓冲,未实现时整批输出丢失 | 实现 `writev(146)`/`readv(145)`,按 iovec 复用 do_write/do_read |
| `od` 读完后报 `standard input: Bad file descriptor` | od 会关闭 fd 0,而旧的 fd 表把 0/1/2 当作永远打开 | stdio 增加 open 状态;read/write/close/fstat/fcntl/statx 均检查 |
| musl 先调 `statx(383)` 再退回 stat64 | 新版 musl 的 `fstatat` 优先使用 statx | 按 256 字节 `struct statx` 填 basic stats,空路径支持 AT_EMPTY_PATH |
| `df` 只打印表头(`can't find mount point`) | BusyBox 跳过 `rootfs` 行,需要真实设备名 | `/proc/mounts` 与 `/etc/mtab` 输出 `/dev/root / ext2`、`proc /proc`、`devtmpfs /dev` |
| `dd` 报 `can't duplicate file descriptor` | dd 会 `dup2(if_fd, 0)`,旧实现把 0/1/2 当作固定 stdio | fd 表统一管理 0..10,`dup/dup2` 复制任意条目(含 stdio 重定向) |
| `ps` 打印表头后 Page Fault | `/proc/<pid>/stat` 字段数不足且缺少尾随空格,解析器越过字符串扫到 `USER_END` | 输出 Linux 的 52 字段并以空格结尾 |
| fork 子进程不并发,只在 waitpid 时运行 | 早期只有单地址空间,两个 8MB 沙箱无法同时映射 | 每进程 8MB 物理块 + 私有页目录(PDE 32–34)+ CR3 切换;tick 抢占调度 |
| 信号返回后进程崩溃/栈被踩 | 早期打算把 trampoline 也压进用户栈,handler 的向下栈增长会覆盖它 | 窗口最后一页映射只读 trampoline(vDSO 方案);sigframe 存在 handler 栈之上 |
| `clone` 线程一启动就 `ret` 到野地址 | 子线程的 esp 没有返回地址,直接复用父栈 | 汇编 `muos_thread_start` 在子栈上先放参数,子线程 `call fn(arg)` 后 exit |
| higher-half 内核一启动就三重故障 | e_entry 写成 0xC0100000,而装载器/CPU 关分页时跳到虚拟地址;或只映射了高半区却没保留 identity | `ENTRY(entry_phys)` 让 e_entry=0x100000;boot.s 保留 0..8MB identity 映射,再用绝对地址跳高半区;mm_init 建立全 RAM 的 0xC0000000 别名 |
| shell 管道子进程 `mov %gs:0` Page Fault | `user_enter_regs` 把 GS 重置为 0x23,丢失 0x33 TLS 选择子 | 保存/恢复子进程 FS/GS 选择子,并按进程保存 GDT TLS base |
| `sh -c ls` 报 `ls: not found` | BusyBox 通过 PATH 执行外部 applet,但没有 `/bin/busybox` | 虚拟 `/bin/*`、`/usr/bin/*`、`/sbin/*` 全部映射到 Multiboot module 0 |

## 版本

当前 **v1.2**:在 v1.1(mmap 解析、全 RAM 恒等映射、内核堆、寄存器布局修复)基础上加入 Ring3 用户态、TSS、Linux i386 `int 0x80` 调用、ELF32 加载器与 RAM FS 16×64KB,支持 `run`(内嵌 ELF)/`exec hello.elf`(FS ELF)、文件 fd(open/close/lseek/read/write)、`uname` 与匿名 `mmap`。首个用户 C 程序 `user/hello.c` 可由 `run` 或 `exec hello.elf` 启动。

桌面侧同步完善:PS/2 鼠标真实驱动 + 文本光标、窗口点击/拖拽/最小化/最大化/关闭、z-order、开始菜单鼠标操作、CMOS RTC 实况时钟、聚焦后键盘交互的 Calculator;编辑器修复 1KB 存盘截断与 Ctrl+S 失效,并支持 PgUp/PgDn;v1.2 时自检 57 项(现为 88 项)。

**v1.2 发布后主线进展**:Multiboot `-initrd` 外部 ELF → `mods`/`execmod` → i386 TLS/uid32/ioctl/dup2 → 真实静态 i386 BusyBox 跑通 `echo` → 扁平 VFS(根目录 fd、`getdents(64)`、`stat(64)`/`statx`、`writev`、`sendfile`、`unlink`/`rename`、可关闭的 stdio)→ 20+ applet 实测通过 → **虚拟 `/dev`+`/proc`+`/etc`、统一 0..10 fd 表、`statfs`/`sysinfo`**,`free`/`df`/`dd`/`uptime` 通过 → **CMOS RTC→epoch 与 RAM FS atime/mtime/ctime**,`date`/`ls -l`/`stat`/`touch` 显示真实时间 → **fork/execve/waitpid 进程模型 + `/proc/<pid>`**,`run fork` 完成父 fork → 子 execve `/hello.elf` → 子 exit 42 → 父 waitpid 收尸,`ps` 显示 kernel/busybox → **虚拟 `/bin/*`、`pipe/pipe2`、shell 管道、`/proc` loadavg/stat/maps**,`sh -c 'echo hi | cat'` 与 `ls | wc -l` 通过 → **多级 RAM FS(`mkdir/rmdir/mkdir -p`、跨目录 `cp/mv`、子目录枚举)** → **`*at` 族(dirfd 相对路径、`AT_EMPTY_PATH`、`AT_REMOVEDIR`)** → **文件 mmap、truncate/chmod/fsync、getrlimit/prlimit64** → **poll/select 非阻塞就绪语义** → **nanosleep/times/getrusage/chown 族/readlink** → **P1 收尾** → **每进程页目录 + 抢占调度** → **信号投递(pending/blocked + sigreturn trampoline)、`clone` 线程、阻塞 poll/read** → **ET_DYN/PIE 装载与重定位 + auxv** → **ATA PIO 块设备与跨启动持久化** → **higher-half 内核(0xC0100000,自举页表 + 全 RAM 别名)** → **MuFS 持久化文件系统(ATA 写穿 + 开机挂载)** → **动态链接:`PT_INTERP` + 用户态 `ld-mu.so`(`DT_NEEDED`/SysV hash 符号解析/共享库映射)** → **RTL8139 + ARP/IPv4/ICMP/UDP 与 socket ABI** → **VBE 线性帧缓冲文本控制台(ISO 引导 800×600×32)** → **DHCP 自动配置 + 最小 TCP(SOCK_STREAM)与回环** → **`.gnu.hash`/`R_386_COPY` 支持 + 像素桌面原型(`pixel`)**。下一步:TCP 重传/并发服务端、TLS 重定位、真正的图形窗口系统(见 ROADMAP)。

## 参考资源

- [OSDev Wiki](https://wiki.osdev.org/) — 交叉编译器、Multiboot、VGA、PIT、PS/2
- [Multiboot 0.6.96 规范](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [Linux x86 boot protocol / i386 System V ABI](https://refspecs.linuxbase.org/elf/abi386-4.pdf) — Linux ABI 兼容工作的规范基础
- 代码细节与设计意图:[CODE_LOGIC.md](CODE_LOGIC.md);演进计划:[ROADMAP.md](ROADMAP.md)
