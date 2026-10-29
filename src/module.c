#include "module.h"
#include "string.h"

/* Multiboot1 module entry (16 bytes) */
typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

typedef struct {
    uint32_t start;
    uint32_t end;
    char     name[64];
} module_t;

static module_t modules[MODULE_MAX];
static uint32_t module_count = 0;

void modules_init(const multiboot_info_t* mb) {
    module_count = 0;
    if (!mb || !(mb->flags & MULTIBOOT_FLAG_MODS) ||
        mb->mods_count == 0 || mb->mods_addr == 0) {
        return;
    }

    uint32_t n = mb->mods_count;
    if (n > MODULE_MAX) n = MODULE_MAX;

    const multiboot_module_t* raw = (const multiboot_module_t*)mb->mods_addr;
    for (uint32_t i = 0; i < n; i++) {
        if (raw[i].mod_end <= raw[i].mod_start) continue;

        module_t* m = &modules[module_count];
        m->start = raw[i].mod_start;
        m->end   = raw[i].mod_end;
        m->name[0] = '\0';
        if (raw[i].string) {
            /* The loader's string lives in module/table RAM which the
             * allocator may later overwrite; keep our own copy. */
            strncpy(m->name, (const char*)raw[i].string, sizeof(m->name) - 1);
            m->name[sizeof(m->name) - 1] = '\0';
        }
        module_count++;
    }
}

uint32_t modules_count(void) {
    return module_count;
}

bool_t modules_get(uint32_t index, uint32_t* start, uint32_t* end) {
    if (index >= module_count) return false;
    if (start) *start = modules[index].start;
    if (end)   *end   = modules[index].end;
    return true;
}

const char* modules_name(uint32_t index) {
    if (index >= module_count) return NULL;
    return modules[index].name[0] ? modules[index].name : NULL;
}
