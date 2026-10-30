# MuOS 演进路线图

目标:**运行真实的静态链接 i386 Linux 二进制**(Linux ABI 兼容),保持 32 位 x86。NT/Win32 兼容作为长期远景(见末节)。

本文描述的是已确认的架构方向;每阶段的具体设计在动工时再细化,避免过早设计。

```
阶段 0(已完成)            阶段 1                  阶段 2                  阶段 3                  阶段 4
启动/中断/内存/堆    →     higher-half 内核   →    int 0x80 syscall   →    ELF32 加载器      →     运行 Linux 二进制
多任务地基 + mmap          Ring3 用户态             基础 POSIX 语义           fork/execve              (busybox/hello)
```

## 当前进度快照(2026-10-30)

- 内核自检 **88/0**;`build/verify_release.py` 的 kernel 与 ISO 两条路径均 PASS。
- `src/syscall.c` 已定义 **120+ 个 Linux i386 系统调用号**,覆盖进程/stdio、信号、文件与目录(`*at`)、管道、poll/select、mmap(匿名+文件)、stat/statfs、时间/RTC、rlimit、chown/chmod、nanosleep。
- 真实 Alpine 静态 i386 BusyBox 1.36.1(musl)可运行:`ls/cat/ps/free/df/dd/mkdir/touch/cp/mv/rm/chown/sleep/ulimit/sh` 与 shell 管道实测通过。
- 验证用例:`run`(内嵌 ELF)、`run pie`(ET_DYN/PIE)、`exec hello.elf`、`run fork/pipefork/execbin/sched`、`run sig`(信号)、`run thread`(CLONE_VM 线程)、`run blkpoll`(阻塞 poll)、`run at/mmapfile/poll/extra/p1`、`run ata` + `run ataread`(两次启动验证磁盘持久化)。
- 进程模型:每进程页目录 + 独立 8MB 物理块、Ring3 抢占式轮转调度、`clone` 线程(共享地址空间/fd 表)、信号投递(pending/blocked、handler + sigreturn trampoline、默认动作)、阻塞 `wait4/waitid/poll/read`。
- 仍有限制:higher-half(0xC0000000)未做;无完整动态链接器(仅支持静态 PIE + 重定位,PT_INTERP 只检测并拒绝);磁盘只到块设备层,无持久化文件系统;无网卡/socket ABI;无帧缓冲图形。

## 后续计划(建议顺序)

### P1 收尾 ABI 小缺口(低风险,继续按真实二进制驱动)
- [x] 目录 `rename/renameat`(源为目录时整棵子树改名,拒绝移入自身)
- [x] fs fd 引用计数(`unlink` 后数据保留到最后一个 fd 关闭,`dup/fork` 快照计数)
- [x] `waitid(284)`(SIGCHLD/CLD_EXITED siginfo,支持 WNOHANG)
- [x] `/proc/stat` 全量字段(`cpu/cpu0/intr/ctxt/btime/processes/procs_running/blocked`),BusyBox `top -b -n 1` 实测通过
- [x] `poll` 的真正阻塞等待(`PROC_WAIT_POLL` + 唤醒方按保存的 fd 表填充 `revents`,PIT 到期唤醒;无其他可运行进程时退回 `sti; hlt` 有界等待)

### P2 进程模型升级(结构性)
- [x] 每进程页目录 + 独立 8MB 物理块(CR3 切换,块/页目录按引用计数共享)
- [ ] higher-half 内核(0xC0000000):仍需改 linker.ld 与全量映射策略
- [x] 抢占调度:Ring3 进程 100Hz 轮转,`waitpid` 阻塞让出 CPU(内核 shell 不参与调度)
- [x] `clone` 线程(`CLONE_VM/CLONE_FS/CLONE_FILES/CLONE_THREAD/CLONE_SETTLS`)、每线程内核栈与 TLS
- [x] 信号投递:pending/blocked 掩码、`signal/sigaction/rt_sigaction/sigprocmask`、handler + sigreturn trampoline(或 libc restorer)、默认终止动作(SIGKILL/SIGTERM 等)、`alarm`
- [x] 阻塞 `read`(pipe/console)与阻塞 `poll`,均由写端/键盘 IRQ/PIT 唤醒

### P3 动态链接与更大程序
- [x] `ET_DYN`/静态 PIE 加载:load bias、`PT_DYNAMIC`、`R_386_RELATIVE/GLOB_DAT/JMP_SLOT/32` 重定位
- [x] 完整 auxv(`AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY`);`run pie` 实测 `PIE-RELOC-OK`
- [x] `PT_INTERP` 检测与明确拒绝(无 ld-musl 可用)
- [ ] 共享库(`ET_DYN .so`)映射 + 动态链接器(需要可用的 ld.so 与 `DT_NEEDED` 符号解析)

