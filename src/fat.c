/* fat.c - read-only FAT32 reader on top of the ATA PIO block device.
 *
 * Supports one FAT32 volume, either as an MBR/GPT partition or as a bare
 * volume starting at LBA 0. Directory lookup understands 8.3 names and
 * VFAT long-file-name entries; files are read by walking the cluster chain.
 */
#include "fat.h"
#include "part.h"
#include "ata.h"
#include "string.h"
#include "serial.h"

#define SECTOR 512u
#define DIRBUF_MAX 16384u        /* largest directory cluster we accept */

typedef struct {
    uint64_t base;               /* partition start, in bytes */
    uint32_t spc;                /* sectors per cluster */
    uint32_t reserved;
    uint32_t nfats;
    uint32_t fsz;                /* sectors per FAT */
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t fat_start;          /* absolute LBA of FAT #0 */
    uint32_t data_start;         /* absolute LBA of cluster 2 */
    uint32_t cluster_count;
    char     label[12];
} fat_vol_t;

static fat_vol_t vol;
static bool_t    present;
static uint8_t   dirbuf[DIRBUF_MAX] __attribute__((aligned(4)));

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool_t rd(uint64_t byte_off, void* buf, uint32_t n) {
    if (byte_off > 0xFFFFFFFFu) return false;
    if (n > 0xFFFFFFFFu - (uint32_t)byte_off) return false;
    return ata_read_bytes((uint32_t)byte_off, buf, n) == (int)n;
}

static uint32_t fat_next(uint32_t cluster) {
    uint32_t off = cluster * 4;
    uint32_t sec = vol.fat_start + off / SECTOR;
    uint8_t b[SECTOR];
    if (!rd((uint64_t)sec * SECTOR, b, SECTOR)) return 0x0FFFFFFFu;
    return le32(b + (off % SECTOR)) & 0x0FFFFFFFu;
}

static bool_t read_cluster(uint32_t cluster, uint8_t* buf) {
    uint32_t bytes = vol.spc * SECTOR;
    if (bytes > DIRBUF_MAX) return false;
    uint64_t off = (uint64_t)(vol.data_start + (cluster - 2) * vol.spc) * SECTOR;
    return rd(off, buf, bytes);
}

/* Build "NAME.EXT" from an 8.3 entry. */
static void short_name(const uint8_t* e, char* out) {
    int o = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = '\0';
}

static int ci_cmp(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return x - y;
        a++; b++;
    }
    return *a - *b;
}

typedef struct {
    uint32_t cluster;
    uint32_t size;
    uint8_t  attr;
} dirent_t;

/* Look up one path component inside the directory cluster chain. */
static bool_t lookup_in_dir(uint32_t dir_cluster, const char* name,
                            dirent_t* out) {
    for (uint32_t c = dir_cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u;
         c = fat_next(c)) {
        if (++guard > 4096) return false;
        if (!read_cluster(c, dirbuf)) return false;
        char lfn[260];
        lfn[0] = '\0';
        int have_lfn = 0;
        for (uint32_t off = 0; off + 32 <= vol.spc * SECTOR; off += 32) {
            const uint8_t* e = dirbuf + off;
            if (e[0] == 0x00) return false;          /* end of directory */
            if (e[0] == 0xE5) { have_lfn = 0; lfn[0] = '\0'; continue; }
            uint8_t attr = e[11];
            if ((attr & 0x3F) == 0x0F) {
                uint8_t seq = e[0] & 0x1F;
                if (seq == 0 || seq > 20) continue;
                int base = ((int)seq - 1) * 13;
                for (int i = 0; i < 13; i++) {
                    uint8_t ch;
                    if (i < 5)       ch = e[1 + i];
                    else if (i < 11) ch = e[14 + (i - 5)];
                    else             ch = e[28 + (i - 11)];
                    if (base + i < 259) lfn[base + i] = ch;
                }
                lfn[259] = '\0';
                have_lfn = 1;
                continue;
            }
            if (attr & 0x08) { have_lfn = 0; lfn[0] = '\0'; continue; }
            char sn[16];
            short_name(e, sn);
            int match = have_lfn ? (ci_cmp(lfn, name) == 0) : 0;
            if (!match) match = (ci_cmp(sn, name) == 0);
            if (match) {
                out->cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                out->size = le32(e + 28);
                out->attr = attr;
                return true;
            }
            have_lfn = 0;
            lfn[0] = '\0';
        }
    }
    return false;
}

