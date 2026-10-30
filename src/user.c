#include "user.h"
#include "elf.h"
#include "mm.h"
#include "tss.h"
#include "gdt.h"
#include "vga.h"
#include "serial.h"
#include "string.h"
#include "proc.h"
#include "fs.h"
#include "pipe.h"
#include "keyboard.h"

/* Embedded user image: built by build.ps1 into build/hello.elf and linked
 * into the kernel as a raw binary blob (see the objcopy step). */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];

/* Same program built -fPIE and linked as ET_DYN at address 0. */
extern const uint8_t _binary_hello_pie_elf_start[];
extern const uint8_t _binary_hello_pie_elf_end[];

/* Dynamic-linking set: the user-mode linker, its shared library and the
 * dynamically linked test program (see user/ldmu.c, user/libmu.c). */
extern const uint8_t _binary_ld_mu_so_start[];
extern const uint8_t _binary_ld_mu_so_end[];
extern const uint8_t _binary_libmu_so_start[];
extern const uint8_t _binary_libmu_so_end[];
extern const uint8_t _binary_dyn_elf_start[];
extern const uint8_t _binary_dyn_elf_end[];

/* Saved kernel ESP at user_enter (callee-saved regs + return address live
 * above it). Written by isr_stubs.s, read by user_return_to_kernel. */
uint32_t user_kernel_esp_save = 0;
uint32_t user_kernel_esp_launch = 0;

static bool_t   active = false;
static bool_t   faulted = false;
static int      exit_code = 0;
static uint32_t brk_base = 0;
static uint32_t brk_current = 0;

typedef struct {
    int      kind;      /* user_fd_kind_t; FREE = slot unused */
    int      fs_slot;   /* RAM FS slot for FILE */
    uint32_t offset;
    int      pipe_id;
    char     path[USER_FD_PATH_MAX];   /* DIR: absolute directory */
    char     vbuf[USER_FD_VBUF_MAX];   /* VFILE: generated /proc text */
    uint32_t vlen;
} user_fd_t;

static user_fd_t user_fds[USER_FD_MAX];
static char cwd_path[USER_FD_PATH_MAX];

static uint32_t align_up_page(uint32_t v) {
    return (v + PAGE_SIZE - 1) & PAGE_MASK;
}

static int fd_index(int fd) {
    if (fd < 0 || fd >= USER_FD_MAX) return -1;
    return fd;
}

/* New file-like fds always start at 3; 0..2 stay as stdio unless dup2'd. */
static int fd_alloc_kind(int kind) {
    for (int i = 3; i < USER_FD_MAX; i++) {
        if (user_fds[i].kind == USER_FD_FREE) {
            memset(&user_fds[i], 0, sizeof(user_fds[i]));
            user_fds[i].kind = kind;
            user_fds[i].fs_slot = -1;
            return i;
        }
    }
    return -1;
}

static int fd_alloc_raw(void) {
    for (int i = 3; i < USER_FD_MAX; i++) {
        if (user_fds[i].kind == USER_FD_FREE) return i;
    }
    return -1;
}

/* Adjust the shared file/pipe reference counters for one fd entry. Unlinked
 * files stay alive while any fd (including a forked snapshot) holds a ref. */
static void fd_entry_ref(int delta, const user_fd_t* e) {
    if (!e) return;
    if (e->kind == USER_FD_FILE && e->fs_slot >= 0) {
        if (delta > 0) fs_ref(e->fs_slot);
        else           fs_unref(e->fs_slot);
    } else if (e->kind == USER_FD_PIPE_WRITE) {
        pipe_writer_add(e->pipe_id, delta);
    }
}

void user_fds_reset(void) {
    /* Only clears the live view. Each process's fd_state is the canonical
     * holder of its file/pipe refs; the scheduler saves/restores it and
     * proc_reap releases it. */
    for (int i = 0; i < USER_FD_MAX; i++) {
        memset(&user_fds[i], 0, sizeof(user_fds[i]));
        user_fds[i].fs_slot = -1;
    }
    user_fds[0].kind = USER_FD_STDIN;
    user_fds[1].kind = USER_FD_STDOUT;
    user_fds[2].kind = USER_FD_STDERR;
    strncpy(cwd_path, "/", USER_FD_PATH_MAX - 1);
    cwd_path[USER_FD_PATH_MAX - 1] = '\0';
}