### P4 平台外设
- [x] ATA PIO(primary master,28-bit LBA)块设备 `/dev/hda`,读/写/部分扇区 RMW;`build/test_ata.py` 两次启动验证持久化
- [ ] 在块设备上建立持久化文件系统(MuFS:超级块 + 文件表 + 目录),当前 RAM FS 重启即失
- [ ] 网络(NE2000/RTL8139)+ ARP/IP/UDP + socket ABI
- [ ] 图形帧缓冲:当前 multiboot `-kernel` 与自研 ISO 引导均未设置 VBE 线性帧缓冲,需引入 GRUB/VBE 引导阶段或 BIOS 模式切换

## 阶段 0:地基(已完成,2026-09)

- [x] multiboot mmap(E820)解析,物理位图分配器边界修复
- [x] 全 RAM 恒等映射(消除分配器/分页 4MB 失配)
- [x] 内核堆 kmalloc/kfree/kcalloc
- [x] 调度器死代码清理,task_t 入堆;IDT 256 项全覆盖(0x80 现为系统调用门)
- [x] 公共字符串库(标准名,防 freestanding 隐式调用链接失败)
- [x] 键盘/中断 EFLAGS 纪律;自检套件扩到 46 项(含 >4MB 分页、堆、mmap;当前 88 项)
- [x] ISO 启动路径修复(入口=镜像首字节);构建脚本零硬编码路径

## 阶段 1:内核与用户态隔离(基本完成)

目标:内核搬到 3GB+ 高地址,进程跑在 Ring 3,具备真正的上下文切换。

- [ ] higher-half 内核:linker.ld 改虚拟地址 0xC0000000,低 1GB 留给用户空间;启用全局页
- [x] 每进程页目录;内核映射区在所有页目录中共享(拷贝内核 PDE;用户窗口 PDE 32–34 私有)
- [x] TSS(仅 ESP0/SS0,单 CPU 不需要任务门);GDT 中加载
- [x] 系统调用入口桩:`cli → int 0x80 → 切换内核栈(从 TSS)→ 保存用户寄存器`
- [x] Ring3 进入/退出:`iret` 到用户代码段 0x1B/数据段 0x23,时钟中断可从 Ring3 正常返回
- [x] `fork`:复制 8MB 物理块 + fd/cwd/brk 快照,子进程立即 READY,由抢占调度并发运行
- [x] 抢占调度:IRQ0 每 tick 在 Ring3 进程间轮转,阻塞的 `wait4/waitid/poll/read` 让出 CPU
- 验收:`run sched` 两个 Ring3 子进程输出交错、`run thread` 两个线程共享全局变量

**2026-10 进展**:首个 Ring3 C 程序(`user/hello.c`)已跑通 —— 用户窗口固定为 0x08048000 起 8MB,但**每个进程有自己的 8MB 物理块与页目录**,CR3 切换而不是搬移快照;`run fork` 父 fork → 子 execve → 子 exit 42 → 父 waitpid 收尸;`run sched` 两个子进程被 100Hz tick 抢占交错执行,`run thread` 验证 `clone(CLONE_VM)` 线程共享地址空间。higher-half 仍是剩余项。

## 阶段 2:系统调用框架(基础调用已跑通)

目标:稳定的内核/用户边界,i386 Linux syscall 语义。

- [x] int 0x80 分发器:eax=调用号,ebx/ecx/edx 传参,返回值 eax;错误约定 `-errno`;未知号返回 `-ENOSYS` 并串口记录
- [x] 第一批中的 `exit(1) read(3) write(4) open(5) close(6) getpid(20) getuid(24) brk(45)`;`fork(2)`、`waitpid(7)/wait4(114)`、`execve(11)` 已实现(每进程地址空间 + 抢占调度)
- [x] 用户态堆:brk 与匿名 mmap 在沙箱内 bump(页级 vmm 分配待做);文件 mmap 已支持(见阶段 4)
- [x] poll/select 就绪语义:`poll(168)/ppoll(309)/select(82)/_newselect(142)/pselect6(308)` 返回当前可读/可写状态;`poll`/`ppoll` 真正阻塞,由 pipe 写端/键盘 IRQ/PIT 唤醒(唤醒方按目标进程保存的 fd 表填 `revents`)
- [x] 用户 fd 表:stdio 0/1/2 + 文件 fd 从 3 起,映射 RAM FS 槽位并维护偏移;随进程切换保存/恢复(`proc_fd_state`),线程可经 `CLONE_FILES` 共享
- [x] fs 句柄引用计数(open 计数/close 释放),防止文件被删除后 fd 悬空
- [x] 信号:pending/blocked 掩码、handler + sigreturn、默认终止动作;`wait4/poll/read` 可被信号打断返回 EINTR
- 验收:`user/hello.c` 已通过 int 0x80 打印字符串、读键盘、文件读写、uname、mmap 并正常退出;下一步用真实 i386 二进制补调用清单

