/* ldmu.c - ld-mu.so, the MuOS dynamic linker (ET_DYN, no dependencies).
 *
 * The kernel maps the main program's segments (PT_INTERP present) and this
 * interpreter, then enters `_start` with the program's original stack. This
 * code walks the auxiliary vector, maps every DT_NEEDED shared library from
 * the MuFS (/lib), resolves symbols through the SysV hash table and applies
 * the RELATIVE / 32 / PC32 / GLOB_DAT / JMP_SLOT relocations of each object,
 * then jumps to AT_ENTRY with the untouched stack.
 *
 * Freestanding: no libc, syscalls via int 0x80, local memcpy/memset.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_BASE   7
#define AT_ENTRY  9

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_TLS      7

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_REL      17
#define DT_RELSZ    18
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_GNU_HASH 0x6ffffef5

#define R_386_NONE     0
#define R_386_32       1
#define R_386_PC32     2
#define R_386_COPY     5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_TLS_TPOFF  14
#define R_386_TLS_IE     15
#define R_386_TLS_GOTIE  16
#define R_386_TLS_LE     17
#define R_386_TLS_IE_32  33
#define R_386_TLS_LE_32  34
#define R_386_TLS_DTPMOD32 35
#define R_386_TLS_DTPOFF32 36
#define R_386_TLS_TPOFF32  37
#define R_386_IRELATIVE 42

#define SHN_UNDEF 0
#define SHN_ABS   0xFFF1

#define PROG_BASE  0x08048000u    /* kernel maps the program's lowest segment here */
#define LIB_BASE   0x08400000u    /* first shared library address */
#define MAX_OBJ    4
#define FILE_MAX   65536u
/* Static TLS block arena (i386 variant II): TP sits at TLS_TP, module
 * blocks are allocated downwards from it. The address lies between the
 * shared libraries (0x08400000) and the user stack reserve. */
#define TLS_TP     0x08780000u

typedef struct {
    u8  e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} ehdr_t;

typedef struct {
    u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} phdr_t;

typedef struct { u32 d_tag, d_val; } dyn_t;
typedef struct { u32 r_offset, r_info; } rel_t;
typedef struct {
    u32 st_name, st_value, st_size;
    u8  st_info, st_other;
    u16 st_shndx;
} sym_t;

typedef struct obj {
    u32 bias;
    u32 dyn;
    u32 symtab, strtab, hash, syment;
    u32 gnu_hash;
    u32 rel, relsz, jmprel, pltrelsz;
    u32 tls_present, tls_vaddr, tls_filesz, tls_memsz, tls_align;
    int tls_off;                 /* block start relative to TP (negative) */
    u32 tls_id;                  /* module id for DTPMOD32 / __tls_get_addr */
    const char* name;
} obj_t;

static obj_t objs[MAX_OBJ];
static char  objnames[MAX_OBJ][64];
static int   nobj = 0;
static u32   next_base = LIB_BASE;
static u8    fbuf[FILE_MAX];
static u32   tls_tp = TLS_TP;
static u32   tls_mod_off[MAX_OBJ];   /* per-module TP offset */
static u32   tls_mod_id[MAX_OBJ];    /* per-module id */

/* ---------------- freestanding bits ---------------- */

void* memset(void* dst, int v, unsigned n) {
    u8* d = (u8*)dst;
    while (n--) *d++ = (u8)v;
    return dst;
}

void* memcpy(void* dst, const void* src, unsigned n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static int strcmp_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a - *b;
}

static int sc3(int nr, int a, int b, int c) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r)
                      : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void out(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    sc3(4, 1, (int)s, (int)n);
}

static void sys_exit(int code) {
    sc3(1, code, 0, 0);
    for (;;) { }
}

static void fail(const char* what, const char* detail) {
    out("[ld-mu] ");
    out(what);
    if (detail) { out(": "); out(detail); }
    out("\n");
    sys_exit(127);
}

/* ---------------- file input ---------------- */

static int read_file(const char* path, u32* size_out) {
    int fd = sc3(5, (int)path, 0, 0);
    if (fd < 0) return -1;
    int total = 0;
    while (total < (int)FILE_MAX) {
        int n = sc3(3, fd, (int)(fbuf + total), (int)(FILE_MAX - total));
        if (n <= 0) break;
        total += n;
    }
    sc3(6, fd, 0, 0);
    if (total <= 0) return -1;
    *size_out = (u32)total;
    return 0;
}