int user_fd_alloc(int fs_slot) {
    int fd = fd_alloc_kind(USER_FD_FILE);
    if (fd < 0) return -1;
    user_fds[fd].fs_slot = fs_slot;
    fs_ref(fs_slot);
    return fd;
}

int user_fd_alloc_dir(const char* path) {
    int fd = fd_alloc_kind(USER_FD_DIR);
    if (fd < 0) return -1;
    if (!path || path[0] != '/') path = "/";
    strncpy(user_fds[fd].path, path, USER_FD_PATH_MAX - 1);
    user_fds[fd].path[USER_FD_PATH_MAX - 1] = '\0';
    return fd;
}

int user_fd_alloc_dev(int kind) {
    if (kind != USER_FD_DEV_NULL && kind != USER_FD_DEV_ZERO &&
        kind != USER_FD_DEV_CONSOLE && kind != USER_FD_DEV_ATA) return -1;
    return fd_alloc_kind(kind);
}

int user_fd_alloc_vfile(const char* data, uint32_t len) {
    int fd = fd_alloc_kind(USER_FD_VFILE);
    if (fd < 0) return -1;
    if (len > USER_FD_VBUF_MAX) len = USER_FD_VBUF_MAX;
    if (data && len) memcpy(user_fds[fd].vbuf, data, len);
    user_fds[fd].vlen = len;
    return fd;
}

int user_fd_alloc_pipe(int pipe_id, bool_t write_end) {
    int fd = fd_alloc_kind(write_end ? USER_FD_PIPE_WRITE : USER_FD_PIPE_READ);
    if (fd < 0) return -1;
    user_fds[fd].pipe_id = pipe_id;
    if (write_end) pipe_writer_add(pipe_id, +1);
    return fd;
}

int user_fd_pipe_id(int fd) {
    int i = fd_index(fd);
    if (i < 0) return -1;
    if (user_fds[i].kind != USER_FD_PIPE_READ &&
        user_fds[i].kind != USER_FD_PIPE_WRITE) return -1;
    return user_fds[i].pipe_id;
}

int user_fd_kind(int fd) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].kind == USER_FD_FREE) return -1;
    return user_fds[i].kind;
}

bool_t user_fd_is_dir(int fd) {
    int i = fd_index(fd);
    return (i >= 0 && user_fds[i].kind == USER_FD_DIR) ? true : false;
}

const char* user_fd_dir_path(int fd) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].kind != USER_FD_DIR) return "";
    return user_fds[i].path;
}

uint32_t user_fd_vfile(int fd, const char** data) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].kind != USER_FD_VFILE) return 0;
    if (data) *data = user_fds[i].vbuf;
    return user_fds[i].vlen;
}

/* Note: dup copies the descriptor entry including its offset; offsets are
 * not shared between duplicates (dd-style redirection does not care). */
int user_fd_dup(int oldfd) {
    int oi = fd_index(oldfd);
    if (oi < 0 || user_fds[oi].kind == USER_FD_FREE) return -1;
    int ni = fd_alloc_raw();
    if (ni < 0) return -1;
    user_fds[ni] = user_fds[oi];
    fd_entry_ref(+1, &user_fds[ni]);
    return ni;
}

bool_t user_fd_dup2(int oldfd, int newfd) {
    int oi = fd_index(oldfd);
    int ni = fd_index(newfd);
    if (oi < 0 || ni < 0) return false;
    if (user_fds[oi].kind == USER_FD_FREE) return false;
    if (oi == ni) return true;
    fd_entry_ref(-1, &user_fds[ni]);      /* release the replaced target */
    user_fds[ni] = user_fds[oi];
    fd_entry_ref(+1, &user_fds[ni]);
    return true;
}

int user_fd_slot(int fd) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].kind != USER_FD_FILE) return -1;
    return user_fds[i].fs_slot;
}

uint32_t user_fd_offset(int fd) {
    int i = fd_index(fd);
    if (i < 0) return 0;
    return user_fds[i].offset;
}

bool_t user_fd_set_offset(int fd, uint32_t offset) {
    int i = fd_index(fd);
    if (i < 0 || user_fds[i].kind == USER_FD_FREE) return false;
    user_fds[i].offset = offset;
    return true;
}

void user_fd_close(int fd) {
    int i = fd_index(fd);
    if (i < 0) return;
    fd_entry_ref(-1, &user_fds[i]);
    memset(&user_fds[i], 0, sizeof(user_fds[i]));
    user_fds[i].fs_slot = -1;
}

