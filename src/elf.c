#include "elf.h"
#include "string.h"

/* Loader errors (internal; surfaced as a single message by user.c) */
#define ELF_OK          0
#define ELF_ERR_HEADER  (-1)
#define ELF_ERR_FORMAT  (-2)
#define ELF_ERR_RANGE   (-3)
#define ELF_ERR_NOLOAD  (-4)
#define ELF_ERR_RELOC   (-5)

static bool_t is_power_of_two(uint32_t v) {
    return v && !(v & (v - 1));
}

static bool_t range_ok(uint32_t va, uint32_t len, uint32_t base, uint32_t limit) {
    if (va < base || va > limit) return false;
    return len <= limit - va;
}

/* Validate the ELF header and program header table. */
static int elf_parse(const void* image, uint32_t image_size, elf32_ehdr_t* eh) {
    if (!image || image_size < sizeof(elf32_ehdr_t)) return ELF_ERR_HEADER;

    /* The blob is embedded at arbitrary alignment, so copy the header
     * instead of casting the pointer (strict-alignment safety). */
    memcpy(eh, image, sizeof(*eh));

    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0) return ELF_ERR_HEADER;
    if (eh->e_ident[4] != ELF_CLASS_32)              return ELF_ERR_FORMAT;
    if (eh->e_ident[5] != ELF_DATA_LSB)              return ELF_ERR_FORMAT;
    if (eh->e_ident[6] != ELF_VERSION_CUR)           return ELF_ERR_FORMAT;
    if (eh->e_type != ELF_TYPE_EXEC && eh->e_type != ELF_TYPE_DYN)
                                                     return ELF_ERR_FORMAT;
    if (eh->e_machine != ELF_MACHINE_386)            return ELF_ERR_FORMAT;
    if (eh->e_ehsize < sizeof(elf32_ehdr_t))         return ELF_ERR_FORMAT;
    if (eh->e_phentsize < sizeof(elf32_phdr_t) ||
        eh->e_phentsize == 0 || eh->e_phnum == 0 ||
        eh->e_phnum > 128)                           return ELF_ERR_FORMAT;
    if (eh->e_phoff > image_size)                    return ELF_ERR_FORMAT;
    uint32_t table_bytes = (uint32_t)eh->e_phnum * eh->e_phentsize;
    if (table_bytes > image_size - eh->e_phoff)      return ELF_ERR_FORMAT;
    return ELF_OK;
}

static const elf32_phdr_t* elf_phdr(const void* image,
                                    const elf32_ehdr_t* eh, uint16_t i) {
    return (const elf32_phdr_t*)((const uint8_t*)image + eh->e_phoff +
                                 (uint32_t)i * eh->e_phentsize);
}

/* Bias for ET_DYN: place the lowest segment at load_base. */
static uint32_t elf_load_bias(const void* image, const elf32_ehdr_t* eh,
                              uint32_t load_base) {
    if (eh->e_type != ELF_TYPE_DYN) return 0;
    uint32_t min_va = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, elf_phdr(image, eh, i), sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) continue;
        if (ph.p_vaddr < min_va) min_va = ph.p_vaddr;
    }
    if (min_va == 0xFFFFFFFFu || min_va > load_base) return 0;
    return load_base - min_va;
}

/* Apply one table of i386 Elf32_Rel relocations against the loaded image. */
static int elf_apply_rel_array(uint32_t rel_va, uint32_t relsz,
                               uint32_t symtab, uint32_t syment,
                               uint32_t bias, uint32_t base, uint32_t limit,
                               uint32_t* count) {
    if (relsz == 0) return ELF_OK;
    if (relsz > 256u * 1024u) return ELF_ERR_RELOC;
    if ((relsz & 7u) != 0) return ELF_ERR_RELOC;
    if (!range_ok(rel_va, relsz, base, limit)) return ELF_ERR_RANGE;

    uint32_t n = relsz / sizeof(elf32_rel_t);
    for (uint32_t i = 0; i < n; i++) {
        const elf32_rel_t* r = (const elf32_rel_t*)(rel_va + i * 8u);
        uint32_t type  = r->r_info & 0xFFu;
        uint32_t symix = r->r_info >> 8;
        uint32_t where = r->r_offset + bias;
        if (where < base || where > limit - 4) return ELF_ERR_RANGE;
        uint32_t* dst = (uint32_t*)where;

        switch (type) {
            case R_386_NONE:
                break;
            case R_386_RELATIVE:
                *dst += bias;
                break;
            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT:
            case R_386_32: {
                if (!symtab || syment < sizeof(elf32_sym_t))
                    return ELF_ERR_RELOC;
                if (symix > 0xFFFFFu) return ELF_ERR_RELOC;
                uint32_t sym_va = symtab + symix * syment;
                if (!range_ok(sym_va, sizeof(elf32_sym_t), base, limit))
                    return ELF_ERR_RELOC;
                const elf32_sym_t* sym = (const elf32_sym_t*)sym_va;
                if (sym->st_shndx == SHN_UNDEF)
                    return ELF_ERR_RELOC;   /* needs a shared library */
                uint32_t val = (sym->st_shndx == SHN_ABS)
                               ? sym->st_value : bias + sym->st_value;
                if (type == R_386_32) *dst += val;
                else                  *dst  = val;
                break;
            }
            default:
                return ELF_ERR_RELOC;      /* COPY/IRELATIVE/TLS unsupported */
        }
        if (count) (*count)++;
    }
    return ELF_OK;
}