/* Join "/lib/" and a DT_NEEDED name into buf; return buf. */
static const char* lib_path(const char* name, char* buf, u32 cap) {
    u32 p = 0;
    const char* pre = "/lib/";
    while (*pre && p + 1 < cap) buf[p++] = *pre++;
    while (*name && p + 1 < cap) buf[p++] = *name++;
    buf[p] = '\0';
    return buf;
}

/* ---------------- dynamic section ---------------- */

static void parse_dynamic(obj_t* o, u32 phdr_addr, u32 phnum) {
    const phdr_t* ph = (const phdr_t*)phdr_addr;
    for (u32 i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC && ph[i].p_memsz >= sizeof(dyn_t))
            o->dyn = o->bias + ph[i].p_vaddr;
        if (ph[i].p_type == PT_TLS) {
            o->tls_present = 1;
            o->tls_vaddr   = ph[i].p_vaddr;
            o->tls_filesz  = ph[i].p_filesz;
            o->tls_memsz   = ph[i].p_memsz;
            o->tls_align   = ph[i].p_align;
        }
    }
    if (!o->dyn) return;

    const dyn_t* d = (const dyn_t*)o->dyn;
    for (u32 i = 0; i < 128; i++) {
        if (d[i].d_tag == DT_NULL) break;
        switch (d[i].d_tag) {
            case DT_SYMTAB:   o->symtab = d[i].d_val; break;
            case DT_STRTAB:   o->strtab = d[i].d_val; break;
            case DT_HASH:     o->hash   = d[i].d_val; break;
            case DT_GNU_HASH: o->gnu_hash = d[i].d_val; break;
            case DT_SYMENT:   o->syment = d[i].d_val; break;
            case DT_REL:      o->rel    = d[i].d_val; break;
            case DT_RELSZ:    o->relsz  = d[i].d_val; break;
            case DT_JMPREL:   o->jmprel = d[i].d_val; break;
            case DT_PLTRELSZ: o->pltrelsz = d[i].d_val; break;
            default: break;
        }
    }
    if (o->symtab) o->symtab += o->bias;
    if (o->strtab) o->strtab += o->bias;
    if (o->hash)   o->hash   += o->bias;
    if (o->gnu_hash) o->gnu_hash += o->bias;
    if (o->rel)    o->rel    += o->bias;
    if (o->jmprel) o->jmprel += o->bias;
}

/* ---------------- symbol lookup (SysV hash) ---------------- */