/* Resolve an absolute path to its directory entry. */
static bool_t resolve(const char* path, dirent_t* out) {
    if (!present || !path) return false;
    uint32_t cluster = vol.root_cluster;
    char comp[64];
    const char* p = path;
    while (*p == '/') p++;

    if (!*p) {
        out->cluster = cluster;
        out->size = 0;
        out->attr = 0x10;
        return true;
    }

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/') p++;
        dirent_t d;
        if (!lookup_in_dir(cluster, comp, &d)) return false;
        if (*p) {
            if (!(d.attr & 0x10)) return false;
            cluster = d.cluster;
        } else {
            *out = d;
            return true;
        }
    }
    return false;
}

static int app(char* out, uint32_t cap, int* pos, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if ((uint32_t)*pos + 1 >= cap) { out[cap - 1] = '\0'; return -1; }
        out[(*pos)++] = s[i];
    }
    out[*pos] = '\0';
    return 0;
}

int fat_list(const char* path, char* out, uint32_t cap) {
    dirent_t d;
    if (!cap) return -1;
    out[0] = '\0';
    if (!resolve(path, &d) || !(d.attr & 0x10)) return -1;
    uint32_t cluster = d.cluster;
    int pos = 0;
    for (uint32_t c = cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u;
         c = fat_next(c)) {
        if (++guard > 4096) break;
        if (!read_cluster(c, dirbuf)) return -1;
        char lfn[260];
        lfn[0] = '\0';
        int have_lfn = 0;
        for (uint32_t off = 0; off + 32 <= vol.spc * SECTOR; off += 32) {
            const uint8_t* e = dirbuf + off;
            if (e[0] == 0x00) goto done;
            if (e[0] == 0xE5) { have_lfn = 0; lfn[0] = '\0'; continue; }
            uint8_t attr = e[11];
            if ((attr & 0x3F) == 0x0F) {
                uint8_t seq = e[0] & 0x1F;
                if (seq == 0 || seq > 20) continue;
                int base = ((int)seq - 1) * 13;
                for (int i = 0; i < 13; i++) {
                    uint8_t ch;
                    if (i < 5)       ch = e[1 + i];
                    else if (i < 11) ch = e[14 + (i - 5)];
                    else             ch = e[28 + (i - 11)];
                    if (base + i < 259) lfn[base + i] = ch;
                }
                lfn[259] = '\0';
                have_lfn = 1;
                continue;
            }
            if (attr & 0x08) { have_lfn = 0; lfn[0] = '\0'; continue; }

            const char* nm = have_lfn ? lfn : (const char*)0;
            char sn[16];
            short_name(e, sn);
            if (!nm) nm = sn;
            if (ci_cmp(nm, ".") != 0 && ci_cmp(nm, "..") != 0) {
                char line[96];
                int lp = 0;
                if (attr & 0x10) {
                    line[lp++] = 'D'; line[lp++] = ' ';
                    for (int i = 0; nm[i] && lp < 88; i++) line[lp++] = nm[i];
                } else {
                    line[lp++] = 'F'; line[lp++] = ' ';
                    u32_to_dec(line + lp, le32(e + 28));
                    while (line[lp]) lp++;
                    line[lp++] = ' ';
                    for (int i = 0; nm[i] && lp < 88; i++) line[lp++] = nm[i];
                }
                line[lp++] = '\n';
                if (app(out, cap, &pos, line, (uint32_t)lp) != 0) return -1;
            }
            have_lfn = 0;
            lfn[0] = '\0';
        }
    }
done:
    return pos;
}

int fat_read_file(const char* path, void* buf, uint32_t cap) {
    dirent_t d;
    if (!resolve(path, &d) || (d.attr & 0x10)) return -1;
    uint32_t want = d.size > cap ? cap : d.size;
    uint32_t done = 0;
    uint8_t* dst = (uint8_t*)buf;
    for (uint32_t c = d.cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u && done < want;
         c = fat_next(c)) {
        if (++guard > 4096) break;
        uint32_t bytes = vol.spc * SECTOR;
        if (bytes > DIRBUF_MAX) return -1;
        if (!read_cluster(c, dirbuf)) return -1;
        for (uint32_t i = 0; i < bytes && done < want; i++) dst[done++] = dirbuf[i];
    }
    return (int)done;
}

/* ---------------- write path (8.3 names, write-through) ---------------- */

