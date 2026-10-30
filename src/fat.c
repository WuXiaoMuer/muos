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