/* Walk PT_DYNAMIC and apply .rel.dyn / .rel.plt relocations. */
static int elf_apply_dynamic(uint32_t dyn_va, uint32_t bias,
                             uint32_t base, uint32_t limit,
                             uint32_t* count) {
    if (!range_ok(dyn_va, sizeof(elf32_dyn_t), base, limit))
        return ELF_ERR_RELOC;

    const elf32_dyn_t* d = (const elf32_dyn_t*)dyn_va;
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    uint32_t symtab = 0, syment = 0, pltrel = DT_REL;

    for (uint32_t i = 0; i < 256; i++) {
        if (!range_ok((uint32_t)d, sizeof(elf32_dyn_t), base, limit))
            return ELF_ERR_RELOC;
        if (d->d_tag == DT_NULL) break;
        switch (d->d_tag) {
            case DT_REL:     rel = d->d_val; break;
            case DT_RELSZ:   relsz = d->d_val; break;
            case DT_JMPREL:  jmprel = d->d_val; break;
            case DT_PLTRELSZ: pltrelsz = d->d_val; break;
            case DT_PLTREL:  pltrel = d->d_val; break;
            case DT_SYMTAB:  symtab = d->d_val; break;
            case DT_SYMENT:  syment = d->d_val; break;
            default: break;
        }
        d++;
    }

    if (symtab) symtab += bias;
    int rc;
    if (rel && relsz) {
        rc = elf_apply_rel_array(rel + bias, relsz, symtab, syment,
                                 bias, base, limit, count);
        if (rc != ELF_OK) return rc;
    }
    if (jmprel && pltrelsz) {
        if (pltrel != DT_REL) return ELF_ERR_RELOC;
        rc = elf_apply_rel_array(jmprel + bias, pltrelsz, symtab, syment,
                                 bias, base, limit, count);
        if (rc != ELF_OK) return rc;
    }
    return ELF_OK;
}

/* Parse + copy every PT_LOAD segment into [load_base, load_limit) and fill
 * the loader info. Dynamic relocations are NOT applied here; the caller
 * decides whether it or a user-mode interpreter will do it. The PT_DYNAMIC
 * address is returned through dyn_va_out (0 = none). */
static int elf_map_segments(const void* image, uint32_t image_size,
                            uint32_t load_base, uint32_t load_limit,
                            elf32_load_info_t* info, uint32_t* dyn_va_out) {
    elf32_ehdr_t eh;
    int rc = elf_parse(image, image_size, &eh);
    if (rc != ELF_OK) return rc;

    uint32_t bias = elf_load_bias(image, &eh, load_base);
    uint32_t max_end = load_base;
    uint32_t loads = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, elf_phdr(image, &eh, i), sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) continue;

        if (ph.p_filesz > ph.p_memsz)                 return ELF_ERR_FORMAT;
        if (ph.p_offset > image_size ||
            ph.p_filesz > image_size - ph.p_offset)   return ELF_ERR_RANGE;

        uint32_t va = ph.p_vaddr + bias;
        if (!range_ok(va, ph.p_memsz, load_base, load_limit))
            return ELF_ERR_RANGE;
        if (ph.p_align && !is_power_of_two(ph.p_align)) return ELF_ERR_FORMAT;

        if (ph.p_filesz)
            memcpy((void*)va, (const uint8_t*)image + ph.p_offset, ph.p_filesz);
        if (ph.p_memsz > ph.p_filesz) {
            memset((void*)(va + ph.p_filesz), 0, ph.p_memsz - ph.p_filesz);
        }
        uint32_t seg_end = va + ph.p_memsz;
        if (seg_end > max_end) max_end = seg_end;
        loads++;
    }
    if (loads == 0) return ELF_ERR_NOLOAD;

    uint32_t entry = eh.e_entry + bias;
    if (entry < load_base || entry >= load_limit) return ELF_ERR_RANGE;

    elf32_load_info_t local;
    if (!info) info = &local;
    info->entry = entry;
    info->image_end = max_end;
    info->base = bias;
    info->is_dyn = (eh.e_type == ELF_TYPE_DYN) ? true : false;
    info->has_interp = false;
    info->has_dynamic = false;
    info->interp[0] = '\0';
    info->reloc_count = 0;
    info->phent = sizeof(elf32_phdr_t);
    info->phnum = eh.e_phnum;
    info->phdr = 0;

    uint32_t dyn_va = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, elf_phdr(image, &eh, i), sizeof(ph));

        if (ph.p_type == ELF_PT_LOAD &&
            ph.p_offset <= eh.e_phoff &&
            eh.e_phoff < ph.p_offset + ph.p_filesz) {
            info->phdr = ph.p_vaddr + bias + (eh.e_phoff - ph.p_offset);
        }
        if (ph.p_type == ELF_PT_INTERP && ph.p_filesz > 0) {
            if (ph.p_offset <= image_size &&
                ph.p_filesz <= image_size - ph.p_offset && ph.p_filesz < 64) {
                memcpy(info->interp, (const uint8_t*)image + ph.p_offset,
                       ph.p_filesz);
                info->interp[ph.p_filesz] = '\0';
                info->has_interp = true;
            }
        }
        if (ph.p_type == ELF_PT_DYNAMIC && ph.p_memsz >= sizeof(elf32_dyn_t)) {
            dyn_va = ph.p_vaddr + bias;
            info->has_dynamic = true;
        }
    }

    if (dyn_va_out) *dyn_va_out = dyn_va;
    return ELF_OK;
}