const char* user_cwd_get(void) {
    return cwd_path;
}

bool_t user_cwd_set(const char* path) {
    if (!path || path[0] != '/' || strlen(path) >= USER_FD_PATH_MAX) return false;
    strncpy(cwd_path, path, USER_FD_PATH_MAX - 1);
    cwd_path[USER_FD_PATH_MAX - 1] = '\0';
    return true;
}

uint32_t user_fd_state_bytes(void) {
    return (uint32_t)sizeof(user_fds);
}

void user_fd_state_save(void* dst) {
    if (dst) memcpy(dst, user_fds, sizeof(user_fds));
}

void user_fd_state_restore(const void* src) {
    if (src) memcpy(user_fds, src, sizeof(user_fds));
}

void user_fd_state_retain(void* state) {
    user_fd_t* s = (user_fd_t*)state;
    if (!s) return;
    for (int i = 0; i < USER_FD_MAX; i++) fd_entry_ref(+1, &s[i]);
}

void user_fd_state_release(void* state) {
    user_fd_t* s = (user_fd_t*)state;
    if (!s) return;
    for (int i = 0; i < USER_FD_MAX; i++) fd_entry_ref(-1, &s[i]);
}

/* Readiness of one fd in a saved snapshot (cross-process poll wakeups). */
int user_fd_state_events(void* state, int fd, int events) {
    const user_fd_t* s = (const user_fd_t*)state;
    if (!s || fd < 0 || fd >= USER_FD_MAX) return USER_POLLNVAL;
    const user_fd_t* e = &s[fd];
    if (e->kind == USER_FD_FREE) return USER_POLLNVAL;

    int rev = 0;
    switch (e->kind) {
        case USER_FD_STDIN:
        case USER_FD_DEV_CONSOLE:
            if ((events & USER_POLLIN) && keyboard_haschar()) rev |= USER_POLLIN;
            break;
        case USER_FD_FILE:
        case USER_FD_VFILE:
        case USER_FD_DIR:
        case USER_FD_DEV_NULL:
        case USER_FD_DEV_ZERO:
        case USER_FD_DEV_ATA:
            if (events & USER_POLLIN)  rev |= USER_POLLIN;
            if (events & USER_POLLOUT) rev |= USER_POLLOUT;
            break;
        case USER_FD_STDOUT:
        case USER_FD_STDERR:
            if (events & USER_POLLOUT) rev |= USER_POLLOUT;
            break;
        case USER_FD_PIPE_READ:
            if (pipe_valid(e->pipe_id)) {
                if ((events & USER_POLLIN) &&
                    (pipe_fill(e->pipe_id) > 0 || pipe_writers(e->pipe_id) == 0))
                    rev |= USER_POLLIN;
                if (pipe_writers(e->pipe_id) == 0) rev |= USER_POLLHUP;
            } else {
                rev |= USER_POLLERR;
            }
            break;
        case USER_FD_PIPE_WRITE:
            if (pipe_valid(e->pipe_id)) {
                if ((events & USER_POLLOUT) &&
                    pipe_fill(e->pipe_id) < PIPE_BUF_SIZE)
                    rev |= USER_POLLOUT;
                if (pipe_writers(e->pipe_id) == 0) rev |= USER_POLLERR;
            } else {
                rev |= USER_POLLERR;
            }
            break;
        default:
            break;
    }
    return rev;
}

int user_fd_state_kind(void* state, int fd) {
    const user_fd_t* s = (const user_fd_t*)state;
    if (!s || fd < 0 || fd >= USER_FD_MAX) return -1;
    if (s[fd].kind == USER_FD_FREE) return -1;
    return s[fd].kind;
}

int user_fd_state_pipe(void* state, int fd) {
    const user_fd_t* s = (const user_fd_t*)state;
    if (!s || fd < 0 || fd >= USER_FD_MAX) return -1;
    if (s[fd].kind != USER_FD_PIPE_READ && s[fd].kind != USER_FD_PIPE_WRITE)
        return -1;
    return s[fd].pipe_id;
}

void user_get_brk_state(uint32_t* base, uint32_t* cur) {
    if (base) *base = brk_base;
    if (cur) *cur = brk_current;
}

