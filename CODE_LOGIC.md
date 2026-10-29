# MuOS 代码逻辑(v1.2)

本文对应重构后的代码现状:multiboot mmap 解析、全 RAM 恒等映射、内核堆、单任务模型、Ring3 用户态(int 0x80 + ELF32)、PS/2 鼠标 + RTC 桌面、72 项自检。演进计划见 ROADMAP.md。

## 一、总体结构

```
启动(汇编)          内核入口              硬件抽象                系统服务              用户可见
boot.s          →   kernel.c         →   gdt/idt/isr/pic/irq  →  mm.c  kheap.c  →  shell.c(29 命令)
boot_cd.s           初始化序列            pit keyboard mouse         string.c fs.c      editor.c
isr_stubs.s                              vga serial rtc             task.c test.c      win7.c(桌面)
                                                                      tss/elf/syscall/user → user/hello.c
```

- `src/` 平铺 25 个 `.c` + 3 个 `.s`;`include/` 配对头文件,guard 统一 `MUOS_*` 前缀。
- `linker.ld`:内核加载到 `0x100000`,`_kernel_end` 供内存管理使用。
- 两条启动路径最终都把控制权交给 `kernel_main(magic, mb_info)`。

## 二、启动流程

### 2.1 boot.s(QEMU -kernel 路径)

```asm
start:          jmp real_start      ; 入口 = 镜像第一个字节
align 4                             ; Multiboot1 头:magic/flags/checksum
                dd 0x1BADB002 ...   ; (在前 8KB 内,满足规范)
real_start:     cli
                mov esp, stack_top  ; 16KB 内核栈
                push ebx            ; multiboot_info*(QEMU/GRUB 填好)
                push eax            ; 0x2BADB002 魔数
                call kernel_main
```

**为什么入口必须在第一个字节**:ISO 路径把 flat binary 拷到 0x100000 后直接 `jmp 0x100000`,若 multiboot 头排在最前,魔数 `02 B0 AD 1B` 会被当指令执行(旧版的真实故障)。`jmp` 跨头让两条路径共用同一镜像布局。

### 2.2 boot_cd.s(El Torito 光盘路径)

1. BIOS 加载引导扇区(boot.img 前半,由 make_iso.py 在偏移 4092 处补丁内核大小)到 0x7C00
2. 实模式:读光盘其余部分到 0x8C00 → 开 A20 → 建平坦 GDT → 置 PE 进入保护模式
3. 把内核 flat binary 拷到 0x100000,远跳 `(0x08, 0x100000)`
4. **EBX = 0**(无 multiboot 信息):mm.c 检测后回退 mem_upper 默认值

### 2.3 kernel_main 初始化序列