## 阶段 3:ELF32 加载器与进程模型

目标:从"文件系统里的 ELF"创建进程。

- [x] ELF32 头解析:program headers、p_type=LOAD 段载入沙箱、bss 清零;支持 ET_EXEC 与 ET_DYN(PIE,带 load bias 与动态重定位)
- [x] 初始栈:argc/argv 按 System V i386 布局压栈,`_start` 进入 `main`
- [x] ELF 来源:内核内嵌 blob + RAM FS 文件 + Multiboot `-initrd` 模块(`execmod`)
- [x] `execve` 系统调用:预校验 ELF → 清空沙箱 → 重载段/栈,保留 fd/cwd/brk,同一 pid 原地替换
- [x] 命令行参数传入(argv 数组/字符串从用户空间拷贝);stdin/stdout/stderr 由 fd 表预开
- [x] waitpid/exit 收尸:`PROC_ZOMBIE` + exit status 编码;父进程 `wait4` 运行子进程并回收
- [x] shell 改造:虚拟 `/bin/<applet>` → busybox 模块 + PATH 查找 + fork+execve;`sh -c 'ls'`、`echo hi | cat` 通过
- 验收:`run fork` 完成 fork→execve→exit→waitpid;`ps` 读取 `/proc/<pid>/stat|cmdline`

## 阶段 4:Linux ABI 兼容面

目标:运行真实 Linux 静态二进制(如 busybox i386 静态构建)。

- [x] 起步:`old_mmap/munmap(90/91)`(匿名 + 文件映射)、`uname(122)`、`lseek(19)`、uid/gid
- [x] 真实静态 i386 BusyBox 1.36.1 进入 Ring3 并运行 `echo`(i386 TLS、exit_group、uid32、ioctl、dup2、envp/auxv 栈)
- [x] BusyBox `ls/cat` 需求集:根目录 fd + `getdents(141)/getdents64(220)`、`stat/lstat/fstat(64)`、`statx(383)`、`access(33)`、`getcwd(183)`、`writev/readv(145/146)`、`rt_sigaction(174)`、`fcntl(64)`、`llseek(140)`、`sendfile(64)`、可关闭的 stdio fd
- [x] 扁平 VFS 文件变更:`unlink(10)`、`rename(38)`、`fs_rename`;`utimes/futimesat/utimensat` 校验路径后成功(时间戳不持久化)
- [x] 元数据/截断/同步:`chmod(15)/fchmod(94)`、`chown(16)/lchown(182)/chown32(212)/lchown32(198)`、`fchown(95/207)/fchownat(298)`、`readlink(85)`、`truncate(92/193)/ftruncate(93/194)`(增长零填充、缩小改 size)、`fsync(118)/fdatasync(144)/sync(36)`;`getrlimit(76)/ugetrlimit(191)/setrlimit(75)/prlimit64(340)`,`ulimit -n` 实测返回 11
- [x] 睡眠/统计:`nanosleep(162)`(`sti; hlt` 等 PIT tick)、`times(43)`、`getrusage(77)`;BusyBox `sleep 0.1` 与 `chown 0 /notes.txt` 实测通过
- [x] 文件 mmap:RAM FS 槽位与 `/proc` 快照按页拷入沙箱并零填充,`old_mmap` 字节偏移、`mmap2` 页偏移,`MAP_FIXED` 在沙箱内允许;映射是副本,`MAP_SHARED` 写回待做
- [x] 验收:`ls /`、`cat /notes.txt`、`pwd`、`id`、`uname -a` 与 20+ applet(含 `ls -l`、`wc`、`grep`、`sed`、`od`、`cp/rm/mv/touch`)全部通过;`ENOENT`/`ENOTDIR`/`EBADF` 路径也符合 Linux 行为
- [x] 设备与虚拟文件:`/dev/null` `/dev/zero` `/dev/console` 内核态字符设备;`/proc/{meminfo,uptime,version,cpuinfo,mounts}`、`/etc/mtab` 只读快照
- [x] `statfs/fstatfs(99/100)`、`statfs64/fstatfs64(268/269)`、`sysinfo(116)`;统一 0..10 fd 表后 `dup/dup2` 支持 stdio 重定向,`free/df/dd/stat -f` 实测通过
- [x] 进程 ABI:`fork(2)`、`waitpid(7)/wait4(114)`、`execve(11)`(每进程独立 8MB 物理块 + 页目录,抢占并发)
- [x] 管道 ABI:`pipe(42)/pipe2(331)`、fd 表 PIPE_READ/WRITE、shell 管道 `echo hi | cat`
- [x] 真正并发:`clone` 线程(CLONE_VM/FILES/THREAD/SETTLS)、信号投递、抢占调度、每进程页目录
- [x] 目录层级:`mkdir/rmdir`、子目录路径解析、直接子项枚举、跨目录文件 `rename`;`mkdir -p`/`cp`/`mv` 实测通过
- [x] `*at` 族:`openat(295)`、`mkdirat(296)`、`fstatat64(300)`、`unlinkat(301,含 AT_REMOVEDIR)`、`renameat(302)`、`readlinkat(305)`、`fchmodat(306)`、`faccessat(307)`;绝对路径/`AT_FDCWD`/目录 fd 三种解析,`AT_EMPTY_PATH` 支持;`run at` 用例与 BusyBox `mkdir/touch/ls/rm` 回归通过;目录 rename 已支持整棵子树改名
- [x] `/proc/<pid>/{stat,status,comm,cmdline}` 与动态 PID 目录;`ps` 显示 kernel/busybox
- [x] `/proc/loadavg`、`/proc/stat`、`/proc/self/maps` 最小实现
- [x] `vmstat/top` 所需的 CPU 统计字段(`/proc/stat` 全量,`top -b -n 1` 通过);内存统计已有 `/proc/meminfo`
- [x] errno 起步对齐(EBADF/EFAULT/EINVAL/ENOENT/ENOTDIR/EISDIR/ENOMEM/ENOSYS 等);`uname` 已回报 MuOS/muos/i386
- [x] `stat` 族返回 MuOS 假 identity:根目录 `S_IFDIR|0755`、文件 `S_IFREG|0644`、`st_dev=1`、块大小/块数按 64KB 文件计算
- [ ] errno 与 Linux 做一次全量审计(尤其 `statx` flags/mask 与 `sendfile` 边界)
- [x] RTC 读取(CMOS,桌面任务栏时钟);`time(13)`/`gettimeofday(78)`/`clock_gettime(265/403)` 返回真实 UTC epoch
- [x] RTC → epoch 换算(CMOS 年月日 + 世纪寄存器 + 闰年 civil-days 算法),RAM FS 记录 atime/mtime/ctime,`date/ls -l/stat/touch` 显示真实时间
- [ ] a.out/ELF 的零页(empty bad-address page,0x08048000 之前不可映射)与地址布局兼容
- [x] 方法学落地:每个真实二进制暴露的缺失 syscall 即刻补齐,串口只记录一次未知号;本轮 BusyBox 已驱动出完整 VFS 元数据面与 `/dev`/`/proc` 基础
- 验收:`echo/ls/cat/free/df/dd` 已通过;下一步以 `sh -c`、管道、`ps` 暴露的 `fork/pipe/proc` 需求作为清单

