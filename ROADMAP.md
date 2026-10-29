# MuOS 演进路线图

目标:**运行真实的静态链接 i386 Linux 二进制**(Linux ABI 兼容),保持 32 位 x86。NT/Win32 兼容作为长期远景(见末节)。

本文描述的是已确认的架构方向;每阶段的具体设计在动工时再细化,避免过早设计。

```
阶段 0(已完成)            阶段 1                  阶段 2                  阶段 3                  阶段 4
启动/中断/内存/堆    →     higher-half 内核   →    int 0x80 syscall   →    ELF32 加载器      →     运行 Linux 二进制
多任务地基 + mmap          Ring3 用户态             基础 POSIX 语义           fork/execve              (busybox/hello)
```

## 阶段 0:地基(已完成,2026-09)

- [x] multiboot mmap(E820)解析,物理位图分配器边界修复
- [x] 全 RAM 恒等映射(消除分配器/分页 4MB 失配)
- [x] 内核堆 kmalloc/kfree/kcalloc
- [x] 调度器死代码清理,task_t 入堆;IDT 256 项全覆盖(0x80 现为系统调用门)
- [x] 公共字符串库(标准名,防 freestanding 隐式调用链接失败)
- [x] 键盘/中断 EFLAGS 纪律;自检套件扩到 46 项(含 >4MB 分页、堆、mmap;当前 57 项)
- [x] ISO 启动路径修复(入口=镜像首字节);构建脚本零硬编码路径

## 阶段 1:内核与用户态隔离(基本完成)

目标:内核搬到 3GB+ 高地址,进程跑在 Ring 3,具备真正的上下文切换。

- [ ] higher-half 内核:linker.ld 改虚拟地址 0xC0000000,低 1GB 留给用户空间;启用全局页
- [ ] 每进程页目录;内核映射区在所有页目录中共享(拷贝内核 PDE 或递归映射法)
- [x] TSS(仅 ESP0/SS0,单 CPU 不需要任务门);GDT 中加载
- [x] 系统调用入口桩:`cli → int 0x80 → 切换内核栈(从 TSS)→ 保存用户寄存器`
- [x] Ring3 进入/退出:`iret` 到用户代码段 0x1B/数据段 0x23,时钟中断可从 Ring3 正常返回
- [ ] fork 雏形:复制页目录 + COW 可后置(先全量拷贝,教学内核可接受)
- 验收:两个用户进程(Ring3 汇编 + printf 到串口)并发运行不互相破坏

**2026-10 进展**:首个 Ring3 C 程序(`user/hello.c`)已跑通 —— 采用全 RAM 恒等映射下的 8MB 用户沙箱(0x08048000),先交付单进程用户态;higher-half、每进程页目录与多进程抢占留待后续。

## 阶段 2:系统调用框架(基础调用已跑通)

目标:稳定的内核/用户边界,i386 Linux syscall 语义。

- [x] int 0x80 分发器:eax=调用号,ebx/ecx/edx 传参,返回值 eax;错误约定 `-errno`;未知号返回 `-ENOSYS` 并串口记录
- [x] 第一批中的 `exit(1) read(3) write(4) open(5) close(6) getpid(20) getuid(24) brk(45)`;`fork(2) waitpid(7)` 待做
- [x] 用户态堆:brk 与匿名 mmap 在沙箱内 bump(页级 vmm 分配待做)
- [x] 用户 fd 表:stdio 0/1/2 + 文件 fd 从 3 起,映射 RAM FS 槽位并维护偏移;尚未挂进 task_t
- [ ] fs 句柄引用计数(open 计数/close 释放),防止文件被删除后 fd 悬空
- [ ] 基本信号占位(SIGKILL/SIGTERM 直接生效即可)
- 验收:`user/hello.c` 已通过 int 0x80 打印字符串、读键盘、文件读写、uname、mmap 并正常退出;下一步用真实 i386 二进制补调用清单

## 阶段 3:ELF32 加载器与进程模型

目标:从"文件系统里的 ELF"创建进程。

- [x] ELF32 头解析:program headers、p_type=LOAD 段载入沙箱、bss 清零;拒绝非 i386/非静态 ET_EXEC
- [x] 初始栈:argc/argv 按 System V i386 布局压栈,`_start` 进入 `main`
- [x] ELF 来源:内核内嵌 blob + RAM FS 文件;`exec hello.elf` 跑通
- [ ] `execve` 系统调用:销毁旧地址空间 → 建新页目录 → 载入段
- [ ] 命令行参数可配置传入;stdin/stdout/stderr 三 fd 预开
- [ ] waitpid/exit 收尸(僵尸态 + 简单 init 进程兜底)
- [ ] shell 改造:命令名 → /bin 查找 → fork+execve;ELF 支持磁盘/RAM FS 任意程序
- 验收:已跑通 `run`(内嵌)与 `exec hello.elf`(FS);下一步支持从 FS 运行任意 C 程序

## 阶段 4:Linux ABI 兼容面

目标:运行真实 Linux 静态二进制(如 busybox i386 静态构建)。

- [x] 起步:`old_mmap/munmap(90/91)`(仅匿名)、`uname(122)`、`lseek(19)`、uid/gid
- [ ] 继续补 busybox 启动需求集:文件 mmap、`writev(146)`、`rt_sigaction(174)`、`stat/fcntl/ioctl` 等
- [ ] a.out/ELF 的零页(empty bad-address page,0x08048000 之前不可映射)与地址布局兼容
- [ ] `/dev/null` `/dev/console` 设备文件;简单字符设备框架
- [x] errno 起步对齐(EBADF/EFAULT/EINVAL/ENOENT/ENOMEM/ENOSYS 等);`uname` 已回报 MuOS/muos/i386
- [ ] errno 与 Linux 做一次全量审计;补 `stat` 族返回的假 identity
- [x] RTC 读取(CMOS,桌面任务栏时钟);`time(13)`/`gettimeofday(78)` syscall 尚未接
- [ ] 时钟调用:`time(13)`/`gettimeofday(78)`(RTC → epoch 换算)
- 验收:busybox 静态 i386 能跑 `echo`/`ls`;此后每个真实二进制暴露的缺失 syscall 就是待办清单

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
- [x] 真 PS/2 鼠标驱动(8042 aux 初始化、IRQ12 与键盘共享 drain、文本光标、点击/拖拽)
- [ ] panic 打印带 addr2line 说明(已加 -g)
- [ ] ISO 路径构造最小 mb_info(消除 EBX=0 的 32MB 回退)
- [ ] 多核/APIC(远期,单核不影响 ABI 兼容目标)