static bool_t wr(uint64_t byte_off, const void* buf, uint32_t n) {
    if (byte_off > 0xFFFFFFFFu || n > 0xFFFFFFFFu - (uint32_t)byte_off)
        return false;
    return ata_write_bytes((uint32_t)byte_off, buf, n) == (int)n;
}

static bool_t set_fat(uint32_t cluster, uint32_t value) {
    uint32_t off = cluster * 4;
    for (uint32_t f = 0; f < vol.nfats; f++) {
        uint32_t sec = vol.fat_start + f * vol.fsz + off / SECTOR;
        uint8_t b[SECTOR];
        if (!rd((uint64_t)sec * SECTOR, b, SECTOR)) return false;
        memcpy(b + (off % SECTOR), &value, 4);
        if (!wr((uint64_t)sec * SECTOR, b, SECTOR)) return false;
    }
    return true;
}

static uint32_t alloc_hint = 2;

static bool_t alloc_cluster(uint32_t* out) {
    uint32_t last = 2 + vol.cluster_count;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 2 : alloc_hint;
        uint32_t stop  = pass ? alloc_hint : last;
        for (uint32_t c = start; c < stop; c++) {
            if (fat_next(c) == 0) {
                if (!set_fat(c, 0x0FFFFFFFu)) return false;
                alloc_hint = c + 1;
                if (alloc_hint >= last) alloc_hint = 2;
                *out = c;
                return true;
            }
        }
    }
    return false;
}

static void free_chain(uint32_t first) {
    uint32_t c = first;
    for (uint32_t guard = 0; c >= 2 && c < 0x0FFFFFF8u && guard < 4096; guard++) {
        uint32_t next = fat_next(c);
        set_fat(c, 0);
        c = next;
    }
}

static void make_short(const char* name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int n = 0, dot = -1;
    for (const char* p = name; *p && n < 12; p++) {
        if (*p == '.') { dot = n; continue; }
        n++;
    }
    int base_len = (dot >= 0 && dot <= 8) ? dot : (n > 8 ? 8 : n);
    if (dot < 0 || dot > 8) base_len = n > 8 ? 8 : n;
    int bi = 0;
    for (const char* p = name; *p && bi < base_len; p++) {
        if (*p == '.') continue;
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '~'))
            ch = '_';
        out[bi++] = (uint8_t)ch;
    }
    if (dot >= 0 && dot < 8) {
        int ei = 0;
        const char* p = name + dot + 1;
        for (; *p && ei < 3; p++) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-'))
                ch = '_';
            out[8 + ei++] = (uint8_t)ch;
        }
    }
}

/* Find the directory slot for `shortname`; if it does not exist, return the
 * first free 32-byte slot (extending the directory chain if necessary). */
static bool_t __attribute__((unused))
dir_slot(uint32_t dir_cluster, const uint8_t shortname[11],
         bool_t* exists, uint32_t* slot_cluster,
         uint32_t* slot_off, uint32_t* entry_cluster,
         uint32_t* entry_size) {
    uint32_t free_c = 0xFFFFFFFFu, free_off = 0;
    uint32_t last = dir_cluster;
    *exists = false;

    for (uint32_t c = dir_cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u;
         c = fat_next(c)) {
        if (++guard > 4096) return false;
        last = c;
        if (!read_cluster(c, dirbuf)) return false;
        for (uint32_t off = 0; off + 32 <= vol.spc * SECTOR; off += 32) {
            const uint8_t* e = dirbuf + off;
            if (e[0] == 0xE5) {
                if (free_c == 0xFFFFFFFFu) { free_c = c; free_off = off; }
                continue;
            }
            if (e[0] == 0x00) {
                if (free_c == 0xFFFFFFFFu) { free_c = c; free_off = off; }
                goto slot_found;
            }
            if ((e[11] & 0x3F) == 0x0F || (e[11] & 0x08)) continue;
            int same = 1;
            for (int i = 0; i < 11; i++) if (e[i] != shortname[i]) same = 0;
            if (same) {
                *exists = true;
                *slot_cluster = c;
                *slot_off = off;
                if (entry_cluster)
                    *entry_cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                if (entry_size) *entry_size = le32(e + 28);
                return true;
            }
        }
    }

slot_found:
    if (free_c != 0xFFFFFFFFu) {
        *slot_cluster = free_c;
        *slot_off = free_off;
        return true;
    }

    /* Directory is full: append one zeroed cluster. */
    uint32_t fresh;
    if (!alloc_cluster(&fresh)) return false;
    if (!set_fat(last, fresh)) return false;
    uint32_t bytes = vol.spc * SECTOR;
    memset(dirbuf, 0, bytes);
    if (!wr((uint64_t)(vol.data_start + (fresh - 2) * vol.spc) * SECTOR,
            dirbuf, bytes)) return false;
    *slot_cluster = fresh;
    *slot_off = 0;
    return true;
}