int elf32_load_ex(const void* image, uint32_t image_size,
                  uint32_t load_base, uint32_t load_limit,
                  elf32_load_info_t* info) {
    uint32_t dyn_va = 0;
    int rc = elf_map_segments(image, image_size, load_base, load_limit,
                              info, &dyn_va);
    if (rc != ELF_OK) return rc;
    if (dyn_va) {
        rc = elf_apply_dynamic(dyn_va, info->base, load_base, load_limit,
                               &info->reloc_count);
        if (rc != ELF_OK) return rc;
    }
    return ELF_OK;
}

/* Load a program for execve. When it names a PT_INTERP, the kernel maps the
 * segments only: the interpreter (user-mode ld.so) applies the relocations,
 * including the symbol slots of DT_NEEDED shared libraries. Self-contained
 * images get their relocations applied here as before. */
int elf32_load_program(const void* image, uint32_t image_size,
                       uint32_t load_base, uint32_t load_limit,
                       elf32_load_info_t* info) {
    uint32_t dyn_va = 0;
    int rc = elf_map_segments(image, image_size, load_base, load_limit,
                              info, &dyn_va);
    if (rc != ELF_OK) return rc;
    if (info->has_interp) return ELF_OK;
    if (dyn_va) {
        rc = elf_apply_dynamic(dyn_va, info->base, load_base, load_limit,
                               &info->reloc_count);
        if (rc != ELF_OK) return rc;
    }
    return ELF_OK;
}

int elf32_load(const void* image, uint32_t image_size,
               uint32_t load_base, uint32_t load_limit,
               uint32_t* entry_out, uint32_t* end_out) {
    elf32_load_info_t info;
    int rc = elf32_load_ex(image, image_size, load_base, load_limit, &info);
    if (rc != ELF_OK) return rc;
    if (entry_out) *entry_out = info.entry;
    if (end_out)   *end_out = info.image_end;
    return ELF_OK;
}

int elf32_validate(const void* image, uint32_t image_size,
                   uint32_t load_base, uint32_t load_limit,
                   uint32_t* entry_out, uint32_t* end_out) {
    elf32_ehdr_t eh;
    int rc = elf_parse(image, image_size, &eh);
    if (rc != ELF_OK) return rc;

    uint32_t bias = elf_load_bias(image, &eh, load_base);
    uint32_t max_end = load_base;
    uint32_t loads = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, elf_phdr(image, &eh, i), sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) continue;
        if (ph.p_filesz > ph.p_memsz)                 return ELF_ERR_FORMAT;
        if (ph.p_offset > image_size ||
            ph.p_filesz > image_size - ph.p_offset)   return ELF_ERR_RANGE;
        uint32_t va = ph.p_vaddr + bias;
        if (!range_ok(va, ph.p_memsz, load_base, load_limit))
            return ELF_ERR_RANGE;
        if (ph.p_align && !is_power_of_two(ph.p_align)) return ELF_ERR_FORMAT;
        uint32_t seg_end = va + ph.p_memsz;
        if (seg_end > max_end) max_end = seg_end;
        loads++;
    }
    if (loads == 0) return ELF_ERR_NOLOAD;
    uint32_t entry = eh.e_entry + bias;
    if (entry < load_base || entry >= load_limit) return ELF_ERR_RANGE;

    if (entry_out) *entry_out = entry;
    if (end_out)   *end_out = max_end;
    return ELF_OK;
}