**方法学**:不要预先实现"完整 Linux"——用真实二进制当测试驱动,qemu 串口 + panic dump 定位缺失调用,逐个补齐。

## 阶段 5(远景):NT/Win32 兼容

仅记录依赖关系,不入排期。Linux ABI 优先的原因:规范公开、syscall 面小且稳定、有海量现成二进制可测;Win32 是用户态 API 家族(数百个 DLL 导出函数),必须先有进程/内存/句柄基础设施。

前置依赖:阶段 1–3 全部完成 + VFS 抽象层 + 句柄/对象管理器(Object Manager 语义)。

- PE/COFF 加载器(与 ELF 加载器并列的另一前端)
- Win32 子系统进程模型(csrss/服务于窗口的形态简化为单内核模块)
- 关键 DLL 子集:kernel32(进程/内存/文件)、user32/gdi32(接 win7.c 桌面)——工作量按 wine/reactos 经验是 Linux ABI 的一个数量级以上

## 附:近期工程债(不阻塞主线)

- [ ] fs 截断告警(fs_write 返回实际写入量,调用方检查)
- [ ] 内核堆 Grow(耗尽时向 vmm 要新页)
- [ ] higher-half 内核(0xC0000000),当前用户窗口 PDE 私有、其余 PDE 共享内核恒等映射
- [ ] 持久化文件系统(挂在 `/dev/hda` 上)、网卡与 socket ABI、VBE 帧缓冲(见 P4 说明)
- [ ] 完整动态链接器:共享库装载/符号解析/`DT_NEEDED`;当前只有静态 PIE + 自身重定位
- [ ] `MAP_SHARED` 写回文件;`munmap/mprotect` 仍是空操作
- [x] 真 PS/2 鼠标驱动(8042 aux 初始化、IRQ12 与键盘共享 drain、文本光标、点击/拖拽)
- [ ] panic 打印带 addr2line 说明(已加 -g)
- [ ] ISO 路径构造最小 mb_info(消除 EBX=0 的 32MB 回退)
- [ ] 多核/APIC(远期,单核不影响 ABI 兼容目标)
