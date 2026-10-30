# MuOS 代码逻辑(v1.2)

本文对应重构后的代码现状:multiboot mmap 解析、全 RAM 恒等映射、内核堆、Ring3 用户态(int 0x80 + ELF32/ET_DYN)、每进程页目录 + 抢占调度、信号与 clone 线程、阻塞 poll/read、ATA 块设备、PS/2 鼠标 + RTC 桌面、88 项自检。演进计划见 ROADMAP.md。

## 一、总体结构

```
启动(汇编)          内核入口              硬件抽象                系统服务              用户可见
boot.s          →   kernel.c         →   gdt/idt/isr/pic/irq  →  mm.c  kheap.c  →  shell.c(31 命令)
boot_cd.s           初始化序列            pit keyboard mouse         string.c fs.c      editor.c
isr_stubs.s                              vga serial rtc ata         task.c test.c      win7.c(桌面)
                                                                      tss/elf/proc/syscall/user → user/hello.c
```

- `src/` 平铺 29 个 `.c` + 3 个 `.s`;`include/` 配对头文件,guard 统一 `MUOS_*` 前缀。
- `linker.ld`:内核加载到 `0x100000`,`_kernel_end` 供内存管理使用。
- 两条启动路径最终都把控制权交给 `kernel_main(magic, mb_info)`。

## 二、启动流程

### 2.1 boot.s(QEMU -kernel 路径)

```asm
start:          jmp real_start      ; 入口 = 镜像第一个字节(物理 0x100000)
align 4                             ; Multiboot1 头:magic/flags/checksum
                dd 0x1BADB002 ...   ; (在前 8KB 内,满足规范)
real_start:     cli
                mov esi, eax        ; 保存 multiboot magic
                ; 建自举页目录:PT0/PT1 identity 0..8MB,PDE[768]->PT0
                ; 填 PT、PDE[0]/[1]/[768],mov cr3, 打开 CR0.PG|PE
                mov eax, high_entry
                jmp eax             ; 绝对跳转到 0xC01xxxxx 继续
high_entry:     mov esp, stack_top  ; 16KB 内核栈(高地址)
                push ebx            ; multiboot_info
                push esi            ; 魔数
                call kernel_main
```

**为什么入口必须在第一个字节**:ISO 路径把 flat binary 拷到 0x100000 后直接 `jmp 0x100000`,若 multiboot 头排在最前,魔数 `02 B0 AD 1B` 会被当指令执行(旧版的真实故障)。`jmp` 跨头让两条路径共用同一镜像布局。

**higher-half 的入口约定**:链接地址 0xC0100000 但 ELF `e_entry` 必须是物理地址,链接脚本用 `entry_phys = ADDR(.text) - 0xC0000000`;boot.s 在低地址(identity 映射)执行完自举后,用绝对地址跳到高半区,后续所有 C 代码/静态数据都运行在 0xC0xxxxxx。

### 2.2 boot_cd.s(El Torito 光盘路径)

1. BIOS 加载引导扇区(boot.img 前半,由 make_iso.py 在偏移 4092 处补丁内核大小)到 0x7C00
2. 实模式:读光盘其余部分到 0x8C00 → 开 A20 → **VBE 4F00/4F01/4F02 设置线性帧缓冲(0x6F00 处留下描述符)** → 建平坦 GDT → 置 PE 进入保护模式
3. 把内核 flat binary 拷到 0x100000,远跳 `(0x08, 0x100000)`
4. **EBX = 0**(无 multiboot 信息):mm.c 检测后回退 mem_upper 默认值;内核据 0x6F00 的 `VBE1` 魔数启用帧缓冲(`-kernel` 路径没有该魔数,保持 VGA 文本)

### 2.3 kernel_main 初始化序列

```
serial → vga → GDT → IDT(256 项)→ PIC(重映射+全屏蔽)
→ modules_init(mb)                [Multiboot -initrd 模块表提前解析]
→ mm_init(mb, kernel_end)         [mmap 解析 + 位图 + 全 RAM 恒等映射 + 模块页预留]
→ mm_enable_paging()              [CR0.PG|WP]
→ fb_enable() + vga_repaint()     [仅 ISO 路径:映射 VBE 显存并重绘影子网格]
→ kheap_init()                    [1MB 内核堆]
→ tss_init()                      [TSS + 8KB Ring0 入口栈 + ltr]
→ PIT 100Hz + IRQ0 → ATA 探测(primary master)→ keyboard → mouse(8042 aux / IRQ12)
→ task_init + task_register("kernel")
→ tests_run()                     [88 项自检,失败打印 SELF-TEST FAILED]
→ 启动动画 → shell_run()(内核栈上)
```

## 三、中断体系

- **IDT 256 项全覆盖**:0–31 异常、0x20–0x2F IRQ、**0x30–0xFF 填充 `isr_ignore` 桩(纯 iret)**——未预期向量/伪中断安静返回,不会 #NF 级联。**0x80 覆盖为 DPL3 系统调用门**(flags 0xEE),入口 `syscall_entry`。
- **调用链**:`ISR/IRQ 桩(压栈)→ isr_common/irq_common(保存全寄存器+段)→ C handler → 恢复 → iret`。
- **irq.c**:分发到 irq_handlers[16](IRQ0 timer、IRQ1 键盘、IRQ12 鼠标共享 8042 drain),处理伪 IRQ7/15(ISR 寄存器检查),每 IRQ 只发一次 EOI。kbd_irq_handler 自己不再发 EOI(旧版双重 EOI 会提前放行低优先级中断)。
- **8042 路由**:键盘与鼠标共用输出缓冲,状态位 bit5 标识 aux;IRQ1/IRQ12 都进同一个 handler,按 bit5 分流到 `kbd_process_scancode` 或 `mouse_handle_byte`。IRQ12 必须先解蔽主片 IRQ2 级联。
- **isr.c**:CPU 异常一律致命 → 打印异常名 + EIP/CS/EFLAGS/通用寄存器 dump → 停机。`crash` 命令即走此路径。
- **EFLAGS 纪律**:内核里禁止裸 `cli…sti` 对(会破坏 IRQ 上下文和调用方临界区)。keyboard.c 的 `kbd_lock/kbd_unlock`(pushfl/popfl)是标准模式,新代码请照抄。

## 四、内存管理(mm.c)

### 4.1 mmap 解析

- `include/multiboot.h`:完整的 `multiboot_info_t`。**mmap_length 在偏移 44、mmap_addr 在偏移 48,中间隔着 syms[4]**——截断定义会读错位。
- Pass 1:遍历 mmap 条目(`p += entry->size + 4`),type==1 的区域取最高 end,clamp 到 `MM_MAX_RAM_MB`(1GB,32 位上限防护),得到 `ram_top`。
- 无 mmap(ISO 路径 EBX=0)→ 回退 `mem_upper + 1MB`;再无 → 默认 32MB。
- >4GB 条目:base 在 ram_cap 之外直接不计,跨越部分截断。