static u32 elf_hash(const char* s) {
    u32 h = 0, g;
    while (*s) {
        h = (h << 4) + (u32)(*s++);
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static u32 gnu_hash_(const char* s) {
    u32 h = 0x1505u;
    while (*s) h = (h << 5) + h + (u32)(*s++);
    return h;
}

static u32 sym_value(obj_t* o, const sym_t* sym) {
    if (sym->st_shndx == SHN_UNDEF) return 0;
    return (sym->st_shndx == SHN_ABS) ? sym->st_value : o->bias + sym->st_value;
}

/* .gnu.hash: nbuckets, symoffset, bloom_size, bloom_shift, bloom[],
 * buckets[], chain[]. Names in the hash table are not sorted. */
static u32 lookup_gnu(obj_t* o, const char* name) {
    const u32* gh = (const u32*)o->gnu_hash;
    u32 nbucket = gh[0], symoff = gh[1], bloom_size = gh[2];
    u32 bloom_shift = gh[3];
    const u32* bloom = gh + 4;
    const u32* buckets = bloom + bloom_size;
    const u32* chain = buckets + nbucket;
    if (nbucket == 0 || bloom_size == 0) return 0;

    u32 h = gnu_hash_(name);
    u32 w = bloom[(h >> 5) % bloom_size];
    u32 mask = (1u << (h & 31)) | (1u << ((h >> bloom_shift) & 31));
    if ((w & mask) != mask) return 0;

    u32 idx = buckets[h % nbucket];
    if (idx < symoff) return 0;
    for (u32 guard = 0; guard < 4096; guard++, idx++) {
        const sym_t* sym = (const sym_t*)(o->symtab + idx * o->syment);
        u32 c = chain[idx - symoff];
        if ((c & ~1u) == h &&
            sym->st_shndx != SHN_UNDEF &&
            strcmp_((const char*)(o->strtab + sym->st_name), name) == 0)
            return sym_value(o, sym);
        if (c & 1) break;
    }
    return 0;
}

static u32 lookup_in(obj_t* o, const char* name) {
    if (o->gnu_hash) return lookup_gnu(o, name);
    if (!o->hash || !o->symtab || !o->strtab || !o->syment) return 0;
    const u32* ht = (const u32*)o->hash;
    u32 nbucket = ht[0], nchain = ht[1];
    const u32* bucket = ht + 2;
    const u32* chain = bucket + nbucket;
    u32 idx = bucket[elf_hash(name) % nbucket];
    for (u32 guard = 0; idx != 0 && idx < nchain && guard < nchain; guard++) {
        const sym_t* sym = (const sym_t*)(o->symtab + idx * o->syment);
        if (sym->st_shndx != SHN_UNDEF &&
            strcmp_((const char*)(o->strtab + sym->st_name), name) == 0) {
            return sym_value(o, sym);
        }
        idx = chain[idx];
    }
    return 0;
}

/* Symbol-table variants of the lookups above, needed by TLS relocations:
 * they must know which module defines the symbol. */
static const sym_t* find_gnu(obj_t* o, const char* name) {
    const u32* gh = (const u32*)o->gnu_hash;
    u32 nbucket = gh[0], symoff = gh[1], bloom_size = gh[2];
    u32 bloom_shift = gh[3];
    const u32* bloom = gh + 4;
    const u32* buckets = bloom + bloom_size;
    const u32* chain = buckets + nbucket;
    if (nbucket == 0 || bloom_size == 0) return 0;

    u32 h = gnu_hash_(name);
    u32 w = bloom[(h >> 5) % bloom_size];
    u32 mask = (1u << (h & 31)) | (1u << ((h >> bloom_shift) & 31));
    if ((w & mask) != mask) return 0;

    u32 idx = buckets[h % nbucket];
    if (idx < symoff) return 0;
    for (u32 guard = 0; guard < 4096; guard++, idx++) {
        const sym_t* sym = (const sym_t*)(o->symtab + idx * o->syment);
        u32 c = chain[idx - symoff];
        if ((c & ~1u) == h &&
            sym->st_shndx != SHN_UNDEF &&
            strcmp_((const char*)(o->strtab + sym->st_name), name) == 0)
            return sym;
        if (c & 1) break;
    }
    return 0;
}

static const sym_t* find_in(obj_t* o, const char* name) {
    if (o->gnu_hash) return find_gnu(o, name);
    if (!o->hash || !o->symtab || !o->strtab || !o->syment) return 0;
    const u32* ht = (const u32*)o->hash;
    u32 nbucket = ht[0], nchain = ht[1];
    const u32* bucket = ht + 2;
    const u32* chain = bucket + nbucket;
    u32 idx = bucket[elf_hash(name) % nbucket];
    for (u32 guard = 0; idx != 0 && idx < nchain && guard < nchain; guard++) {
        const sym_t* sym = (const sym_t*)(o->symtab + idx * o->syment);
        if (sym->st_shndx != SHN_UNDEF &&
            strcmp_((const char*)(o->strtab + sym->st_name), name) == 0)
            return sym;
        idx = chain[idx];
    }
    return 0;
}

static int find_global(const char* name, obj_t** mo, const sym_t** ms) {
    for (int i = 0; i < nobj; i++) {
        const sym_t* s = find_in(&objs[i], name);
        if (s) { *mo = &objs[i]; *ms = s; return 1; }
    }
    return 0;
}

/* __tls_get_addr for the general-dynamic model: our static TLS blocks are
 * already placed relative to TP, so the lookup is a table walk. The i386
 * ABI passes the (module, offset) pair address in %eax, hence the small
 * assembly trampoline below. */
__attribute__((visibility("hidden")))
u32 ldmu_tls_get_addr_c(u32 mod, u32 off) {
    for (int i = 0; i < nobj; i++) {
        if (tls_mod_id[i] == mod) return tls_tp + tls_mod_off[i] + off;
    }
    return tls_tp + off;
}

__asm__(
    ".text\n"
    ".global ldmu_tls_get_addr_entry\n"
    ".hidden ldmu_tls_get_addr_entry\n"
    "ldmu_tls_get_addr_entry:\n"
    "    movl 4(%eax), %edx\n"
    "    pushl %edx\n"
    "    pushl (%eax)\n"
    "    call ldmu_tls_get_addr_c\n"
    "    addl $8, %esp\n"
    "    ret\n"
);
void ldmu_tls_get_addr_entry(void);

static u32 lookup_global(const char* name) {
    for (int i = 0; i < nobj; i++) {
        u32 v = lookup_in(&objs[i], name);
        if (v) return v;
    }
    if (strcmp_(name, "__tls_get_addr") == 0 ||
        strcmp_(name, "___tls_get_addr") == 0)
        return (u32)(unsigned long)&ldmu_tls_get_addr_entry;
    return 0;
}

/* ---------------- relocation ---------------- */

/* Resolve a TLS symbol to its module and offsets. Symbol index 0 means
 * "the object itself, block offset 0" (GCC emits this for module-local
 * TLS with an implicit addend). */
static void tls_sym(obj_t* o, u32 symix, int* moff, u32* symoff, u32* modid) {
    obj_t* m = o;
    const sym_t* s = (const sym_t*)(o->symtab + symix * o->syment);
    if (symix != 0 && s->st_shndx == SHN_UNDEF) {
        const char* name = (const char*)(o->strtab + s->st_name);
        if (!find_global(name, &m, &s)) fail("unresolved TLS symbol", name);
    }
    *moff   = m->tls_off;
    *symoff = s->st_value;
    *modid  = m->tls_id;
}

static int apply_one(obj_t* o, u32 type, u32 symix, u32 where) {
    u32* dst = (u32*)where;
    u32 val;
    switch (type) {
        case R_386_NONE:
            return 0;
        case R_386_RELATIVE:
            *dst += o->bias;
            return 0;
        case R_386_COPY: {
            /* Executable copies a data object defined in a shared library
             * (classic COPY relocation). */
            const sym_t* sym = (const sym_t*)(o->symtab + symix * o->syment);
            const char* name = (const char*)(o->strtab + sym->st_name);
            u32 src = lookup_global(name);
            if (!src) fail("unresolved COPY symbol", name);
            memcpy((void*)where, (const void*)src, sym->st_size);
            return 0;
        }
        case R_386_IRELATIVE: {
            /* IFUNC: the field holds the resolver address (bias + addend);
             * call it now and store the resolved implementation address. */
            u32 (*resolver)(void) = (u32 (*)(void))(*dst + o->bias);
            *dst = resolver();
            return 0;
        }
        case R_386_32:
        case R_386_PC32:
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT: {
            const sym_t* sym = (const sym_t*)(o->symtab + symix * o->syment);
            const char* name = (const char*)(o->strtab + sym->st_name);
            if (sym->st_shndx == SHN_UNDEF) {
                val = lookup_global(name);
                if (!val) fail("unresolved symbol", name);
            } else {
                val = (sym->st_shndx == SHN_ABS) ? sym->st_value
                                                 : o->bias + sym->st_value;
            }
            if (type == R_386_32)        *dst += val;
            else if (type == R_386_PC32) *dst += val - where;
            else                         *dst = val;
            return 0;
        }
        case R_386_TLS_TPOFF:
        case R_386_TLS_IE:
        case R_386_TLS_GOTIE:
        case R_386_TLS_TPOFF32:
        case R_386_TLS_IE_32: {
            /* The GOT/word addend already holds the symbol's offset inside
             * the module's TLS block; only the module base (relative to
             * TP) is missing (same rule as glibc's i386 ld.so). */
            int moff = 0; u32 symoff = 0, modid = 0;
            tls_sym(o, symix, &moff, &symoff, &modid);
            *dst += (u32)(unsigned)moff;
            return 0;
        }
        case R_386_TLS_LE:
        case R_386_TLS_LE_32: {
            /* Local-exec: no dynamic relocation is normally emitted for
             * these, but resolve fully if one shows up. */
            int moff = 0; u32 symoff = 0, modid = 0;
            tls_sym(o, symix, &moff, &symoff, &modid);
            *dst += (u32)(unsigned)(moff + (int)symoff);
            return 0;
        }
        case R_386_TLS_DTPMOD32: {
            int moff = 0; u32 symoff = 0, modid = 0;
            tls_sym(o, symix, &moff, &symoff, &modid);
            *dst = modid;
            return 0;
        }
        case R_386_TLS_DTPOFF32: {
            int moff = 0; u32 symoff = 0, modid = 0;
            tls_sym(o, symix, &moff, &symoff, &modid);
            *dst += symoff;
            return 0;
        }
        default:
            return -1;
    }
}

static void apply_table(obj_t* o, u32 addr, u32 size) {
    if (!addr || !size) return;
    const rel_t* r = (const rel_t*)addr;
    u32 n = size / sizeof(rel_t);
    for (u32 i = 0; i < n; i++) {
        if (apply_one(o, r[i].r_info & 0xFF, r[i].r_info >> 8,
                      o->bias + r[i].r_offset) != 0)
            fail("unsupported relocation in", o->name);
    }
}

static void apply_relocs(obj_t* o) {
    apply_table(o, o->rel, o->relsz);
    apply_table(o, o->jmprel, o->pltrelsz);
}

/* ---------------- static TLS ---------------- */

/* i386 variant II layout: TP points just above the static TLS blocks and
 * module areas are carved downwards. The main executable always comes
 * first so its TP-relative offsets (resolved at static link time, e.g.
 * `addl $5, -4(%tp)`) stay valid. */
static void tls_setup(void) {
    int off = 0;
    int any = 0;

    for (int i = 0; i < nobj; i++) {
        objs[i].tls_id  = (u32)(i + 1);
        objs[i].tls_off = 0;
        tls_mod_id[i]   = objs[i].tls_id;
        tls_mod_off[i]  = 0;
    }

    for (int i = 0; i < nobj; i++) {
        obj_t* o = &objs[i];
        if (!o->tls_present) continue;
        any = 1;
        u32 align = o->tls_align ? o->tls_align : 4;
        if (align < 4) align = 4;
        u32 memsz = (o->tls_memsz + align - 1) & ~(align - 1);
        if (memsz == 0) continue;

        off -= (int)memsz;
        o->tls_off = off;
        tls_mod_off[i] = (u32)off;

        u32 dst = tls_tp + (u32)off;
        if (o->tls_filesz)
            memcpy((void*)dst, (const void*)(o->bias + o->tls_vaddr),
                   o->tls_filesz);
        if (memsz > o->tls_filesz)
            memset((void*)(dst + o->tls_filesz), 0, memsz - o->tls_filesz);
    }

    if (!any) return;

    *(u32*)tls_tp = tls_tp;    /* gs:0 holds the thread pointer itself */

    /* Selector 6 (0x33 at Ring3) is the user TLS descriptor slot that
     * set_thread_area(243) fills in. */
    struct { u32 entry, base, limit, flags; } d;
    d.entry = 0xFFFFFFFFu;
    d.base  = tls_tp;
    d.limit = 0xFFFFFu;
    d.flags = 0x51u;           /* seg_32bit | limit_in_pages | useable */
    if (sc3(243, (int)&d, 0, 0) < 0) fail("set_thread_area failed", 0);
    __asm__ volatile ("movw $0x33, %%ax\n\tmovw %%ax, %%gs" ::: "eax", "memory");
}

/* ---------------- object loading ---------------- */

static int map_from_file(const char* path, const char* name, u32 base) {
    if (nobj >= MAX_OBJ) fail("too many shared objects", path);
    u32 size = 0;
    if (read_file(path, &size) != 0) fail("cannot open", path);

    const ehdr_t* eh = (const ehdr_t*)fbuf;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') fail("not ELF", path);
    if (eh->e_type != 3) fail("not ET_DYN", path);
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(phdr_t)) fail("bad phdrs", path);
    u32 phoff = eh->e_phoff;
    if (phoff > size || (u32)eh->e_phnum * eh->e_phentsize > size - phoff)
        fail("truncated phdrs", path);
    const phdr_t* ph = (const phdr_t*)(fbuf + phoff);

    u32 minv = 0xFFFFFFFFu;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < minv) minv = ph[i].p_vaddr;
    }
    if (minv == 0xFFFFFFFFu) fail("no LOAD segment", path);

    obj_t* o = &objs[nobj];
    memset(o, 0, sizeof(*o));
    o->bias = (base >= minv) ? (base - minv) : 0;
    objnames[nobj][0] = '\0';
    {
        u32 p = 0;
        while (name[p] && p < 63) { objnames[nobj][p] = name[p]; p++; }
        objnames[nobj][p] = '\0';
    }
    o->name = objnames[nobj];

    u32 max_end = base;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset)
            fail("truncated segment", path);
        u32 va = o->bias + ph[i].p_vaddr;
        memcpy((void*)va, fbuf + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset((void*)(va + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
        if (va + ph[i].p_memsz > max_end) max_end = va + ph[i].p_memsz;
    }
    next_base = (max_end + 0xFFFu) & ~0xFFFu;

    u32 phdr_addr = o->bias + phoff;
    parse_dynamic(o, phdr_addr, eh->e_phnum);
    nobj++;
    return 0;
}

/* Recursively load DT_NEEDED libraries of an object (depth-limited). */
static void load_needed(obj_t* o, int depth) {
    if (!o->dyn || !o->strtab) return;
    const dyn_t* d = (const dyn_t*)o->dyn;
    for (u32 i = 0; i < 128 && d[i].d_tag != DT_NULL; i++) {
        if (d[i].d_tag != DT_NEEDED) continue;
        const char* name = (const char*)(o->strtab + d[i].d_val);
        int already = 0;
        for (int k = 0; k < nobj; k++) {
            if (strcmp_(objnames[k], name) == 0) already = 1;
        }
        if (already) continue;
        if (depth > 2) fail("dependency loop", name);
        char path[80];
        map_from_file(lib_path(name, path, sizeof(path)), name, next_base);
        load_needed(&objs[nobj - 1], depth + 1);
    }
}

/* ---------------- entry ---------------- */

static void jump_to(u32 entry, u32* sp) __attribute__((noreturn));
static void jump_to(u32 entry, u32* sp) {
    __asm__ volatile ("mov %0, %%esp\n\txor %%ebp, %%ebp\n\tjmp *%1"
                      :: "r"(sp), "r"(entry) : "memory");
    __builtin_unreachable();
}

void ld_main(u32* sp) __attribute__((noreturn, visibility("hidden")));

void ld_main(u32* sp) {
    u32 argc = sp[0];
    u32* p = sp + 1 + argc + 1;         /* skip argv + NULL */
    while (*p) p++;                     /* skip envp */
    p++;
    u32 at_phdr = 0, at_phnum = 0, at_phent = 0, at_entry = 0;
    for (;; p += 2) {
        if (p[0] == AT_NULL) break;
        if (p[0] == AT_PHDR)  at_phdr  = p[1];
        if (p[0] == AT_PHENT) at_phent = p[1];
        if (p[0] == AT_PHNUM) at_phnum = p[1];
        if (p[0] == AT_ENTRY) at_entry = p[1];
    }
    (void)at_phent;
    if (!at_phdr || !at_phnum) fail("bad auxv", 0);

    /* ---- main program: derive its load bias from AT_PHDR ---- */
    obj_t* m = &objs[0];
    memset(m, 0, sizeof(*m));
    objnames[0][0] = '<'; objnames[0][1] = '\0';
    m->name = objnames[0];
    u32 minv = 0xFFFFFFFFu;
    const phdr_t* mp = (const phdr_t*)at_phdr;
    for (u32 i = 0; i < at_phnum; i++) {
        if (mp[i].p_type == PT_LOAD && mp[i].p_vaddr < minv) minv = mp[i].p_vaddr;
    }
    if (minv == 0xFFFFFFFFu) fail("main has no LOAD segment", 0);
    m->bias = PROG_BASE - minv;
    parse_dynamic(m, at_phdr, at_phnum);
    nobj = 1;

    /* ---- DT_NEEDED shared libraries, static TLS, then relocations ---- */
    load_needed(m, 0);
    tls_setup();
    for (int i = 1; i < nobj; i++) apply_relocs(&objs[i]);
    apply_relocs(m);

    jump_to(at_entry, sp);
}
