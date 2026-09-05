# MuOS 代码逻辑(v0.3)

本文对应重构后的代码现状:multiboot mmap 解析、全 RAM 恒等映射、内核堆、单任务模型、46 项自检。演进计划见 ROADMAP.md。

## 一、总体结构

```
启动(汇编)          内核入口              硬件抽象                系统服务              用户可见
boot.s          →   kernel.c         →   gdt/idt/isr/pic/irq  →  mm.c  kheap.c  →  shell.c(23 命令)
boot_cd.s           初始化序列            pit keyboard serial        string.c fs.c      editor.c
isr_stubs.s                              vga mouse                  task.c test.c      win7.c(桌面)
```

- `src/` 平铺 20 个 `.c` + 3 个 `.s`;`include/` 配对头文件,guard 统一 `MUOS_*` 前缀。
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
→ mm_init(mb, kernel_end)         [mmap 解析 + 位图 + 全 RAM 恒等映射]
→ mm_enable_paging()              [CR0.PG|WP]
→ kheap_init()                    [1MB 内核堆]
→ PIT 100Hz + IRQ0 → keyboard → mouse
→ task_init + task_register("kernel")
→ tests_run()                     [46 项自检,失败打印 SELF-TEST FAILED]
→ 启动动画 → shell_run()(内核栈上)
```

## 三、中断体系

- **IDT 256 项全覆盖**:0–31 异常、0x20–0x2F IRQ、**0x30–0xFF 填充 `isr_ignore` 桩(纯 iret)**——未预期向量/伪中断安静返回,不会 #NF 级联。**0x80 预留给未来 int 0x80 系统调用**(ROADMAP 阶段 2)。
- **调用链**:`ISR/IRQ 桩(压栈)→ isr_common/irq_common(保存全寄存器+段)→ C handler → 恢复 → iret`。
- **irq.c**:分发到 irq_handlers[16](keyboard 用 IRQ1,timer 用 IRQ0),处理伪 IRQ7/15(ISR 寄存器检查),每 IRQ 发 EOI。
- **isr.c**:CPU 异常一律致命 → 打印异常名 + EIP/CS/EFLAGS/通用寄存器 dump → 停机。`crash` 命令即走此路径。
- **EFLAGS 纪律**:内核里禁止裸 `cli…sti` 对(会破坏 IRQ 上下文和调用方临界区)。keyboard.c 的 `kbd_lock/kbd_unlock`(pushfl/popfl)是标准模式,新代码请照抄。

## 四、内存管理(mm.c)

### 4.1 mmap 解析

- `include/multiboot.h`:完整的 `multiboot_info_t`。**mmap_length 在偏移 44、mmap_addr 在偏移 48,中间隔着 syms[4]**——截断定义会读错位。
- Pass 1:遍历 mmap 条目(`p += entry->size + 4`),type==1 的区域取最高 end,clamp 到 `MM_MAX_RAM_MB`(1GB,32 位上限防护),得到 `ram_top`。
- 无 mmap(ISO 路径 EBX=0)→ 回退 `mem_upper + 1MB`;再无 → 默认 32MB。
- >4GB 条目:base 在 ram_cap 之外直接不计,跨越部分截断。

### 4.2 物理位图分配器

- 位图放 `_kernel_end` 后;**先全置 1(used),再把可用区间清 0**——1MB 以下传统区、BIOS/EBDA、mmap 空洞天然保持 reserved。位图尾部越界位也置 1(total_pages 非整除 32 时安全)。
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
- 保留物:`task_t`(pid/name/state/next)、`task_register/get_current/get_count/list` 供 `tasks`/`ps`/自检;GDT 槽 3/4 已备 Ring3 代码/数据段选择子(0x0B/0x13)。
- task_t 由 kmalloc 分配。

## 七、Shell / FS / 桌面

- **shell.c**:128 字节行编辑(方向键/历史/Tab 补全),`tokenize` 有 MAX_ARGS=16 硬边界(曾有 17+ token 栈溢出漏洞);23 条命令 if/else 分发(与补全表 cmds[] 双份维护,改动时两处同步)。
- **fs.c**:RAM 文件系统,64 槽 × 1KB,fd 即槽位下标(无句柄/引用保护——`fs_open` 后文件可被删,这是已知局限);write/read 截断无告警。架构级改造见 ROADMAP。
- **editor.c**:229 行全屏编辑器,128 行 × 79 列,^S 保存 ^R 重载(需文件名)。
- **win7.c**:文本模式伪图形桌面——8 窗口管理(开/关/最大化/拖拽去抖)、开始菜单状态机、任务栏时钟/内存显示。`gui`/`desktop` 命令已并入此实现。

## 八、构建系统

- **build.ps1**:`-Clean/-Run/-Iso` 组合;工具探测顺序 PATH → toolchain\ → tools\ → 常见安装位置;编译 `-m32 -ffreestanding -O2 -g -Wall -Wextra -fno-tree-loop-distribute-patterns -nostdinc`;链接经 `i686-elf-gcc -nostdlib -T linker.ld`(GCC 驱动而非裸 ld)。
- **make_iso.py**:PROJECT 取脚本所在目录;objcopy 出 flat kernel.bin,bootloader 在偏移 4092 补丁内核大小;pycdlib 打 El Torito ISO。
- ISO 路径实测:`-cdrom` 启动 → 自检全 PASS(修复前该路径直接把魔数当指令执行)。

## 九、关键全局量

| 符号 | 位置 | 说明 |
|---|---|---|
| `_kernel_end` | linker.ld | 内核镜像结束,位图起点 |
| `bitmap / total_pages / free_pages` | mm.c | 物理分配器状态 |
| `page_directory / page_dir_phys` | mm.c | 页目录(恒等映射全部 RAM) |
| `task_list_head / current_task / task_count` | task.c | 任务 introspection |
| `kbd_buffer/head/tail` | keyboard.c | 256 字节键盘环形缓冲 |
| `pit_ticks` | pit.c | 100Hz tick(uptime = ticks/100) |

## 十、已知局限(真实清单)

1. 无用户态:全部 Ring 0,无 TSS,无每进程页目录(ROADMAP 阶段 1)
2. 无系统调用接口:0x80 向量已预留空桩(ROADMAP 阶段 2)
3. 无 ELF 加载器,不能运行外部程序(ROADMAP 阶段 3)
4. 内核堆不可增长、不可重入(IRQ 上下文需手工加锁)
5. fs 无句柄保护、无目录/时间戳/持久化;write 截断不告警
6. 键盘仅 US QWERTY;mouse 是假驱动(固定坐标,真 PS/2 驱动在 ROADMAP)
7. 单处理器(PIC,非 APIC);调度为协作式单任务
8. 无内核栈溢出保护(16KB 栈,GUI 深调用链靠它撑着)
9. 无 guard page / 无 KASLR / panic 信息只有寄存器 dump(有 -g,可 addr2line)