### 4.2 物理位图分配器

- 位图是内核 BSS 里的静态数组 `bitmap_storage`(不是放 `_kernel_end` 后):QEMU 会把 `-initrd` 模块紧贴内核镜像放置,运行时位图会覆盖模块头(ELF 魔数变 BAD)。**先全置 1(used),再把可用区间清 0**——1MB 以下传统区、BIOS/EBDA、mmap 空洞天然保持 reserved。位图尾部越界位也置 1(total_pages 非整除 32 时安全)。
- `mm_init` 在分配页目录/页表与内核堆之前,遍历 `modules_get()` 预留所有模块页;`modules_init()` 必须在 `mm_init()` 之前调用(此时分页未开、直接读物理地址)。
- `mm_alloc_page / mm_alloc_pages(count)`:first-fit;count==0/超总量/超空闲直接 NULL(**无符号下溢已修**)。
- `mm_free_page / mm_free_pages`:校验对齐与范围,坏指针静默忽略。

### 4.3 分页:全 RAM 恒等映射

```
mm_init 末尾:for pfn in [0, total_pages):
    若 PDE[pde] 无表 → mm_alloc_page() 新页表并清零
    PTE[pfn&0x3FF] = pfn*4096 | PRESENT|WRITABLE
```

- **自举为什么可行**:mm_init 在 boot.s 已打开自举分页之后运行;自举页表 identity 映射 0..8MB,而 first-fit 从内核镜像之后(≈1.4MB)升序发帧,页目录 + 全部页表都在 8MB 内,写完再切到新页目录(它把全 RAM identity + 0xC0000000 映射)。
- 这消除了旧版"分配器发放 4MB 以上帧但分页只映射前 4MB"的失配——任何越线访问立刻 Page Fault(当年任务栈交接崩溃的真实根因,详见 §六)。
- `mm_map_page` 返回 `bool_t`(拒绝重复映射/分配失败),`mm_virt_to_phys`/`mm_unmap_page` 是未来用户态的基础原语。
- **higher-half 别名**:映射完 identity 后,把 PDE[0..255] 原样复制到 PDE[768..1023](`0xC0000000 + phys` 指向同一物理页)。内核镜像链接在 0xC0100000,物理装载在 0x100000;boot.s 用自举页目录(identity 0..8MB + PDE[768] -> 物理 0..4MB)完成这段切换。RAM 上限 1GB 正好用满 256 个别名 PDE。
- 物理内存仍可通过 identity 地址直接访问(进程块 `p->mem`、页表、内核堆都返回物理地址),因此用户窗口占用的物理区间继续用 `mm_reserve_range` 预留。

### 4.4 内核堆(kheap.c)

- 区域:`mm_alloc_pages(256)` = 1MB,恒等映射保证可达。
- 结构:first-fit 自由链表;**16 字节块头**(magic 'KHEO'/size/used/pad),负载 16 字节对齐;分配按 16 取整;释放时前后向合并(O(n) 回扫,块数极少可接受)。
- 防护:magic 校验(野指针)、双重释放忽略、区间外指针忽略;耗尽返回 NULL(暂不 Grow,见 ROADMAP)。
- 使用者:`task_register`(替代每任务 4KB 整页浪费)。**非可重入**:IRQ 上下文使用需外面包 cli/sti。

## 五、字符串库(string.c)

标准名 `memset/memcpy/memmove/memcmp/strlen/strcmp/strncmp/strcpy/strncpy + u32_to_dec`:

- **名字是承重的**:`-ffreestanding` 下 GCC 仍会为结构拷贝/清零隐式生成 `memcpy/memset` 调用,缺标准名实现会链接失败。
- build.ps1 全局 `-fno-tree-loop-distribute-patterns`:防止 GCC 把 string.c 自己的字节循环优化成对 memcpy/memset 的调用(自递归)。
- 此前项目里有 4 套重复实现(fs.c str_*/mem_cpy、shell.c nts 系列、win7.c nts、test.c uitoa),已全部收敛。

## 六、任务模型(如实版)

- **内核 shell(pid 1)不参与调度**:它直接跑在内核栈上,PIT 中断只 tick + 驱动用户进程抢占;`task_t` 仅作 `tasks`/`ps`/自检的 introspection。
- **用户进程调度器**:`proc_t`(proc.c)固定 8 槽,每个进程/线程有独立 8MB 物理块 + 页目录 + 8KB TSS 内核栈;IRQ0 每 tick 调 `proc_sched_tick`,保存当前 Registers_t 现场并改写同一中断帧切换到下一个 READY 进程(`iret` 直接进入它)。无 READY 进程时,`proc_exit_current` 切回内核页目录并用 `user_kernel_esp_launch` 返回 shell。
- 阻塞点(`wait4/waitid/poll/read/sigsuspend`)把进程标为 BLOCKED 后让出;唤醒方(子进程 exit、pipe 写/关、键盘 IRQ、PIT)直接把结果写进阻塞进程的保存现场(它的用户内存通过 `p->mem + (uva - USER_BASE)` 访问),或标记 READY 由调度器恢复。
- GDT 槽 3/4 是 Ring3 代码/数据段(0x1B/0x23),槽 5 是 TSS(0x28),槽 6 是 TLS(0x33)。

## 七、Ring3 用户态与系统调用

### 7.1 沙箱为什么是固定地址

内核代码/数据跑在 higher-half,但物理内存仍保留全 RAM 恒等映射用于直接访问;用户窗口占用的物理区间必须从分配器里整段预留,避免与内核页冲突:

```
USER_BASE  = 0x08048000 (经典 Linux i386 ET_EXEC 地址)
USER_SIZE  = 8MB,末端 64KB 留给用户栈,最后一页是只读 sigreturn trampoline
USER_SIGTRAMP = 0x08847000
```

- `mm_init` 用 `mm_reserve_range(0x08000000, 0x08C00000)` 永久占用用户窗口的物理区间(包括所有可能的进程块)。
- 每个进程 `proc_snapshot_alloc` 分配连续 8MB 物理块 + 一个 trampoline 页,`mm_create_user_pd(block, tramp)` 复制内核页目录后只改 PDE 32–34:窗口内 2047 页映射到该进程块(USER|RW),最后一页映射只读 trampoline;其余 PDE 与内核共享恒等映射。
- 调度器 `mm_switch_pd(pd_phys)` 切 CR3;因此同一虚拟地址在不同进程指向不同物理块,内核仍可用恒等地址直接访问任意进程块(`p->mem`)。

代价:每个进程 8MB 物理块、最多 8 个进程/线程槽、要求 RAM > 128MB;收益:真正并发的进程地址空间,不需要搬移快照。higher-half 仍未做。

### 7.2 进入/退出 Ring3(isr_stubs.s)