/* ---------------- long-name creation helpers ---------------- */

static bool_t name_is_83(const char* n) {
    int base = 0, ext = 0, dot = 0;
    for (const char* p = n; *p; p++) {
        if (*p == '.') {
            if (dot) return false;
            dot = 1;
            continue;
        }
        if (*p >= 'a' && *p <= 'z') return false;
        if (!dot) { if (++base > 8) return false; }
        else      { if (++ext > 3) return false; }
    }
    return base > 0;
}

/* Build a unique "NAME~n.EXT" 8.3 alias for a long name. */
static void make_alias(const char* name, uint8_t out[11]) {
    char base[8];
    int bn = 0;
    const char* p = name;
    for (; *p && *p != '.' && bn < 6; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
            ch = '_';
        base[bn++] = ch;
    }
    while (bn < 6) base[bn++] = '_';
    char alias[20];
    int a = 0;
    for (int i = 0; i < 6; i++) alias[a++] = base[i];
    alias[a++] = '~';
    alias[a++] = '1';
    if (*p == '.') {
        alias[a++] = '.';
        const char* q = p + 1;
        for (int i = 0; *q && i < 3; q++, i++) {
            char ch = *q;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            alias[a++] = ch;
        }
    }
    alias[a] = '\0';
    make_short(alias, out);
}

/* Find an existing directory entry by long or short name. */
static bool_t dir_find(uint32_t dir_cluster, const char* name, bool_t* found,
                       uint32_t* slot_c, uint32_t* slot_off,
                       uint32_t* entry_cluster, uint32_t* entry_size) {
    *found = false;
    for (uint32_t c = dir_cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u;
         c = fat_next(c)) {
        if (++guard > 4096) return false;
        if (!read_cluster(c, dirbuf)) return false;
        char lfn[260];
        lfn[0] = '\0';
        int have = 0;
        for (uint32_t off = 0; off + 32 <= vol.spc * SECTOR; off += 32) {
            const uint8_t* e = dirbuf + off;
            if (e[0] == 0x00) return true;
            if (e[0] == 0xE5) { have = 0; lfn[0] = '\0'; continue; }
            uint8_t attr = e[11];
            if ((attr & 0x3F) == 0x0F) {
                uint8_t seq = e[0] & 0x1F;
                if (!seq || seq > 20) continue;
                int base = ((int)seq - 1) * 13;
                for (int i = 0; i < 13; i++) {
                    uint8_t ch;
                    if (i < 5)       ch = e[1 + i];
                    else if (i < 11) ch = e[14 + (i - 5)];
                    else             ch = e[28 + (i - 11)];
                    if (base + i < 259) lfn[base + i] = ch;
                }
                lfn[259] = '\0';
                have = 1;
                continue;
            }
            if (attr & 0x08) { have = 0; lfn[0] = '\0'; continue; }
            char sn[16];
            short_name(e, sn);
            if ((have && ci_cmp(lfn, name) == 0) || ci_cmp(sn, name) == 0) {
                *found = true;
                *slot_c = c;
                *slot_off = off;
                if (entry_cluster)
                    *entry_cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                if (entry_size) *entry_size = le32(e + 28);
                return true;
            }
            have = 0;
            lfn[0] = '\0';
        }
    }
    return true;
}

/* Find `need` contiguous free 32-byte slots; append a cluster if needed. */
static bool_t dir_free_run(uint32_t dir_cluster, int need,
                           uint32_t* slot_c, uint32_t* slot_off) {
    uint32_t last = dir_cluster;
    for (uint32_t c = dir_cluster, guard = 0; c >= 2 && c < 0x0FFFFFF8u;
         c = fat_next(c)) {
        if (++guard > 4096) return false;
        last = c;
        if (!read_cluster(c, dirbuf)) return false;
        int run = 0;
        uint32_t start = 0;
        for (uint32_t off = 0; off + 32 <= vol.spc * SECTOR; off += 32) {
            const uint8_t* e = dirbuf + off;
            bool_t free = (e[0] == 0x00 || e[0] == 0xE5);
            if (free) {
                if (run == 0) start = off;
                run++;
                if (run >= need) { *slot_c = c; *slot_off = start; return true; }
            } else {
                run = 0;
            }
        }
    }
    uint32_t fresh;
    if (!alloc_cluster(&fresh)) return false;
    if (!set_fat(last, fresh)) return false;
    uint32_t bytes = vol.spc * SECTOR;
    memset(dirbuf, 0, bytes);
    if (!wr((uint64_t)(vol.data_start + (fresh - 2) * vol.spc) * SECTOR,
            dirbuf, bytes)) return false;
    *slot_c = fresh;
    *slot_off = 0;
    return ((int)(bytes / 32) >= need);
}