bool_t user_restore_brk_state(uint32_t base, uint32_t cur) {
    if (base < USER_BASE || cur < base || cur > USER_END) return false;
    brk_base = base;
    brk_current = cur;
    return true;
}

int user_last_exit_code(void) {
    return exit_code;
}

bool_t user_last_faulted(void) {
    return faulted;
}

void user_clear_exit_state(void) {
    exit_code = 0;
    faulted = false;
}

void user_set_active(bool_t v) {
    active = v;
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

const void* user_get_pie_image(uint32_t* size) {
    uint32_t n = (uint32_t)(_binary_hello_pie_elf_end -
                            _binary_hello_pie_elf_start);
    if (size) *size = n;
    return _binary_hello_pie_elf_start;
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

bool_t user_handle_fault(registers_t* regs) {
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

    /* The scheduler either switches to another process (returns here so
     * the ISR epilogue iret's into it) or resumes the shell (no return). */
    proc_exit_current(regs, 139, true);
    return true;
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
 *   argc, argv[], NULL, envp NULL, auxv AT_NULL, strings...
 * Real libcs (musl/glibc) walk envp and auxv; missing terminators make
 * them interpret the first string bytes as pointers. */
static uint32_t build_user_stack(int argc, const char* const* argv,
                                 const elf32_load_info_t* li,
                                 uint32_t interp_base) {
    uint32_t sp = USER_SIGTRAMP - 32;   /* top page is the sigreturn tramp */
    uint32_t addrs[16];

    if (argc < 1) argc = 1;
    if (argc > 16) argc = 16;
    for (int i = argc - 1; i >= 0; i--) {
        sp = push_string(sp, argv[i] ? argv[i] : "");
        addrs[i] = sp;
    }

    /* Auxiliary vector in final order; pushed in reverse below. A real
     * ld.so needs AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY/AT_BASE. */
    uint32_t aux[7][2] = {
        {  3, li ? li->phdr  : 0 },     /* AT_PHDR */
        {  4, li ? li->phent : 32 },    /* AT_PHENT */
        {  5, li ? li->phnum : 0 },     /* AT_PHNUM */
        {  6, 4096 },                   /* AT_PAGESZ */
        {  7, interp_base },            /* AT_BASE (interpreter bias) */
        {  9, li ? li->entry : 0 },     /* AT_ENTRY */
        {  0, 0 },                      /* AT_NULL */
    };
    for (int i = 6; i >= 0; i--) {
        sp -= 4; *(uint32_t*)sp = aux[i][1];
        sp -= 4; *(uint32_t*)sp = aux[i][0];
    }
    sp -= 4; *(uint32_t*)sp = 0;    /* envp[0] = NULL */
    sp -= 4; *(uint32_t*)sp = 0;    /* argv[argc] = NULL */
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 4; *(uint32_t*)sp = addrs[i];
    }
    sp -= 4; *(uint32_t*)sp = (uint32_t)argc;
    return sp;
}

/* Resolve a PT_INTERP path: the MuFS first (/lib/...), then the embedded
 * ld-mu.so blob (built into the kernel image at build time). */
static int interp_find(const char* path, const void** img, uint32_t* size) {
    if (!path || !*path) return -1;
    while (*path == '/') path++;
    if (strlen(path) >= 64) return -1;
    char norm[64];
    strncpy(norm, path, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';

    int slot = fs_open(norm);
    if (slot >= 0) {
        uint32_t n = fs_fd_size(slot);
        if (n) {
            *img = fs_fd_data(slot);
            *size = n;
            return 0;
        }
    }
    if (strcmp(norm, "lib/ld-mu.so") == 0 || strcmp(norm, "ld-mu.so") == 0) {
        *img = _binary_ld_mu_so_start;
        *size = (uint32_t)(_binary_ld_mu_so_end - _binary_ld_mu_so_start);
        return 0;
    }
    return -1;
}

/* Map the program (and its interpreter when PT_INTERP is present). Returns
 * the address to enter: the interpreter when dynamically linked, otherwise
 * the program entry. The program's own relocations are left to the
 * interpreter in the dynamic case (see user/ldmu.c). */
static int user_load_for_exec(const void* img, uint32_t img_size,
                              elf32_load_info_t* li, uint32_t* entry_out,
                              uint32_t* interp_base) {
    int rc = elf32_load_program(img, img_size, USER_BASE,
                                USER_END - USER_STACK_RESERVE, li);
    if (rc != 0) return rc;

    *entry_out = li->entry;
    *interp_base = 0;
    if (!li->has_interp) return 0;

    const void* iimg = NULL;
    uint32_t isz = 0;
    if (interp_find(li->interp, &iimg, &isz) != 0) return -5;

    elf32_load_info_t ii;
    rc = elf32_load_ex(iimg, isz, USER_INTERP_BASE,
                       USER_END - USER_STACK_RESERVE, &ii);
    if (rc != 0) return -6;

    *entry_out = ii.entry;      /* enter ld-mu.so, it jumps to AT_ENTRY */
    *interp_base = ii.base;
    return 0;
}

/* Replace the live sandbox with a new image (execve). Fds/cwd persist. */
int user_exec_image(const void* img, uint32_t img_size, int argc,
                    const char* const* argv,
                    uint32_t* entry_out, uint32_t* sp_out) {
    if (!img || img_size == 0) return -1;

    /* The top page is the read-only sigreturn trampoline. */
    memset((void*)USER_BASE, 0, USER_SIZE - PAGE_SIZE);

    elf32_load_info_t li;
    uint32_t entry = 0, interp_base = 0;
    int rc = user_load_for_exec(img, img_size, &li, &entry, &interp_base);
    if (rc != 0) return rc;

    uint32_t image_end = li.image_end;
    brk_base = align_up_page(image_end);
    if (brk_base < USER_BASE + PAGE_SIZE) brk_base = USER_BASE + PAGE_SIZE;
    brk_current = brk_base;

    uint32_t sp = build_user_stack(argc, argv, &li, interp_base);
    if (sp <= brk_current + PAGE_SIZE) return -2;

    exit_code = 0;
    faulted = false;
    active = true;
    if (entry_out) *entry_out = entry;
    if (sp_out) *sp_out = sp;
    return 0;
}

int user_run_embedded(const char* arg0, const char* arg1) {
    uint32_t img_size = 0;
    const void* img = user_get_image(&img_size);
    return user_run_image(img, img_size, arg0, arg1);
}

int user_run_pie_embedded(const char* arg0, const char* arg1) {
    uint32_t img_size = 0;
    const void* img = user_get_pie_image(&img_size);
    return user_run_image(img, img_size, arg0, arg1);
}

/* Write one embedded blob into the MuFS, replacing an existing copy. */
static void fs_install_blob(const char* path, const uint8_t* data,
                            uint32_t size) {
    int slot = fs_open(path);
    if (slot < 0) slot = fs_create(path);
    if (slot >= 0) fs_write(slot, (const char*)data, size);
}

void user_install_dyn_runtime(void) {
    if (!fs_is_dir("lib")) fs_mkdir("lib");
    fs_install_blob("lib/ld-mu.so", _binary_ld_mu_so_start,
                    (uint32_t)(_binary_ld_mu_so_end - _binary_ld_mu_so_start));
    fs_install_blob("lib/libmu.so", _binary_libmu_so_start,
                    (uint32_t)(_binary_libmu_so_end - _binary_libmu_so_start));
    fs_sync();
}

int user_run_dyn_embedded(void) {
    user_install_dyn_runtime();
    uint32_t size = (uint32_t)(_binary_dyn_elf_end - _binary_dyn_elf_start);
    return user_run_image(_binary_dyn_elf_start, size, "dyn", NULL);
}

int user_run_image(const void* img, uint32_t img_size,
                   const char* arg0, const char* arg1) {
    const char* argv[2];
    int argc = 1;
    argv[0] = arg0 ? arg0 : "app";
    if (arg1 && *arg1) {
        argv[1] = arg1;
        argc = 2;
    }
    return user_run_image_argv(img, img_size, argc, argv);
}

int user_run_image_argv(const void* img, uint32_t img_size,
                        int argc, const char* const* argv) {
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

    /* Every run gets its own 8MB physical block mapped at USER_BASE by a
     * private page directory. Switch into it before touching USER_BASE. */
    proc_t* self = proc_alloc(argc > 0 && argv[0] ? argv[0] : "app", 1);
    if (!self || !proc_snapshot_alloc(self)) {
        if (self) proc_reap(self);
        vga_print("[user] not enough memory for a process block\n");
        serial_write("[user] process block allocation failed\n");
        return -1;
    }
    uint32_t prev_pd = mm_get_current_pd();
    proc_switch_space(self);

    /* Fresh process image every run (top page: read-only trampoline). */
    memset((void*)USER_BASE, 0, USER_SIZE - PAGE_SIZE);

    elf32_load_info_t li;
    uint32_t entry = 0, interp_base = 0;
    int rc = user_load_for_exec(img, img_size, &li, &entry, &interp_base);
    if (rc != 0) {
        vga_setcolor(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
        if (rc == -5) {
            vga_print("[user] missing interpreter: ");
            vga_print(li.interp);
            vga_putchar('\n');
            serial_write("[user] missing interpreter\n");
        } else if (rc == -6) {
            vga_print("[user] interpreter rejected by the ELF loader\n");
            serial_write("[user] interpreter rejected\n");
        } else {
            vga_print("[user] ELF rejected (err ");
            vga_print_dec((uint32_t)(-rc));
            vga_print(")\n");
            serial_write("[user] ELF rejected\n");
        }
        vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        mm_switch_pd(prev_pd);
        proc_reap(self);
        return -1;
    }

    uint32_t image_end = li.image_end;
    brk_base = align_up_page(image_end);
    if (brk_base < USER_BASE + PAGE_SIZE) brk_base = USER_BASE + PAGE_SIZE;
    brk_current = brk_base;

    uint32_t user_sp = build_user_stack(argc, argv, &li, interp_base);
    if (user_sp <= brk_current + PAGE_SIZE) {
        vga_print("[user] image leaves no room for the stack\n");
        mm_switch_pd(prev_pd);
        proc_reap(self);
        return -1;
    }

    exit_code = 0;
    faulted = false;
    user_fds_reset();
    active = true;

    if (!proc_fdstate_alloc(self)) {
        vga_print("[user] no memory for fd state\n");
        mm_switch_pd(prev_pd);
        proc_reap(self);
        return -1;
    }
    user_fd_state_save(self->fd_state);
    self->fds_retained = true;
    strncpy(self->cwd, user_cwd_get(), USER_FD_PATH_MAX - 1);
    self->cwd[USER_FD_PATH_MAX - 1] = '\0';
    user_get_brk_state(&self->brk_base, &self->brk_cur);
    gdt_set_user_tls(0, 0, 0);          /* fresh TLS base for this program */
    self->tls_base = self->tls_limit = self->tls_flags = 0;

    self->state = PROC_RUNNING;
    self->cmdline[0] = '\0';
    for (int i = 0; i < argc && i < 16; i++) {
        uint32_t pos = strlen(self->cmdline);
        if (pos + 1 >= PROC_CMDLINE_MAX) break;
        if (i) self->cmdline[pos++] = ' ';
        const char* a = argv[i] ? argv[i] : "";
        while (*a && pos + 1 < PROC_CMDLINE_MAX) self->cmdline[pos++] = *a++;
        self->cmdline[pos] = '\0';
    }
    proc_set_current(self);

    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("[user] entering Ring3 at ");
    vga_print_hex(entry);
    vga_print(" (ELF ");
    vga_print_dec(img_size);
    vga_print(" bytes)\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    serial_write("[user] enter ring3 app=");
    serial_write((argc > 0 && argv[0]) ? argv[0] : "app");
    serial_write(" entry="); serial_hex(entry);
    serial_write(" size="); serial_dec(img_size);
    serial_write("\n");

    /* Drops to CPL3; returns here only after sys_exit or a user fault. */
    user_enter(entry, user_sp);

    active = false;
    /* The scheduler returns to the shell page directory before this call
     * resumes; make sure it really is the kernel one. */
    mm_switch_pd(prev_pd);
    tss_set_esp0(tss_get_kernel_stack_top());

    int final_code = self ? self->exit_code : exit_code;
    bool_t final_fault = self ? self->faulted : faulted;

    vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    if (final_fault) {
        vga_print("[user] process terminated by fault\n");
        serial_write("[user] process terminated by fault\n");
    } else {
        vga_print("[user] process exited with code ");
        vga_print_dec((uint32_t)final_code);
        vga_putchar('\n');
        serial_write("[user] exit code=");
        serial_dec((uint32_t)final_code);
        serial_write("\n");
    }
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    proc_cleanup_children(self->pid);
    proc_reap(self);
    proc_set_current(proc_find(1));
    user_fds_reset();       /* clear the stale view; refs were released above */

    return final_fault ? -1 : final_code;
}
