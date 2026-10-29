#include "user.h"
#include "elf.h"
#include "mm.h"
#include "tss.h"
#include "vga.h"
#include "serial.h"
#include "string.h"

/* Embedded user image: built by build.ps1 into build/hello.elf and linked
 * into the kernel as a raw binary blob (see the objcopy step). */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];

/* Saved kernel ESP at user_enter (callee-saved regs + return address live
 * above it). Written by isr_stubs.s, read by user_return_to_kernel. */
uint32_t user_kernel_esp_save = 0;

static bool_t   space_claimed = false;
static bool_t   active = false;
static bool_t   faulted = false;
static int      exit_code = 0;
static uint32_t brk_base = 0;
static uint32_t brk_current = 0;

typedef struct {
    int      fs_slot;   /* -1 = free */
    uint32_t offset;
} user_fd_t;

static user_fd_t user_fds[USER_FD_MAX];

static uint32_t align_up_page(uint32_t v) {
    return (v + PAGE_SIZE - 1) & PAGE_MASK;
}

void user_fds_reset(void) {
    for (int i = 0; i < USER_FD_MAX; i++) {
        user_fds[i].fs_slot = -1;
        user_fds[i].offset = 0;
    }
}

int user_fd_alloc(int fs_slot) {
    for (int i = 0; i < USER_FD_MAX; i++) {
        if (user_fds[i].fs_slot < 0) {
            user_fds[i].fs_slot = fs_slot;
            user_fds[i].offset = 0;
            return 3 + i;   /* 0..2 are stdio */
        }
    }
    return -1;
}

static int fd_index(int fd) {
    if (fd < 3 || fd >= 3 + USER_FD_MAX) return -1;
    return fd - 3;
}

int user_fd_slot(int fd) {
    int i = fd_index(fd);
    if (i < 0) return -1;
    return user_fds[i].fs_slot;
}

uint32_t user_fd_offset(int fd) {
    int i = fd_index(fd);
    if (i < 0) return 0;
    return user_fds[i].offset;
}

bool_t user_fd_set_offset(int fd, uint32_t offset) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].fs_slot < 0) return false;
    user_fds[i].offset = offset;
    return true;
}

void user_fd_close(int fd) {
    int i = fd_index(fd);
    if (i < 0) return;
    user_fds[i].fs_slot = -1;
    user_fds[i].offset = 0;
}

uint32_t user_mmap_anon(uint32_t len) {
    if (len == 0) return 0;
    uint32_t rounded = (len + PAGE_SIZE - 1) & PAGE_MASK;
    if (rounded < len) return 0;                       /* overflow */
    uint32_t start = align_up_page(brk_current);
    uint32_t limit = USER_END - USER_STACK_RESERVE;
    if (start >= limit || rounded > limit - start) return 0;
    brk_current = start + rounded;
    memset((void*)start, 0, rounded);
    return start;
}

const void* user_get_image(uint32_t* size) {
    uint32_t n = (uint32_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    if (size) *size = n;
    return _binary_hello_elf_start;
}

bool_t user_is_active(void) {
    return active;
}

bool_t user_ptr_ok(uint32_t ptr, uint32_t len) {
    if (ptr < USER_BASE) return false;
    if (ptr > USER_END) return false;
    return len <= USER_END - ptr;   /* overflow-safe */
}

uint32_t user_get_brk(void) {
    return brk_current;
}

bool_t user_set_brk(uint32_t addr) {
    if (addr < brk_base || addr > USER_END - USER_STACK_RESERVE) return false;
    brk_current = addr;
    return true;
}

/* Small serial-only helpers (the VGA versions live in vga.c) */
static void serial_hex(uint32_t v) {
    char b[11] = "0x00000000";
    for (int i = 0; i < 8; i++) {
        uint8_t nib = (v >> ((7 - i) * 4)) & 0xF;
        b[2 + i] = nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10);
    }
    serial_write(b);
}

static void serial_dec(uint32_t v) {
    char b[12];
    u32_to_dec(b, v);
    serial_write(b);
}

void user_exit(int code) {
    exit_code = code;
    faulted = false;
    active = false;
    user_return_to_kernel();
    for (;;) { /* not reached */ }
}

