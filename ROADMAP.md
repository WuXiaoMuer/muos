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
- [x] 调度器死代码清理,task_t 入堆;IDT 256 项全覆盖(0x80 预留)
- [x] 公共字符串库(标准名,防 freestanding 隐式调用链接失败)
- [x] 键盘/中断 EFLAGS 纪律;自检套件扩到 46 项(含 >4MB 分页、堆、mmap)
- [x] ISO 启动路径修复(入口=镜像首字节);构建脚本零硬编码路径

## 阶段 1:内核与用户态隔离

目标:内核搬到 3GB+ 高地址,进程跑在 Ring 3,具备真正的上下文切换。

- [ ] higher-half 内核:linker.ld 改虚拟地址 0xC0000000,低 1GB 留给用户空间;启用全局页
- [ ] 每进程页目录;内核映射区在所有页目录中共享(拷贝内核 PDE 或递归映射法)
- [ ] TSS(仅 ESP0/SS0,单 CPU 不需要任务门);GDT 中加载
- [ ] 系统调用入口桩:`cli → sysenter/int 0x80 → 切换内核栈(从 TSS)→ 保存用户寄存器`
- [ ] Ring3 上下文切换:iretd 到用户代码段(0x0B/0x13 已就绪),时钟中断抢占
- [ ] fork 雏形:复制页目录 + COW 可后置(先全量拷贝,教学内核可接受)
- 验收:两个用户进程(Ring3 汇编 + printf 到串口)并发运行不互相破坏

## 阶段 2:系统调用框架

目标:稳定的内核/用户边界,i386 Linux syscall 语义。

- [ ] int 0x80 分发器:eax=调用号,ebx/ecx/edx/esi/edi/ebp 传参,返回值 eax;错误约定 `-errno`
- [ ] 第一批调用号(Linux i386 编号对齐):`exit(1) fork(2) read(3) write(4) open(5) close(6) waitpid(7) brk(45) getpid(20) getuid(24)`
- [ ] 用户态堆:brk 基于 vmm(页粒度 + 内核堆管理小块)
- [ ] fd 表进 task 结构;fs 增加句柄语义(open/count 引用,close 释放)
- [ ] 基本信号占位(SIGKILL/SIGTERM 直接生效即可)
- 验收:用 ndisasm/手写汇编写一个 Ring3 程序,通过 int 0x80 打印字符串并正常退出

## 阶段 3:ELF32 加载器与进程模型

目标:从"文件系统里的 ELF"创建进程。

- [ ] ELF32 头解析:program headers、p_type=LOAD 段按页载入用户空间、bss 清零;拒绝非 i386/动态入口(暂只支持静态 ET_EXEC)
- [ ] execve:销毁旧地址空间 → 建新页目录 → 载入段 → 设入口栈(argv/envp 按 System V i386 布局压栈)→ iret 到 e_entry
- [ ] 命令行传递;stdin/stdout/stderr 三 fd 预开
- [ ] waitpid/exit 收尸(僵尸态 + 简单 init 进程兜底)
- [ ] shell 改造:命令名 → /bin 查找 → fork+execve(或先做简化版 exec-only)
- 验收:磁盘里放一个静态编译的 hello(i386),`hello` 命令跑起来

## 阶段 4:Linux ABI 兼容面

目标:运行真实 Linux 静态二进制(如 busybox i386 静态构建)。

- [ ] 逐个补 syscall 到 busybox 的启动需求集:`mmap/munmap(90/91)`、`uname(122)`、`writev(146)`、`rt_sigaction(174)`、`old_mmap` 变体等
- [ ] a.out/ELF 的零页(empty bad-address page,0x08048000 之前不可映射)与地址布局兼容
- [ ] `/dev/null` `/dev/console` 设备文件;简单字符设备框架
- [ ] errno 值与 Linux 对齐;uname 回报假 identity(机器名 "muos")
- [ ] 时钟调用:`time(13)`/`gettimeofday(78)`(需要 RTC 或 PIT 推算 epoch)
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
- [ ] 真 PS/2 鼠标驱动(注意 8042 键盘共存时序)
- [ ] panic 打印带 addr2line 说明(已加 -g)
- [ ] ISO 路径构造最小 mb_info(消除 EBX=0 的 32MB 回退)
- [ ] 多核/APIC(远期,单核不影响 ABI 兼容目标)