```
serial → vga → GDT → IDT(256 项)→ PIC(重映射+全屏蔽)
→ modules_init(mb)                [Multiboot -initrd 模块表提前解析]
→ mm_init(mb, kernel_end)         [mmap 解析 + 位图 + 全 RAM 恒等映射 + 模块页预留]
→ mm_enable_paging()              [CR0.PG|WP]
→ kheap_init()                    [1MB 内核堆]
→ tss_init()                      [TSS + 8KB Ring0 入口栈 + ltr]
→ PIT 100Hz + IRQ0 → keyboard → mouse(8042 aux / IRQ12)
→ task_init + task_register("kernel")
→ tests_run()                     [72 项自检,失败打印 SELF-TEST FAILED]
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

- **自举为什么可行**:mm_init 执行时 CR0.PG 尚未开启,直接写物理地址无需映射;first-fit 按地址升序发帧,每张新页表都落在已建好映射的低 4MB 内,无鸡生蛋问题。
- 这消除了旧版"分配器发放 4MB 以上帧但分页只映射前 4MB"的失配——任何越线访问立刻 Page Fault(当年任务栈交接崩溃的真实根因,详见 §六)。
- `mm_map_page` 返回 `bool_t`(拒绝重复映射/分配失败),`mm_virt_to_phys`/`mm_unmap_page` 是未来用户态的基础原语。
- higher-half(3GB+)映射留到 Ring3 里程碑。

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

- **现状:单任务**。shell 直接跑在内核栈上,PIT 中断只 tick,无上下文切换。
- 历史:旧版 task_create 手工伪造中断帧 + irq_common 切 ESP。崩溃的真实根因是 §4.3 的分页失配(任务栈页落在 4MB 以上未映射区)。如今分页已修好,但多任务暂未恢复——直接做正确的 Ring3 方案(TSS + 用户栈 + 每进程页目录),不再回头修 Ring0 轮转。
- 保留物:`task_t`(pid/name/state/next)、`task_register/get_current/get_count/list` 供 `tasks`/`ps`/自检;GDT 槽 3/4 是 Ring3 代码/数据段(0x1B/0x23),槽 5 是 TSS(0x28)。
- task_t 由 kmalloc 分配。

## 七、Ring3 用户态与系统调用

### 7.1 沙箱为什么是固定地址

内核仍是全 RAM 恒等映射(VA=PA),因此用户页不能"随便映射到别处":如果把 0x08048000 重映射到另一个物理帧,内核原本通过 0x08048000 访问的物理页就消失。当前方案是**整段预留**:

```
USER_BASE = 0x08048000 (经典 Linux i386 ET_EXEC 地址)
USER_SIZE = 8MB,sandbox 末端的 64KB 留给用户栈
```

`mm_reserve_range()` 在首次 `run` 时把这段物理页从位图分配器整段标记占用(全有或全无);`mm_user_map_range()` 再把覆盖它的 PDE/PTE 置 U/S 并重载 CR3。其余 RAM 仍是 supervisor 页,用户读写内核会 #PF。

代价:单地址空间、一次只能跑一个用户进程、要求 RAM > 128MB;收益:在没有 higher-half / 每进程页目录之前,先把 Ring3 + Linux ABI 跑通。

### 7.2 进入/退出 Ring3(isr_stubs.s)

- `user_enter(entry, user_stack)`:`cli` → 保存 ebp/ebx/esi/edi 与 ESP 到 `user_kernel_esp_save` → **DS/ES/FS/GS 置 0x23**(iret 不会重载它们,DPL0 选择子会立刻 #GP)→ 压 SS=0x23/ESP/EFLAGS=0x202/CS=0x1B/EIP → `iret`。
- 进入 Ring3 后,TSS.esp0(8KB 专用栈)接管所有中断/int 0x80 的 Ring0 入口;PIT 100Hz 在用户态照常运行。
- `user_return_to_kernel()`:exit/故障时**丢弃 TSS 栈**,恢复保存的 ESP 与 callee-saved 寄存器、段寄存器回到 0x10,`sti; ret` 回到 `user_run_embedded`。注意同特权级 iret 不会恢复 ESP/SS,所以不能用 iret 回内核。
- `registers_t` 布局对 Ring3 帧额外包含 useresp/ss,与 isr_stubs.s 的压栈顺序一致。

### 7.3 int 0x80 分发(syscall.c)

- `syscall_entry`:push 0/0x80 → 与 isr_common 相同的保存序列 → `syscall_handler(registers_t*)` → `regs->eax` 即返回值 → iret 回用户。
- Linux i386 ABI:eax=调用号,ebx/ecx/edx(以及 esi/edi/ebp)传参;错误用 `-errno`。
- 已实现(按用途分组):
  - 进程/stdio:`exit(1) exit_group(252) read(3) write(4) readv(145) writev(146) getpid(20) getppid(64) getuid/getgid/geteuid/getegid(24/47/49/50) 及其 32 位变体(199-202) getgroups(80/205)`;`rt_sigaction/rt_sigprocmask(174/175)` 空实现,`modify_ldt(123)` 返回 ENOSYS(由 set_thread_area 取代)
  - 文件:`open(5) close(6) lseek(19) llseek(140) dup(41) dup2(63,含 stdio 重定向) fcntl(55/221) access(33) getcwd(183) chdir(12) unlink(10) rename(38) sendfile(187/239)`
  - 目录/元数据:`getdents(141) getdents64(220) stat/lstat/fstat(106-108) stat64/lstat64/fstat64(195-197) statx(383) statfs/fstatfs(99/100) statfs64/fstatfs64(268/269) utimes(271) futimesat(299) utimensat(320)`
  - 内存/其他:`brk(45) old_mmap(90) mmap2(192) munmap(91) mprotect(125) uname(122) sysinfo(116) set/get_thread_area(243/244) time(13) gettimeofday(78) clock_gettime(265/403)`
  - 未知号返回 `-ENOSYS(-38)` 并在串口打印一次编号,作为 ROADMAP 阶段 4 的待办清单。
- fd 表在 user.c 统一管理 0..10:启动时 0/1/2 是 `USER_FD_STDIN/STDOUT/STDERR`,其余条目可为 RAM FS 文件、目录、`/dev` 节点、`/proc` 快照或已关闭(FREE)。新 fd 从 3 起分配;`dup(41)` 复制任意条目,`dup2(63)` 可把文件/设备重定向到 0..2(或反向),`dd` 的 `dup2(if_fd,0)` 由此可用。
- 路径解析 `vfs_normalize`:以进程 cwd(`user_cwd_get/set`,启动时 `/`)为基准处理绝对/相对路径、`.`、`..`(越界停在 `/`),输出规范绝对路径。命名空间:`/` 是 RAM FS(仅单段文件名),另有虚拟 `/dev`、`/proc`、`/etc`。
- 虚拟文件:`/dev/null` 读 EOF/写丢弃,`/dev/zero` 读零/写丢弃,`/dev/console` 接键盘与 VGA+串口;`/proc/{meminfo,uptime,version,cpuinfo,mounts}` 与 `/etc/mtab` 在 `open` 时由内核生成快照存入 fd,`/proc/meminfo` 取 mm 页统计,`uptime` 取 PIT 100Hz。
- `getdents/getdents64` 按目录 fd 的绝对路径枚举 `.`、`..`、RAM FS 文件以及 `dev/proc/etc` 虚拟子目录,分别按 i386 `linux_dirent`(10 字节头)与 `linux_dirent64`(19 字节头)打包;`d_off` 写下一项序号,缓冲区不足时保留偏移。
- `stat` 族:`stat64` 写 96 字节 i386 布局,旧 `stat` 写 64 字节布局;目录 `S_IFDIR|0755`/nlink 2,文件 `S_IFREG|0644`,设备 `S_IFCHR|0666`,`/proc` 快照 `S_IFREG|0444`,`st_dev=1`;三类时间来自 RAM FS 的 atime/mtime/ctime。`statx(383)` 写 256 字节 `struct statx`(mask=STATX_BASIC_STATS),空路径且 `AT_EMPTY_PATH` 时改为 stat dirfd。
- `statfs/fstatfs(99/100)` 与 `statfs64/fstatfs64(268/269)` 按 16×64KB 容量与空闲槽位填充,`df`/`stat -f` 可用;`sysinfo(116)` 回报 uptime/内存/任务数。`/proc/mounts` 输出 `/dev/root / ext2` 等真实设备行,BusyBox `df` 会跳过 `rootfs` 伪设备。
- `write/read` 按 fd 类型分发:`STDIN`/`DEV_CONSOLE` 读复用 `keyboard_getchar`(内部 `sti; hlt; cli`),`STDOUT/STDERR/DEV_CONSOLE` 写 VGA+串口,`FILE` 走 `fs_read_at/fs_write_at`,`VFILE` 读内核快照、写返回 EACCES,`DEV_NULL/DEV_ZERO` 分别 EOF/零填充与丢弃写入。所有用户缓冲区先用 `user_ptr_ok` 校验。
- `writev/readv` 按 iovec 顺序复用 `do_write/do_read`,部分成功时返回已处理字节数。musl 的 stdio 缓冲正是用 `writev` 刷出——这是 BusyBox `ls` 能打印的关键。
- `sendfile64(239)` 从 in_fd(可选用户 offset 指针)读入内核缓冲并写 out_fd,更新偏移;BusyBox 的 fd 拷贝优先用它,失败再退回 read/write。
- `brk` 与匿名 `mmap` 共用沙箱 bump 指针:brk 只改 `brk_current`(页早已映射),mmap 按页对齐分配并清零;文件映射、`MAP_FIXED` 拒绝。`munmap/mprotect` 是空操作(平坦沙箱)。
- `uname` 按 Linux i386 的 6×65 字节 utsname 填充 MuOS/muos/1.2.0/i386;`unlink/rename` 真正修改 RAM FS;`utimes/futimesat/utimensat` 会写入 atime/mtime(支持 `UTIME_NOW/OMIT`),`fs_set_times` 同时刷新 ctime。
- 时钟:CMOS RTC 读出完整年月日(处理 BCD/12h/世纪寄存器),`rtc_datetime_to_epoch` 用 civil-days 算法换算 UTC 秒;`time(13)`、`gettimeofday(78)`、`clock_gettime(265/403)` 返回真实 epoch,monotonic 时钟用 PIT ticks,亚秒精度 10ms。

### 7.4 ELF32 加载器(elf.c)

- 仅接受 `ET_EXEC` + `EM_386` + 静态镜像;校验 class/data/version、e_phentsize/phnum 边界(p_offset+p_filesz 不越界)、每个 `PT_LOAD` 完全落在 `[USER_BASE, USER_END-64KB)`。
- 逐段 `memcpy` + bss `memset`,记录最高装入地址作为 brk 起点;入口必须在沙箱内。
- 镜像由 build.ps1 用同一交叉编译器链接到 0x08048000,再用 `objcopy -I binary -O elf32-i386` 变成 `hello_elf.o` 链进内核(`_binary_hello_elf_start/end`);运行前整段 memset 清零,重复 `run` 安全。
- 三个入口共用 `user_run_image_argv(image,size,argc,argv)`:`run` 用内嵌 blob,`exec <file>` 用 `fs_open` + `fs_fd_data/size` 从 RAM FS 读取,`execmod <n> [args...]` 用 Multiboot module。ELF 加载器只读源镜像,不依赖它在沙箱内。
- `execmod` 先把模块镜像复制到 `mm_alloc_pages` 的连续内核页再清零沙箱;模块页在 mm_init 已预留,不会被堆/页表覆盖。

### 7.5 用户程序(user/)

- `start.s`:`_start` 从内核构造的 System V 栈布局取 argc/argv,调用 `main`,返回值交给 `muos_exit`。
- 初始栈完整布局:`argc, argv[], NULL, envp NULL, auxv AT_NULL, strings...`。真实 musl/glibc 会遍历 envp/auxv,缺终止符会把字符串字节当指针(busybox 曾因此在 `strcmp` 里 Page Fault)。
- `runtime.c`:内联 `int $0x80` 的 muos_* 封装、字符串/数字输出、memset/memcpy(用户镜像 -nostdlib 需要)。不是 libc。
- `hello.c`:打印 CS(验证 0x1B)、argc/argv、1..100 求和、栈数组、brk 写读、`uname`、RAM FS 打开/读取/写入/回读、匿名 `mmap`、键盘 read;`run fault` 参数让它故意写 0xC0000000。
- 用户态异常在 `isr_handler` 开头分流:`(regs->cs & 3) == 3 && user_is_active()` → `user_handle_fault` 打印 EIP/CS/CR2 后回 Shell,内核 panic 路径不受影响。

### 7.6 i386 TLS 与真实 libc 启动

- GDT 槽 6 是 DPL3 的 TLS 数据段(`gdt_set_user_tls`);`set_thread_area(243)` 读用户 `user_desc`(16 字节),接受 entry -1/6,写入 base/limit/flags,回填 `entry_number=6`。用户代码随后 `mov %gs,0x33`。
- 描述符:access 0xF2(可写数据)/0xF0(只读),gran 0x40(字节)或 0xC0(4KB);limit 取 20 位。
- `get_thread_area(244)` 回读;`exit_group(252)` 走 exit;`set_tid_address(258)` 返回 1;`getuid32/...` 返回 0;`ioctl` 返回 `-ENOTTY`;`dup2` 只接受同 fd。
- **验收**:Alpine static i386 BusyBox 1.36.1(musl)作为 `-initrd` 模块进入 Ring3;`echo`、`ls /`、`cat /notes.txt`、`pwd`、`id`、`uname -a` 及 20+ applet(含 `wc/grep/sed/od/stat/cp/rm/mv/touch`)实测通过,不存在的路径返回 ENOENT。

## 八、Shell / FS / 桌面

- **shell.c**:128 字节行编辑(方向键/历史/Tab 补全),`tokenize` 有 MAX_ARGS=16 硬边界(曾有 17+ token 栈溢出漏洞);29 条命令 if/else 分发(与补全表 cmds[] 双份维护,改动时两处同步);`run [fault]` 运行内嵌 ELF,`exec <file>` 运行 RAM FS 中的 ELF,`mods`/`execmod <n> [args...]` 运行 Multiboot module。启动时把内嵌 ELF 复制为 `hello.elf`;`cat` 按 fd 流式输出完整 64KB 文件。
- **fs.c**:RAM 文件系统,16 槽 × 64KB(能存放静态 i386 ELF),fd 即槽位下标(无句柄/引用保护——`fs_open` 后文件可被删,这是已知局限)。除 shell 用的整文件 write/read 外,`fs_read_at/fs_write_at/fs_fd_truncate` 供系统调用层按偏移读写,`fs_rename` 支持同目录改名/覆盖(重名校验、内容保留)。架构级改造见 ROADMAP。
- **editor.c**:128 行 × 79 列全屏编辑器;静态 10KB IO 缓冲(修复旧版 1KB 存盘截断),^S 保存 ^R 重载,PgUp/PgDn 翻页;运行时隐藏硬件光标、用反色单元作光标。
- **win7.c**:文本模式 Win7 风格桌面——行 0 标题、1..23 桌面、24 任务栏;窗口按 z-order 绘制(活动窗口最后),鼠标点击/标题栏拖拽/最小化/最大化/关闭,开始菜单鼠标/键盘双操作,桌面图标可见;任务栏显示实时内存与 CMOS RTC `HH:MM:SS`。`gui`/`win7`/`desktop` 命令共用此实现;`run` 之外的 Text Editor/Calculator 可从菜单或图标启动(Calculator 聚焦时直接吃数字/运算符)。
- **mouse.c + rtc.c**:PS/2 鼠标初始化(0xA8/命令字节/0xF6/0xF4)与 3 字节包解析(9 位 dx/dy、溢出位、按键),映射为 80×25 文本单元;`rtc_read_time` 处理 UIP、BCD/12 小时制,失败时任务栏回退 `UP:MM`。
- **vga.c**:`vga_hide_cursor/show_cursor` 用 CRTC start/end 保存/恢复 BIOS 光标形状,桌面与编辑器绘制自己的光标。

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

## 十一、已知局限(真实清单)

1. 用户态是**单进程、固定 8MB 沙箱**:无 higher-half、无每进程页目录、无 fork/调度;一次只跑一个用户程序(ROADMAP 阶段 1 剩余项)
2. 系统调用仍缺进程/并发原语:`fork/clone/waitpid/signals/pipe/execve/mkdir` 与文件 mmap 均未实现;`/proc` 只有静态文件,没有 `/proc/<pid>`,因此 `ps` 不可用(ROADMAP 阶段 2/4)
3. ELF 可从内核内嵌、RAM FS(`exec`)或 Multiboot module(`execmod`)加载;BusyBox 的 `ls/cat/free/df/dd` 已跑通,但还不能 fork+execve 运行子进程,也没有管道/多级目录/磁盘
4. 内核堆不可增长、不可重入(IRQ 上下文需手工加锁)
5. fs 无句柄保护、无目录层级/持久化;时间戳仅存于 RAM FS,重启丢失;write 截断不告警;`/dev`、`/proc`、`/etc` 为只读虚拟层
6. 键盘仅 US QWERTY;PS/2 鼠标是文本模式粗粒度指针(按 8×16 单元),无滚轮/右键菜单/双击标题栏最大化
7. 桌面仍是 80×25 文本模式,无图形帧缓冲、字体渲染与动画;窗口内容除 Calculator 外主要是状态展示
8. 单处理器(PIC,非 APIC);内核仍是协作式单任务
9. 无内核栈溢出保护(16KB 栈,GUI 深调用链靠它撑着;用户态入口另有 8KB TSS 栈)
10. 无 guard page / 无 KASLR / panic 信息只有寄存器 dump(有 -g,可 addr2line)
11. NT/PE 兼容尚未开始:执行前端目前只有 ELF32(ROADMAP 阶段 5)