static uint8_t lfn_checksum(const uint8_t sn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sn[i]);
    }
    return sum;
}

/* Write count LFN entries + the short entry into one cluster buffer. */
static bool_t dir_write_long(uint32_t slot_c, uint32_t slot_off,
                             const char* name, const uint8_t sn[11],
                             uint32_t first, uint32_t size, int entries) {
    if (!read_cluster(slot_c, dirbuf)) return false;
    uint8_t csum = lfn_checksum(sn);
    int len = 0;
    while (name[len]) len++;
    for (int seq = entries; seq >= 1; seq--) {
        uint8_t* e = dirbuf + slot_off + (uint32_t)(entries - seq) * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (seq == entries ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = csum;
        for (int i = 0; i < 13; i++) {
            int pos = (seq - 1) * 13 + i;
            uint8_t ch = (pos < len) ? (uint8_t)name[pos] : 0;
            if (i < 5)       e[1 + i] = ch;
            else if (i < 11) e[14 + (i - 5)] = ch;
            else             e[28 + (i - 11)] = ch;
        }
    }
    uint8_t* s = dirbuf + slot_off + (uint32_t)entries * 32;
    memset(s, 0, 32);
    for (int i = 0; i < 11; i++) s[i] = sn[i];
    s[11] = 0x20;
    s[20] = (uint8_t)(first >> 16);
    s[21] = (uint8_t)(first >> 24);
    s[26] = (uint8_t)first;
    s[27] = (uint8_t)(first >> 8);
    memcpy(s + 28, &size, 4);
    uint64_t off = (uint64_t)(vol.data_start + (slot_c - 2) * vol.spc) * SECTOR;
    return wr(off, dirbuf, vol.spc * SECTOR);
}

int fat_write_file(const char* path, const void* data, uint32_t len) {
    if (!present || !path) return -1;

    /* Split parent path and file component. */
    char parent[96];
    const char* slash = 0;
    for (const char* p = path; *p; p++) if (*p == '/') slash = p;
    const char* name = slash ? slash + 1 : path;
    if (!name[0]) return -1;
    if (slash) {
        uint32_t n = (uint32_t)(slash - path);
        if (n == 0) { parent[0] = '/'; parent[1] = '\0'; }
        else {
            if (n > sizeof(parent) - 1) return -1;
            memcpy(parent, path, n);
            parent[n] = '\0';
        }
    } else {
        parent[0] = '/';
        parent[1] = '\0';
    }

    dirent_t pd;
    if (!resolve(parent, &pd) || !(pd.attr & 0x10)) return -1;

    uint8_t sn[11];
    bool_t use_lfn = !name_is_83(name);
    if (use_lfn) make_alias(name, sn);
    else         make_short(name, sn);

    int nlen = 0;
    while (name[nlen]) nlen++;
    int lfn_entries = use_lfn ? (nlen + 12) / 13 : 0;
    if (lfn_entries > 19) return -1;

    bool_t exists;
    uint32_t slot_c = 0, slot_off = 0, old_c = 0, old_size = 0;
    if (!dir_find(pd.cluster, name, &exists, &slot_c, &slot_off, &old_c, &old_size))
        return -1;
    if (!exists) {
        if (!dir_free_run(pd.cluster, lfn_entries + 1, &slot_c, &slot_off))
            return -1;
    }

    uint32_t first = 0, prev = 0, written = 0;
    uint32_t bytes_per = vol.spc * SECTOR;
    if (exists && old_c) free_chain(old_c);

    while (written < len) {
        uint32_t c;
        if (!alloc_cluster(&c)) { if (first) free_chain(first); return -1; }
        if (!first) first = c;
        else if (!set_fat(prev, c)) { if (first) free_chain(first); return -1; }
        uint32_t chunk = len - written;
        if (chunk > bytes_per) chunk = bytes_per;
        uint64_t off = (uint64_t)(vol.data_start + (c - 2) * vol.spc) * SECTOR;
        if (!wr(off, (const uint8_t*)data + written, chunk)) {
            if (first) free_chain(first);
            return -1;
        }
        written += chunk;
        prev = c;
    }
    if (!first && len == 0) first = 0;

    if (use_lfn && !exists) {
        if (!dir_write_long(slot_c, slot_off, name, sn, first, len, lfn_entries)) {
            if (first) free_chain(first);
            return -1;
        }
        return (int)len;
    }

    /* Rewrite the (short) directory entry in place. */
    if (!read_cluster(slot_c, dirbuf)) {
        if (first) free_chain(first);
        return -1;
    }
    uint8_t* e = dirbuf + slot_off;
    if (!exists) {
        for (int i = 0; i < 11; i++) e[i] = sn[i];
    }
    e[11] = 0x20;                      /* archive */
    e[12] = 0;
    e[13] = 0;
    e[14] = e[15] = e[16] = e[17] = e[18] = e[19] = 0;
    e[20] = (uint8_t)(first >> 16);
    e[21] = (uint8_t)(first >> 24);
    e[22] = e[23] = e[24] = e[25] = 0;
    e[26] = (uint8_t)first;
    e[27] = (uint8_t)(first >> 8);
    memcpy(e + 28, &len, 4);
    uint64_t off = (uint64_t)(vol.data_start + (slot_c - 2) * vol.spc) * SECTOR;
    if (!wr(off, dirbuf, bytes_per)) {
        if (first) free_chain(first);
        return -1;
    }
    return (int)len;
}

const char* fat_label(void) { return vol.label; }
uint32_t fat_part_lba(void) { return (uint32_t)(vol.base / SECTOR); }

static bool_t try_volume(uint64_t base_bytes) {
    uint8_t bs[SECTOR];
    if (!rd(base_bytes, bs, SECTOR)) return false;
    if (bs[0] != 0xEB && bs[0] != 0xE9) return false;

    uint32_t spc  = bs[13];
    uint32_t rsvd = le16(bs + 14);
    uint32_t nfat = bs[16];
    uint32_t rootent = le16(bs + 17);
    uint32_t total = le16(bs + 19);
    uint32_t fsz16 = le16(bs + 22);
    uint32_t fsz32 = le32(bs + 36);
    uint32_t rootc = le32(bs + 44);
    if (!total) total = le32(bs + 32);

    /* FAT32 only: no 16-bit FAT size, no fixed root directory. */
    if (le16(bs + 11) != SECTOR || fsz16 != 0 || rootent != 0) return false;
    if (!spc || spc > 32 || (spc & (spc - 1))) return false;
    if (rsvd == 0 || nfat == 0 || nfat > 2 || fsz32 == 0 || rootc < 2) return false;
    if (total <= rsvd + nfat * fsz32) return false;

    vol.base = base_bytes;
    vol.spc = spc;
    vol.reserved = rsvd;
    vol.nfats = nfat;
    vol.fsz = fsz32;
    vol.root_cluster = rootc;
    vol.total_sectors = total;
    vol.fat_start = (uint32_t)(base_bytes / SECTOR) + rsvd;
    vol.data_start = vol.fat_start + nfat * fsz32;
    vol.cluster_count = (total - rsvd - nfat * fsz32) / spc;

    for (int i = 0; i < 11; i++) vol.label[i] = bs[71 + i];
    vol.label[11] = '\0';
    for (int i = 10; i >= 0 && vol.label[i] == ' '; i--) vol.label[i] = '\0';
    return true;
}

bool_t fat_init(void) {
    present = false;
    bool_t found = false;

    for (int i = 0; i < part_count(); i++) {
        part_t p;
        if (!part_get(i, &p)) continue;
        if (try_volume((uint64_t)p.start_lba * SECTOR)) { found = true; break; }
    }
    if (!found && !part_is_partitioned()) {
        found = try_volume(0);
    }
    if (!found) return false;

    present = true;
    serial_write("[fat] FAT32 ");
    serial_write(vol.label);
    serial_write(" at LBA ");
    { char b[12]; u32_to_dec(b, fat_part_lba()); serial_write(b); }
    serial_write("\n");
    return true;
}

bool_t fat_present(void) { return present; }