- `user_enter(entry, user_stack)`:`cli` → 保存 ebp/ebx/esi/edi 与 ESP 到 `user_kernel_esp_save` → **DS/ES/FS/GS 置 0x23**(iret 不会重载它们,DPL0 选择子会立刻 #GP)→ 压 SS=0x23/ESP/EFLAGS=0x202/CS=0x1B/EIP → `iret`。
- 进入 Ring3 后,TSS.esp0(8KB 专用栈)接管所有中断/int 0x80 的 Ring0 入口;PIT 100Hz 在用户态照常运行。
- `user_return_to_kernel()`:exit/故障时**丢弃 TSS 栈**,恢复保存的 ESP 与 callee-saved 寄存器、段寄存器回到 0x10,`sti; ret` 回到 `user_run_embedded`。注意同特权级 iret 不会恢复 ESP/SS,所以不能用 iret 回内核。
- `registers_t` 布局对 Ring3 帧额外包含 useresp/ss,与 isr_stubs.s 的压栈顺序一致。

### 7.3 int 0x80 分发(syscall.c)

- `syscall_entry`:push 0/0x80 → 与 isr_common 相同的保存序列 → `syscall_handler(registers_t*)` → `regs->eax` 即返回值 → iret 回用户。
- Linux i386 ABI:eax=调用号,ebx/ecx/edx(以及 esi/edi/ebp)传参;错误用 `-errno`。
- 已实现(按用途分组):
  - 进程/stdio:`exit(1) exit_group(252) read(3) write(4) readv(145) writev(146) getpid(20) getppid(64) gettid(224) getuid/getgid/geteuid/getegid(24/47/49/50) 及其 32 位变体(199-202) getgroups(80/205)`;`fork(2)/vfork(190)/clone(120)`(支持 CLONE_VM/FILES/SIGHAND/THREAD/SETTLS/PTID/CTID)、`wait4(114)/waitid(284)`、`execve(11)`;`pipe(42)/pipe2(331)`、`poll(168)/ppoll(309)`(真阻塞)、`select(82)/_newselect(142)/pselect6(308)`;`kill(37)` 真实投递;`modify_ldt(123)` 返回 ENOSYS(由 set_thread_area 取代)
  - 信号:`signal(48)`、`sigaction(67)`、`rt_sigaction(174)`、`sigprocmask(126)`、`rt_sigprocmask(175)`、`sigsuspend(130)/rt_sigsuspend(179)`、`sigreturn(119)/rt_sigreturn(173)`、`alarm(27)`
  - 文件:`open(5) close(6) lseek(19) llseek(140) dup(41) dup2(63,含 stdio 重定向) fcntl(55/221) access(33) getcwd(183) chdir(12) unlink(10) rename(38) sendfile(187/239) chmod(15) fchmod(94) chown(16)/lchown(182)/chown32(212)/lchown32(198) fchown(95/207) fchownat(298) readlink(85) truncate(92/193) ftruncate(93/194) fsync(118) fdatasync(144) sync(36)`;`*at` 族:`openat(295) mkdirat(296) fstatat64(300) unlinkat(301) renameat(302) readlinkat(305) fchmodat(306) faccessat(307)`
  - 目录/元数据:`getdents(141) getdents64(220) stat/lstat/fstat(106-108) stat64/lstat64/fstat64(195-197) statx(383) statfs/fstatfs(99/100) statfs64/fstatfs64(268/269) utimes(271) futimesat(299) utimensat(320)`
  - 内存/其他:`brk(45) old_mmap(90) mmap2(192) munmap(91) mprotect(125) uname(122) sysinfo(116) set/get_thread_area(243/244) getrlimit(76)/ugetrlimit(191)/setrlimit(75)/prlimit64(340) nanosleep(162) times(43) getrusage(77) time(13) gettimeofday(78) clock_gettime(265/403)`
  - 未知号返回 `-ENOSYS(-38)` 并在串口打印一次编号,作为 ROADMAP 阶段 4 的待办清单。
- fd 表在 user.c 统一管理 0..10:启动时 0/1/2 是 `USER_FD_STDIN/STDOUT/STDERR`,其余条目可为 RAM FS 文件、目录、`/dev` 节点、`/proc` 快照或已关闭(FREE)。新 fd 从 3 起分配;`dup(41)` 复制任意条目,`dup2(63)` 可把文件/设备重定向到 0..2(或反向),`dd` 的 `dup2(if_fd,0)` 由此可用。
- 路径解析 `vfs_normalize`:以进程 cwd(`user_cwd_get/set`,启动时 `/`)为基准处理绝对/相对路径、`.`、`..`(越界停在 `/`),输出规范绝对路径。命名空间:`/` 是 RAM FS(支持多级目录,路径去掉前导 `/` 后存于 fs.c),另有虚拟 `/dev`、`/proc`、`/etc`、`/bin`。
- `*at` 族共用 `at_resolve_raw`:绝对路径直接透传;`AT_FDCWD(-100)` 保留相对路径交给 `vfs_normalize`;目录 fd 则把 `user_fd_dir_path(fd)` 与相对路径拼成绝对路径(非目录 fd 返回 ENOTDIR,坏 fd 返回 EBADF)。`at_path` 额外拒绝空路径(ENOENT)。`do_fstatat` 单独处理空路径 + `AT_EMPTY_PATH`(stat dirfd),`do_unlinkat` 的 `AT_REMOVEDIR` 转 rmdir;`do_readlinkat` 对已存在目标返回 EINVAL(无符号链接);`fchmodat` 对真实文件/目录返回成功(RAM FS 无 mode 位)。
- 虚拟文件:`/dev/null` 读 EOF/写丢弃,`/dev/zero` 读零/写丢弃,`/dev/console` 接键盘与 VGA+串口;`/proc/{meminfo,uptime,version,cpuinfo,mounts}`、`/proc/<pid>/{stat,status,comm,cmdline}` 与 `/etc/mtab` 在 `open` 时由内核生成快照存入 fd,`/proc/meminfo` 取 mm 页统计,`uptime` 取 PIT 100Hz;`/proc/<pid>/stat` 输出 Linux 的 52 字段(尾部保留空格),BusyBox `ps` 可解析。`/proc/stat` 输出 `cpu`/`cpu0` 十个时间字段(由 PIT tick 近似切分 user/system/idle)、`intr`、`ctxt`、`btime`、`processes`、`procs_running/blocked` 与 `softirq`,`top -b -n 1` 可显示 CPU 利用率。
- `getdents/getdents64` 按目录 fd 的绝对路径枚举 `.`、`..`、RAM FS 文件以及 `dev/proc/etc` 虚拟子目录,分别按 i386 `linux_dirent`(10 字节头)与 `linux_dirent64`(19 字节头)打包;`d_off` 写下一项序号,缓冲区不足时保留偏移。
- `stat` 族:`stat64` 写 96 字节 i386 布局,旧 `stat` 写 64 字节布局;目录 `S_IFDIR|0755`/nlink 2,文件 `S_IFREG|0644`,设备 `S_IFCHR|0666`,`/proc` 快照 `S_IFREG|0444`,`st_dev=1`;三类时间来自 RAM FS 的 atime/mtime/ctime。`statx(383)` 写 256 字节 `struct statx`(mask=STATX_BASIC_STATS),空路径且 `AT_EMPTY_PATH` 时改为 stat dirfd。
- `statfs/fstatfs(99/100)` 与 `statfs64/fstatfs64(268/269)` 按 16×64KB 容量与空闲槽位填充,`df`/`stat -f` 可用;`sysinfo(116)` 回报 uptime/内存/任务数。`/proc/mounts` 输出 `/dev/root / ext2` 等真实设备行,BusyBox `df` 会跳过 `rootfs` 伪设备。
- `write/read` 按 fd 类型分发:`STDIN`/`DEV_CONSOLE` 读先阻塞等待(见下),无其他可运行进程时退回 `keyboard_getchar`(内部 `sti; hlt; cli`);`STDOUT/STDERR/DEV_CONSOLE` 写 VGA+串口,`FILE` 走 `fs_read_at/fs_write_at`,`VFILE` 读内核快照、写返回 EACCES,`DEV_NULL/DEV_ZERO` 分别 EOF/零填充与丢弃写入,`DEV_ATA` 走 `ata_read_bytes/ata_write_bytes`(按 fd 偏移,部分扇区读改写)。所有用户缓冲区先用 `user_ptr_ok` 校验。
- `writev/readv` 按 iovec 顺序复用 `do_write/do_read`,部分成功时返回已处理字节数。musl 的 stdio 缓冲正是用 `writev` 刷出——这是 BusyBox `ls` 能打印的关键。
- `sendfile64(239)` 从 in_fd(可选用户 offset 指针)读入内核缓冲并写 out_fd,更新偏移;BusyBox 的 fd 拷贝优先用它,失败再退回 read/write。
- `poll/select` 就绪规则:`fd_read_ready` 中文件/VFILE/目录/`/dev/null|zero|hda` 恒真,pipe 读端看 `pipe_fill>0` 或无写端(EOF),stdin/console 看 `keyboard_haschar()`;`fd_write_ready` 中文件/stdio/console/null/zero/hda 恒真,pipe 写端看缓冲区未满。`poll(168)` 逐项填 `revents`(请求的 `POLLIN/POLLOUT`、pipe 无写端附加 `POLLHUP`、坏 fd 给 `POLLNVAL`)。
- **阻塞 poll**:用户进程且当前有可运行目标时,`do_poll` 调 `proc_block_current_ext(PROC_WAIT_POLL, fds, nfds, deadline)` 后立即退回汇编 iret 到下一个进程;唤醒方 `proc_wake_pollers` 用 `user_fd_state_events(p->fd_state, fd, events)` 在**保存的 fd 表**上重算,把 `revents` 通过 `p->mem` 写进阻塞进程的用户数组,置 `p->eax=ready` 后标记 READY;PIT 到期 `proc_poll_expire` 置 0。没有其他可运行进程(或 pid1 shell)时退回旧的 `sti; hlt` 有界循环。
- **阻塞 read**:pipe 读端为空且有写端、stdin 无字符时同样阻塞(`PROC_WAIT_READ`,记录 fd/缓冲/长度);`pipe_write/pipe_writer_add/读端消费` 与键盘 IRQ 调 `proc_wake_readers`,后者直接 `pipe_read` 到内核临时缓冲并 `memcpy` 进 `p->mem`,stdin 则取一个键盘字符,置 `eax`。信号会把这些等待以 `-EINTR` 唤醒。
- `select/_newselect` 按 32 位 fd_set 字就地置位/清零;`pselect6` 立即扫描 + 有界 PIT 等待,`ppoll` 复用阻塞 poll;`pselect6/ppoll` 忽略 sigmask;nfds 上限 1024。
- `mmap` 是沙箱内模拟:匿名取 bump 区,文件映射把 RAM FS/`/proc` 快照按页拷入并零填充,`mmap2` 的 ebp 页号乘 4096,`MAP_FIXED` 校验页对齐与顶部 64KB 栈保留区;映射不写回,`munmap/mprotect` 为空操作。`truncate/ftruncate` 调用 `fs_truncate`(增长零填充、缩小只改 size),64 位变体在 hi 非零时返回 EFBIG;`fsync/fdatasync/sync` 因 RAM FS 已在内核内存中直接成功。
- `nanosleep` 把秒/纳秒换算成 100Hz PIT tick,用 `sti; hlt; cli` 等待(不参与轮转,但中断仍可唤醒;换算全程 32 位避免 `__udivdi3`)。`times(43)` 返回启动以来 tick,`getrusage(77)` 校验 who 后清空 72 字节结构,`chown` 族复用 `do_chmod`/`do_fchmod` 只校验目标存在与类型,`readlink(85)` 对非符号链接返回 EINVAL。
- `uname` 按 Linux i386 的 6×65 字节 utsname 填充 MuOS/muos/1.2.0/i386;`unlink/rename` 真正修改 RAM FS;`utimes/futimesat/utimensat` 会写入 atime/mtime(支持 `UTIME_NOW/OMIT`),`fs_set_times` 同时刷新 ctime。
- 时钟:CMOS RTC 读出完整年月日(处理 BCD/12h/世纪寄存器),`rtc_datetime_to_epoch` 用 civil-days 算法换算 UTC 秒;`time(13)`、`gettimeofday(78)`、`clock_gettime(265/403)` 返回真实 epoch,monotonic 时钟用 PIT ticks,亚秒精度 10ms。

### 7.4 ELF32 加载器(elf.c)

- 接受 `ET_EXEC` 与 `ET_DYN` + `EM_386`;校验 class/data/version、e_phentsize/phnum 边界(p_offset+p_filesz 不越界)、每个 `PT_LOAD` 完全落在 `[USER_BASE, USER_END-64KB)`。
- `ET_EXEC` 按 link-time 地址装入;`ET_DYN` 先求 `min(p_vaddr)`,取 `bias = load_base - min_va`,所有段与入口加 bias —— 这就是内核内建的最小 PIE 装载。
- 逐段 `memcpy` + bss `memset`,记录最高装入地址作为 brk 起点;入口必须在沙箱内。
- `PT_DYNAMIC`:遍历 `DT_REL/DT_RELSZ` 与 `DT_JMPREL/DT_PLTRELSZ`(i386 `Elf32_Rel`),支持 `R_386_RELATIVE`、`R_386_GLOB_DAT`、`R_386_JMP_SLOT`、`R_386_32`;符号未定义(需要共享库)或 `COPY/IRELATIVE/TLS` 时返回 `ELF_ERR_RELOC`。这是 `run pie` 能工作的关键。
- `PT_INTERP`:读取解释器路径并回填 `elf32_load_info_t.interp`。`elf32_load_program` 在动态镜像只映射段、**不**应用重定位(交给用户态 ld.so);`user_load_for_exec` 随后按 `/lib/ld-mu.so`(MuFS 优先、内嵌 blob 回退)把解释器映射到 `USER_INTERP_BASE=0x08200000`,入口改为解释器 entry,`AT_BASE` 为其 load bias,`AT_ENTRY`/`AT_PHDR` 仍是主程序的值。
- 初始栈的 auxv 由 `build_user_stack(argc,argv,li,interp_base)` 按 `AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_BASE/AT_ENTRY/AT_NULL` 写入。
- 镜像由 build.ps1 用同一交叉编译器链接到 0x08048000,再用 `objcopy -I binary -O elf32-i386` 变成 `hello_elf.o` 链进内核(`_binary_hello_elf_start/end`);运行前整段 memset 清零,重复 `run` 安全。
- 三个入口共用 `user_run_image_argv(image,size,argc,argv)`:`run` 用内嵌 blob,`exec <file>` 用 `fs_open` + `fs_fd_data/size` 从 RAM FS 读取,`execmod <n> [args...]` 用 Multiboot module。ELF 加载器只读源镜像,不依赖它在沙箱内。
- `execmod` 先把模块镜像复制到 `mm_alloc_pages` 的连续内核页再清零沙箱;模块页在 mm_init 已预留,不会被堆/页表覆盖。

### 7.5 用户程序(user/)

- `start.s`:`_start` 从内核构造的 System V 栈布局取 argc/argv,调用 `main`,返回值交给 `muos_exit`。
- 初始栈完整布局:`argc, argv[], NULL, envp NULL, auxv(AT_PHDR..AT_NULL), strings...`。真实 musl/glibc 会遍历 envp/auxv,缺终止符会把字符串字节当指针(busybox 曾因此在 `strcmp` 里 Page Fault)。
- `runtime.c`:内联 `int $0x80` 的 muos_* 封装、字符串/数字输出、memset/memcpy(用户镜像 -nostdlib 需要)。不是 libc。新增 `muos_kill/signal/sigprocmask/alarm/gettid`。
- `syscall6.s`:`muos_pselect6`(6 参必须走 ebp)与 `muos_thread_start`(clone 线程的汇编脚手架:在子栈放 arg,子线程 `call fn(arg)` 后 exit)。
- `hello.c`:打印 CS(验证 0x1B)、argc/argv、C 计算、brk、`uname`、RAM FS、匿名 `mmap`、键盘 read;子命令覆盖 `at/mmapfile/poll/extra/p1/fork/pipefork/execbin/sched`(P1)、`sig/thread/blkpoll`(P2)、`pie`(P3,PIE 指针经 `R_386_RELATIVE` 修正)、`ata/ataread`(P4)、`net`(P4,socket/ICMP/UDP);`run fault` 故意写 0xC0000000。
- `dyn.c` + `libmu.c`:`run dyn` 运行 `dyn.elf`,它只声明未定义的 `mu_hello()`;共享库里的实现通过 `DT_NEEDED` 由 `ld-mu.so` 在运行时装载并解析,输出的 `DYN-LIB-OK`/`DYN-OK` 证明跨对象符号绑定与 RELATIVE 重定位都生效。
- `linker.ld`(ET_EXEC,0x08048000)与 `linker_pie.ld`(ET_DYN,从 `SIZEOF_HEADERS` 开始、`load PT_LOAD FILEHDR PHDRS` + `dynamic PT_DYNAMIC`);build.ps1 用 `-fPIE -pie` 产出 `hello-pie.elf` 并作为第二个 blob 嵌入。
- **动态链接集**:`linker_so.ld`(共享对象,`FILEHDR PHDRS`,从 `SIZEOF_HEADERS` 起)产出 `libmu.so`(导出 `mu_hello`,数据指针经 `R_386_RELATIVE` 修正)与 `ld-mu.so`(`ld_start.s` 取原始 esp 调 `ld_main`);`linker_dyn.ld` 增加 `interp PT_INTERP` 与独立的 `.rel.dyn/.rel.plt`,配合 `-Wl,--dynamic-linker=/lib/ld-mu.so -L build -lmu` 产出 `dyn.elf`(DT_NEEDED libmu.so)。三者都以 `objcopy -I binary` 嵌入内核。
- `ldmu.c`(ld-mu.so,无依赖):从栈解析 auxv → 由 `AT_PHDR` + `PROG_BASE` 反推主程序 bias → 解析 `PT_DYNAMIC` → 按 `DT_NEEDED` 从 `/lib/<name>` 用 `open/read` 读入 `fbuf` 并映射到 `0x08400000` 起的空位 → SysV hash(`DT_HASH`)解析符号 → 对依赖库和主程序依次应用 `RELATIVE/32/PC32/GLOB_DAT/JMP_SLOT` → `jmp AT_ENTRY`(不改动栈)。`.gnu.hash`/TLS/`R_386_COPY` 未实现。
- 用户态异常在 `isr_handler` 开头分流:`(regs->cs & 3) == 3 && user_is_active()` → `user_handle_fault` 打印 EIP/CS/CR2 后回 Shell,内核 panic 路径不受影响。

### 7.6 i386 TLS 与真实 libc 启动

- GDT 槽 6 是 DPL3 的 TLS 数据段(`gdt_set_user_tls`);`set_thread_area(243)` 读用户 `user_desc`(16 字节),接受 entry -1/6,写入 base/limit/flags,回填 `entry_number=6`。用户代码随后 `mov %gs,0x33`。
- 描述符:access 0xF2(可写数据)/0xF0(只读),gran 0x40(字节)或 0xC0(4KB);limit 取 20 位。
- `get_thread_area(244)` 回读;`exit_group(252)` 走 exit;`set_tid_address(258)` 返回 1;`getuid32/...` 返回 0;`ioctl` 返回 `-ENOTTY`;`dup/dup2` 复制 fd 表条目并可重定向 stdio。
- **验收**:Alpine static i386 BusyBox 1.36.1(musl)作为 `-initrd` 模块进入 Ring3;`echo`、`ls /`、`cat /notes.txt`、`pwd`、`id`、`uname -a` 及 20+ applet(含 `wc/grep/sed/od/stat/cp/rm/mv/touch`)实测通过,不存在的路径返回 ENOENT。

### 7.7 进程、线程与信号(proc.c)

- `proc_t` 固定 8 槽:pid/ppid/tgid、state、exit_code/faulted、`proc_space_t*`(mem/pd/tramp/refs,`CLONE_VM` 线程共享)、8KB 独立内核栈(每线程一个)、fd 表(`proc_fdstate_t` 引用计数,`CLONE_FILES` 可共享)、cwd/brk/TLS、完整寄存器组、阻塞等待字段(poll fds/deadline/read fd)、信号字段(pending/blocked/handler/flags/restorer/sigframe 栈)。
- `fork/clone`:分配槽位 + 内核栈;非 `CLONE_VM` 复制 8MB 块与 fd 快照;`CLONE_VM` 只加地址空间引用并共享 `p->mem`;`CLONE_FILES` 共享 fd 表;`CLONE_SETTLS` 写入子线程 TLS;子线程 `eax=0`、`esp=stack`(由 `muos_thread_start` 预置),父进程返回 tid。`SIGCHLD` 语义由 wait4 处理。
- `wait4/waitid`:先找 ZOMBIE 直接收尸;否则 `proc_block_current` 保存现场并切到下一个 READY;子进程 `proc_exit_current` 经 `proc_wake_parent` 把 status/siginfo 写进父进程内存并置 `parent->eax=pid`,父进程恢复后如同 syscall 正常返回。
- `exit`:标记 ZOMBIE、释放 fd 快照引用、唤醒父进程;若无可运行进程则切回内核页目录并返回 shell。僵尸的地址空间在 `proc_reap`(父进程 wait 或 `proc_cleanup_children`)时按引用计数释放。
- `execve(11)`:先把 path/argv 拷到内核,`vfs_lookup` 找到 RAM FS 文件,`elf32_validate` 预校验;`user_exec_image` 清空当前进程块、重载段+栈(保留 fd/cwd),再把 syscall 的 iret 帧改成新 entry/esp——同一个 pid 原地替换。
- 信号:`proc_signal_send` 置 pending 位;若目标 BLOCKED,把 `eax=-EINTR`、清 wait_kind 并置 READY。`proc_signal_deliver` 在 syscall 返回前与 tick 中运行:handler 为空则执行默认动作(SIGKILL/SIGTERM 等终止,状态 128+sig;SIGCHLD/SIGCONT 忽略);有 handler 则把 12 个寄存器 + eip 存到用户栈(handler 栈之上),入口设为 handler、返回地址为 `USER_SIGTRAMP`(只读 `mov eax,173; int 0x80`)或 libc restorer,并记录 frame 供 `proc_sigreturn` 恢复。屏蔽位只推迟投递,不会丢弃 pending。
- `sched_load` 恢复 CR3/TSS.esp0/fd 表/cwd/brk/TLS,并把 `registers_t` 全部改写后置 `frame_switched`;syscall/IRQ epilogue 的 `iret` 因此进入下一个进程。
- `/proc` 目录由 proc 表动态生成 PID 子目录;`/proc/self` 解析为当前进程。`ps` 读取 `/proc/<pid>/stat` 与 `cmdline`;另有 `/proc/loadavg`、`/proc/stat`、`/proc/self/maps`。
- 虚拟 `/bin`、`/usr/bin`、`/sbin` 下的任意名字(以及 `/bin/busybox`)映射到 Multiboot module 0;`execve` 支持 `VFS_T_MODULE`,BusyBox shell 因此能按 PATH 重新执行自身 applet。
- `pipe(42)/pipe2(331)`:8 个 4KB 环形缓冲 pipe.c;fd 表新增 `PIPE_READ/PIPE_WRITE` 类型,`read/write/dup/dup2/close/fstat` 均支持;shell 的 `echo hi | cat`、`ls | wc -l` 已跑通。
- `clone(120)` 支持 `CLONE_VM` 线程与自定义栈,非 CLONE_VM 时等价 fork;`vfork(190)` 等同 fork;`kill(37)` 真实投递信号,SIGTERM/SIGKILL 默认终止进程;进程组调用暂时返回自身 pid(无 pgid 表)。

### 7.8 网络与 socket ABI(pci.c / net.c)

- `pci.c`:机制 #1 配置空间读写(0xCF8 地址、0xCFC 数据),`pci_find(0x10EC,0x8139)` 扫 bus 0–7/slot 0–31,读 BAR0 得到 I/O 基址并置命令寄存器 bit0(I/O)+bit2(bus master)。
- `net.c` 驱动:RTL8139 软复位(0x37 bit4)后读 MAC(0x00–0x05),设置 RBSTART(0x30,物理地址由 `mm_virt_to_phys` 给出)、RCR=0x100F(全接收 + 32K 环)、TCR、CAPR、CMD=0x0C(RE|TE)。**CAPR 在 RTL8139/QEMU 中比内部读指针超前 16 字节**,因此写入 `rx_capr - 0x10`(初始 0xFFF0),否则 QEMU 会判定 16 字节可用并触发 RxOverflow。接收走轮询:`ISR(0x3E) & ROK`,先写 0xFFFF 应答,再按 4 字节头部(状态/长度)读环,长度含 4 字节 CRC;发送用 4 个描述符轮转(TSAD/TSD,轮询 bit15 完成),发送缓冲与接收环都是内核静态 BSS 的物理地址。
- 协议:ARP 缓存 4 项(请求/应答,收到请求且目标是自己时回 MAC);IPv4 20 字节头 + 校验和,按掩码判断是否经网关;ICMP echo 请求自动回 reply;UDP 无校验和。收到的报文按 socket 分发:RAW 存 IP 载荷(ICMP 消息),DGRAM 按本地端口存 UDP 载荷。
- socket 表 8 槽:`net_socket(type,proto)` 支持 DGRAM/RAW/STREAM(其他类型 ENOSYS),`bind/connect/listen/accept/sendto/recvfrom`;`recvfrom/accept` 在 syscall 内以 `sti; hlt; cli` 轮询约 3 秒,超时返回 `NET_EAGAIN`(-11),由 syscall 层转成 `-EAGAIN`。对端地址结构按 i386 `sockaddr_in`(family/网络序 port/4 字节 IP)解析与回填。
- syscall:直接编号 `socket(359)/bind(361)/connect(362)/shutdown(373)/sendto(369)/recvfrom(371)`,以及旧式 `socketcall(102)`(1/2/3/4/5/9/10/11/12 等);socket fd 固定为 16–23,`close(6)` 会先识别该区间再释放 socket 槽。`listen(363)/accept4(364)` 也已接入;`send/recv` 复用 `sendto/recvfrom`(无地址参数)。固定兜底配置 10.0.2.15/24、网关 10.0.2.2、DNS 10.0.2.3(QEMU user-net 默认值)。
- DHCP:开机 `net_init` 末尾调用 `dhcp_run()`:UDP 68→67 广播(源 0.0.0.0、以太网广播、不做 ARP)`DISCOVER`,等 OFFER(解析 yiaddr/option 53/54),再发 `REQUEST`,等 ACK 后写回 `my_ip/my_mask/gateway`;任何一步失败都保留静态地址。`ip_handle` 对 UDP dport 68 调用 `dhcp_input` 而不是 socket 分发。
- TCP(最小实现):`net_sock_t` 增加 `tcp_state/snd_nxt/rcv_nxt/echo_mode`;`net_connect` 对 STREAM 发 SYN 并等待 SYN-ACK(同步回环时状态在 send 返回前就已建立);`tcp_input` 处理 SYN→新建子连接(SYN-ACK)、ACK→ESTAB、数据段(校验 seq、按窗口存一段、回 ACK)、FIN→CLOSE_WAIT;`tcp_send_seg` 构造 20 字节头 + 伪首部校验和。SYN 到端口 7 且无监听者时创建 `echo_mode` 内核服务,收到数据立即回显,用于 `run tcp` 自测。`127.0.0.1`/本机地址在 `net_send_ip_raw` 里回环到 `ip_handle`(拷贝到独立的 `loop_frame`,避免与 `out_frame` 别名)。无重传/超时/拥塞控制。
- 用户侧 `hello.c net` / `tcp`:net 构造 ICMP echo(校验和按大端写入)发往 10.0.2.2,`recvfrom` 校验 type=0/id/seq;再用 UDP 向 10.0.2.3:53 发 DNS A 查询,校验事务号与 QR 位。tcp 用 SOCK_STREAM 连接 127.0.0.1:7,完成后打印 `TCP-OK`。

### 7.9 VBE 线性帧缓冲(fb.c / boot_cd.s)

- 引导:`boot_cd.s` 在实模式用 `INT 10h` 先 `4F00`(CX:DX='VBE2')取 VBE 信息(校验 `VESA` 魔数),再按 `0x114→0x117→0x113→0x116→0x112` 顺序 `4F01` 取模式信息,要求 attributes 的 bit7(LFB)与 bit0(supported)、bpp≥16,最后 `4F02` 带 `0x4000`(LFB)设模式。成功时把 PhysBasePtr/XResolution/YResolution/bpp/BytesPerScanLine/mode 写到 `0x6F00`,魔数 `'VBE1'` **最后写**,失败则不写(内核回退文本模式)。
- 内核:`fb_pending()` 只检查魔数与合理性;`fb_enable()` 在 `mm_enable_paging()` 之后把 `pitch*height` 按页 `mm_map_page(phys, phys, PAGE_KERNEL|PCD)` 恒等映射(uncached),设置原点把 640×400 文本区居中;按 bpp 写像素(32/24/16 bpp,16bpp 为 5-6-5)。字模来自 `include/font8x8.h`(公有领域 8×8),每个 8×8 字形纵向复制 2 次得到 8×16 单元。
- `vga.c` 现在维护 80×25 的 `cells[]` 影子网格:文本模式下同步写 `0xB8000`;有 VBE 但尚未映射时只写影子;`fb_active()` 后每个单元直接 `fb_put_cell`。滚动在影子上 memmove 后整屏重绘;光标在帧缓冲下画白色下划线并先重绘旧单元。`vga_repaint()` 用于 `fb_enable()` 之后把启动早期已写入影子的横幅补画到屏幕。
- 桌面/编辑器:所有 `0xB8000` 直写改为 `vga_put_cell/vga_get_cell`(`win7.c` 的 put_cell/fd/ft/ftn、鼠标反色保存恢复,`editor.c` 的行/状态/帮助/光标),因此两种显示后端行为一致。

## 八、Shell / FS / 桌面

- **shell.c**:128 字节行编辑(方向键/历史/Tab 补全),`tokenize` 支持单双引号并保留引号内空格,MAX_ARGS=16 硬边界;29 条命令 if/else 分发(与补全表 cmds[] 双份维护,改动时两处同步);`run [fault|pie|dyn|net]` 运行内嵌 ELF(其中 `dyn` 先写 `/lib` 再跑动态程序;`net` 走 socket 自测),`exec <file>` 运行 RAM FS 中的 ELF,`mods`/`execmod <n> [args...]` 运行 Multiboot module。启动时把内嵌 ELF 复制为 `hello.elf`;`cat` 按 fd 流式输出完整 64KB 文件。
- **fs.c**:16 文件槽 × 64KB + 16 目录槽;路径是相对 root 的多级字符串(如 `tmp/x`),`fs_mkdir/fs_rmdir/fs_dir_get/fs_parent_is_dir` 支持子目录、非空目录拒绝与直接子项枚举。文件槽位带 `refs` 与 `deleted` 标志:unlink 时若仍有 fd 打开则清空名字但保留数据,最后一个 `fs_unref` 才释放槽位;`fs_rename_dir` 整棵子树改名(先校验全部新名字长度、拒绝目标已存在/移入自身)。`fs_read_at/fs_write_at/fs_fd_truncate` 供系统调用层按偏移读写,`fs_rename` 可跨目录改名/覆盖(重名校验、父目录校验、内容保留)。
- **MuFS 持久化(同一文件底部)**:磁盘布局 = 扇区 0 超级块(`MUFS` 魔数/版本/generation)、扇区 2–5 文件元数据表(16×88B)、扇区 6–8 目录元数据表(16×68B)、扇区 16 起 16 个 64KB 数据槽。`fs_persist_mount()` 在 `fs_init` 后调用:魔数/版本/槽参数匹配则读入命名空间与文件体,否则格式化为空;之后每个变更点(`create/write(_at)/truncate/delete/rename*/mkdir/rmdir/set_times/unref 释放`)调用 `persist_meta()` 并只写涉及的数据扇区(`persist_slot/persist_slot_range`)。`fs_sync()` 递增 generation、重写超级块 + 全部元数据 + 全部已用槽;`fsync(118)/sync(36)` 调用它。无盘时 `persist_active=false`,全部是内存操作。
- **editor.c**:128 行 × 79 列全屏编辑器;静态 10KB IO 缓冲(修复旧版 1KB 存盘截断),^S 保存 ^R 重载,PgUp/PgDn 翻页;运行时隐藏硬件光标、用反色单元作光标。
- **win7.c**:文本模式 Win7 风格桌面——行 0 标题、1..23 桌面、24 任务栏;窗口按 z-order 绘制(活动窗口最后),鼠标点击/标题栏拖拽/最小化/最大化/关闭,开始菜单鼠标/键盘双操作,桌面图标可见;任务栏显示实时内存与 CMOS RTC `HH:MM:SS`。`gui`/`win7`/`desktop` 命令共用此实现;`run` 之外的 Text Editor/Calculator 可从菜单或图标启动(Calculator 聚焦时直接吃数字/运算符)。
- **mouse.c + rtc.c**:PS/2 鼠标初始化(0xA8/命令字节/0xF6/0xF4)与 3 字节包解析(9 位 dx/dy、溢出位、按键),映射为 80×25 文本单元;`rtc_read_time` 处理 UIP、BCD/12 小时制,失败时任务栏回退 `UP:MM`。
- **vga.c**:维护 80×25 影子 `cells[]`;文本模式同步写 `0xB8000`,VBE 模式下改由 `fb.c` 逐单元渲染;`vga_hide_cursor/show_cursor` 用 CRTC start/end 保存/恢复 BIOS 光标形状(仅文本模式),桌面与编辑器绘制自己的光标。

## 九、构建系统

- **build.ps1**:`-Clean/-Run/-Iso` 组合;工具探测顺序 PATH → toolchain\ → tools\ → 常见安装位置;编内核 `-m32 -ffreestanding -O2 -g -Wall -Wextra -fno-tree-loop-distribute-patterns -nostdinc`;链接经 `i686-elf-gcc -nostdlib -T linker.ld`(GCC 驱动而非裸 ld)。
- 用户程序:`user/*.c` 用同款交叉编译器、`-fno-pic -fno-pie -nostdlib` 编译,`user/linker.ld` 链接到 0x08048000,`objcopy -I binary -O elf32-i386 -B i386` 生成 `hello_elf.o` 参与内核链接。
- **make_iso.py**:PROJECT 取脚本所在目录;objcopy 出 flat kernel.bin,bootloader 在偏移 4092 补丁内核大小;pycdlib 打 El Torito ISO。
- ISO 路径实测:`-cdrom` 启动 → 自检全 PASS(修复前该路径直接把魔数当指令执行)。

## 十、关键全局量

| 符号 | 位置 | 说明 |
|---|---|---|
| `_kernel_end` | linker.ld | 内核镜像结束,位图起点 |
| `bitmap / total_pages / free_pages` | mm.c | 物理分配器状态 |
| `page_directory / page_dir_phys` | mm.c | 页目录(恒等映射全部 RAM) |
| `task_list_head / current_task / task_count` | task.c | 任务 introspection |
| `kbd_buffer/head/tail` | keyboard.c | 256 字节键盘环形缓冲 |
| `pit_ticks` | pit.c | 100Hz tick(uptime = ticks/100) |
| `tss / kernel_stack_top` | tss.c | Ring3→Ring0 的 TSS 与 8KB 入口栈 |
| `user_kernel_esp_save` | user.c | user_enter 保存的内核上下文 |
| `brk_base / brk_current` | user.c | 用户堆 bump 指针 |
| `_binary_hello_elf_start/end` | 链接器 | 内嵌用户 ELF 的起止地址 |
| `px / py / btn_*` | mouse.c | PS/2 鼠标虚拟像素坐标(640×400)与按键 |
| `mouse_cx/cy` | win7.c | 软件光标所在文本单元 |
| `cursor_scan_start/end` | vga.c | BIOS 光标形状(隐藏/恢复) |
| `modules[] / module_count` | module.c | Multiboot -initrd 模块范围与名称 |
| `bitmap_storage` | mm.c | 静态物理页位图(避免覆盖 initrd) |
| `tls_base/limit/flags` | gdt.c | GDT slot 6 用户 TLS 描述符 |
| `procs[] / current_proc / next_pid` | proc.c | 8 个进程/线程槽与调度器状态 |
| `frame_switched` | proc.c | 调度器是否已改写中断帧 |
| `present / sectors / model` | ata.c | ATA 主盘探测结果 |
| `_binary_hello_pie_elf_start/end` | 链接器 | 内嵌 ET_DYN(PIE)镜像起止 |
| `_binary_ld_mu_so_start/end` | 链接器 | 内嵌动态链接器 ld-mu.so 起止 |
| `_binary_libmu_so_start/end` | 链接器 | 内嵌共享库 libmu.so 起止 |
| `_binary_dyn_elf_start/end` | 链接器 | 内嵌动态链接测试程序 dyn.elf 起止 |
| `KERNEL_VBASE / KERNEL_BASE / KERNEL_PHYS_BASE` | mm.h | higher-half 虚拟基址、链接地址、物理装载地址 |
| `entry_phys / _kernel_end` | linker.ld | ELF 入口物理地址、内核虚拟结束地址 |
| `persist_active / persist_gen` | fs.c | MuFS 挂载状态与 generation |
| `meta_buf / dmeta_buf` | fs.c | 写穿的元数据暂存表 |
| `io_base / present / mac_addr` | net.c | RTL8139 I/O 基址、探测结果与 MAC |
| `rx_buffer / tx_buffer` | net.c | 32K 接收环与发送 DMA 缓冲(物理地址交给网卡) |
| `socks[] / arp_cache[]` | net.c | 8 个 socket 槽与 4 项 ARP 缓存 |
| `info / fb / active` | fb.c | VBE 模式信息、恒等映射后的显存指针与启用状态 |
| `cells[] / pending` | vga.c | 80×25 影子文本网格与"等待帧缓冲"标志 |

## 十一、已知局限(真实清单)

1. 用户窗口仍是**固定虚拟地址** `0x08048000`(每进程独立物理块/页目录,内核已 higher-half,但用户态无地址随机化);每进程 8MB 物理块、最多 8 个进程/线程槽
2. 调度器是单核 100Hz 轮转,无优先级/时间片配额;`nanosleep` 仍独占 CPU(不参与轮转);`kill` 的进程组语义只支持 pid/0/-1
3. 动态链接由自研 `ld-mu.so` 完成,只支持 SysV hash(`DT_HASH`)与自有 `.so`;`.gnu.hash`、TLS 重定位、`R_386_COPY`、符号版本化未实现,因此还不能加载真实 libc 的共享库;`MAP_SHARED` 不写回
4. MuFS 是整槽布局(16×64KB,无块分配位图/碎片整理/日志),只支持 16 文件 + 16 目录,元数据表每次全量重写(≈3 扇区);网络仅 RTL8139 + ARP/ICMP/UDP/DHCP + 最小 TCP(无重传/超时/拥塞控制,用户态 listen/accept 未做并发验证);VBE 帧缓冲只在自研 ISO 引导路径可用,`-kernel` 仍是 VGA 文本模式;显存按 uncached 恒等映射,桌面依旧是 80×25 字符网格(8×8 字模),并非真正的图形 UI
5. 内核堆不可增长、不可重入(IRQ 上下文需手工加锁)
6. 有 16 目录槽但路径/名称上限 64 字符;无盘时文件系统重启即失(有盘时 MuFS 持久化);write 截断不告警;`/dev`、`/proc`、`/etc`、`/bin` 为只读虚拟层
7. 键盘仅 US QWERTY;PS/2 鼠标是文本模式粗粒度指针(按 8×16 单元),无滚轮/右键菜单/双击标题栏最大化
8. 桌面是 80×25 字符网格(ISO 路径经 VBE 帧缓冲渲染,`-kernel` 为 VGA 文本),无图形控件/字体渲染/动画;窗口内容除 Calculator 外主要是状态展示
9. 单处理器(PIC,非 APIC);用户进程由 IRQ0 抢占,内核 shell 本身仍在启动栈上协作运行
10. 无内核栈溢出保护(16KB 栈,GUI 深调用链靠它撑着;用户态入口另有 8KB TSS 栈)
11. 无 guard page / 无 KASLR / panic 信息只有寄存器 dump(有 -g,可 addr2line)
12. NT/PE 兼容尚未开始:执行前端目前只有 ELF32(ROADMAP 阶段 5)