void user_handle_fault(registers_t* regs) {
    faulted = true;
    active = false;

    uint32_t cr2 = 0;
    if (regs->int_no == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    vga_setcolor(vga_entry_color(VGA_WHITE, VGA_RED));
    vga_print("\n[user] Ring3 process faulted: ");
    vga_print(regs->int_no < 32 ? exception_names[regs->int_no] : "unknown");
    vga_print("\n  EIP="); vga_print_hex(regs->eip);
    vga_print(" CS="); vga_print_hex(regs->cs);
    if (regs->int_no == 14) {
        vga_print(" CR2="); vga_print_hex(cr2);
    }
    vga_print("\n  Returning to shell.\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    serial_write("\n[user] fault: ");
    serial_write(regs->int_no < 32 ? exception_names[regs->int_no] : "unknown");
    serial_write(" EIP="); serial_hex(regs->eip);
    serial_write(" CS=");  serial_hex(regs->cs);
    if (regs->int_no == 14) {
        serial_write(" CR2="); serial_hex(cr2);
    }
    serial_write("\n");

    user_return_to_kernel();
    for (;;) { /* not reached */ }
}

/* Copy a NUL-terminated arg onto the downward-growing user stack */
static uint32_t push_string(uint32_t sp, const char* s) {
    uint32_t len = strlen(s) + 1;
    if (len > 256) len = 256;
    sp -= len;
    strncpy((char*)sp, s, len - 1);
    ((char*)sp)[len - 1] = '\0';
    return sp;
}

/* Build the initial Linux-style process stack:
 *   [argc][argv0][argv1][NULL][strings...]
 * arg1 may be NULL (argc == 1). */
static uint32_t build_user_stack(const char* arg0, const char* arg1) {
    uint32_t sp = USER_END - 32;
    uint32_t argc = 1;
    uint32_t arg1_addr = 0;

    if (arg1 && *arg1) {
        sp = push_string(sp, arg1);
        arg1_addr = sp;
        argc++;
    }

    sp = push_string(sp, arg0);
    uint32_t arg0_addr = sp;

    sp -= 4;
    *(uint32_t*)sp = 0;             /* argv[argc] = NULL */
    if (argc == 2) {
        sp -= 4;
        *(uint32_t*)sp = arg1_addr; /* argv[1] */
    }
    sp -= 4;
    *(uint32_t*)sp = arg0_addr;     /* argv[0] */
    sp -= 4;
    *(uint32_t*)sp = argc;
    return sp;
}

int user_run_embedded(const char* arg0, const char* arg1) {
    uint32_t img_size = 0;
    const void* img = user_get_image(&img_size);
    return user_run_image(img, img_size, arg0, arg1);
}

int user_run_image(const void* img, uint32_t img_size,
                   const char* arg0, const char* arg1) {
    if (active) {
        vga_print("A user program is already running.\n");
        return -1;
    }
    if (!img || img_size == 0) {
        vga_print("[user] empty executable\n");
        return -1;
    }

    uint32_t ram_bytes = mm_get_total_pages() * PAGE_SIZE;
    if (USER_END > ram_bytes) {
        vga_setcolor(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("[user] not enough RAM for the 0x08048000 sandbox (need >128MB)\n");
        vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        serial_write("[user] sandbox above physical RAM\n");
        return -1;
    }

    if (!space_claimed) {
        if (!mm_reserve_range(USER_BASE, USER_END)) {
            vga_print("[user] sandbox pages unavailable (already claimed)\n");
            serial_write("[user] sandbox reservation failed\n");
            return -1;
        }
        space_claimed = true;
    }

    /* Fresh process image every run. */
    memset((void*)USER_BASE, 0, USER_SIZE);

    uint32_t entry = 0, image_end = 0;
    int rc = elf32_load(img, img_size, USER_BASE,
                        USER_END - USER_STACK_RESERVE, &entry, &image_end);
    if (rc != 0) {
        vga_setcolor(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("[user] ELF rejected (err ");
        vga_print_dec((uint32_t)(-rc));
        vga_print(")\n");
        vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        serial_write("[user] ELF rejected\n");
        return -1;
    }

    brk_base = align_up_page(image_end);
    if (brk_base < USER_BASE + PAGE_SIZE) brk_base = USER_BASE + PAGE_SIZE;
    brk_current = brk_base;

    uint32_t user_sp = build_user_stack(arg0 ? arg0 : "hello", arg1);
    if (user_sp <= brk_current + PAGE_SIZE) {
        vga_print("[user] image leaves no room for the stack\n");
        return -1;
    }

    if (!mm_user_map_range(USER_BASE, USER_END)) {
        vga_print("[user] failed to make the sandbox user-accessible\n");
        serial_write("[user] mm_user_map_range failed\n");
        return -1;
    }

    exit_code = 0;
    faulted = false;
    user_fds_reset();
    active = true;
    tss_set_esp0(tss_get_kernel_stack_top());

    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("[user] entering Ring3 at ");
    vga_print_hex(entry);
    vga_print(" (ELF ");
    vga_print_dec(img_size);
    vga_print(" bytes)\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    serial_write("[user] enter ring3 app=");
    serial_write(arg0 ? arg0 : "hello");
    serial_write(" entry="); serial_hex(entry);
    serial_write(" size="); serial_dec(img_size);
    serial_write("\n");

    /* Drops to CPL3; returns here only after sys_exit or a user fault. */
    user_enter(entry, user_sp);

    active = false;
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    if (faulted) {
        vga_print("[user] process terminated by fault\n");
        serial_write("[user] process terminated by fault\n");
    } else {
        vga_print("[user] process exited with code ");
        vga_print_dec((uint32_t)exit_code);
        vga_putchar('\n');
        serial_write("[user] exit code=");
        serial_dec((uint32_t)exit_code);
        serial_write("\n");
    }
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    return faulted ? -1 : exit_code;
}
