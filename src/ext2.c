/* ext2.c - minimal read-only ext2 driver.
 *
 * Layout: superblock at partition byte offset 1024, group descriptor table
 * at block (first_data_block + 1), block size from s_log_block_size.
 * rev 0 images only (128-byte inodes); rev 1 with 128-byte inodes also
 * works. Directory lookup walks linear entries; file reads use the 12
 * direct blocks and the single-indirect block.
 */
#include "ext2.h"
#include "part.h"
#include "ata.h"
#include "string.h"
#include "serial.h"

#define SECTOR 512u
#define MAX_BLOCK 4096u

typedef struct {
    uint64_t base;               /* partition start in bytes */
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inode_table;        /* block number of group 0 inode table */
    uint32_t inode_table_blocks;
    char     volume[32];
} ext2_vol_t;

static ext2_vol_t vol;
static bool_t present;
static uint8_t blkbuf[MAX_BLOCK] __attribute__((aligned(4)));

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool_t rd(uint64_t byte_off, void* buf, uint32_t n) {
    if (byte_off > 0xFFFFFFFFu || n > 0xFFFFFFFFu - (uint32_t)byte_off)
        return false;
    return ata_read_bytes((uint32_t)byte_off, buf, n) == (int)n;
}

static bool_t read_block(uint32_t block, void* buf) {
    uint64_t off = vol.base + (uint64_t)block * vol.block_size;
    return rd(off, buf, vol.block_size);
}

/* Load the inode structure (first 128 bytes are used). */
static bool_t read_inode(uint32_t ino, uint8_t* out) {
    if (ino < 1 || ino > vol.inodes_count) return false;
    uint32_t group = (ino - 1) / vol.inodes_per_group;
    uint32_t index = (ino - 1) % vol.inodes_per_group;

    /* Group descriptor table starts at block (first_data_block + 1);
     * descriptor entries are 32 bytes each in this subset. */
    uint64_t gd_off = vol.base +
                      ((uint64_t)(vol.first_data_block + 1) * vol.block_size) +
                      (uint64_t)group * 32;
    uint8_t gd[64];
    if (!rd(gd_off, gd, 32)) return false;
    uint32_t itable = le32(gd + 8);
    uint64_t off = vol.base + (uint64_t)itable * vol.block_size +
                   (uint64_t)index * vol.inode_size;
    return rd(off, out, vol.inode_size);
}

/* Read one file block (logical index) from an inode. */
static bool_t inode_block(const uint8_t* inode, uint32_t logical, uint32_t* out) {
    uint32_t ptrs = vol.block_size / 4;
    if (logical < 12) {
        *out = le32(inode + 40 + logical * 4);
        return *out != 0;
    }
    logical -= 12;
    if (logical >= ptrs) return false;          /* only single indirect */
    uint32_t ind = le32(inode + 40 + 12 * 4);
    if (!ind) return false;
    uint8_t indbuf[MAX_BLOCK];
    if (!read_block(ind, indbuf)) return false;
    *out = le32(indbuf + logical * 4);
    return *out != 0;
}

typedef struct {
    uint32_t ino;
    uint32_t size;
    uint16_t mode;
} ext2_ent_t;

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

static bool_t lookup_in_dir(uint32_t dir_ino, const char* name, ext2_ent_t* out) {
    uint8_t inode[256];
    if (!read_inode(dir_ino, inode)) return false;
    uint32_t size = le32(inode + 4);
    for (uint32_t logical = 0, done = 0; done < size; logical++) {
        uint32_t block;
        if (!inode_block(inode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return false;
        for (uint32_t off = 0; off + 8 <= vol.block_size && done < size; ) {
            uint32_t ent_ino = le32(blkbuf + off);
            uint16_t rec_len = le16(blkbuf + off + 4);
            uint16_t name_len = le16(blkbuf + off + 6);
            if (rec_len < 8 || off + rec_len > vol.block_size) return false;
            if (ent_ino && name_len && off + 8 + name_len <= vol.block_size) {
                char nm[64];
                uint32_t n = name_len > 63 ? 63 : name_len;
                memcpy(nm, blkbuf + off + 8, n);
                nm[n] = '\0';
                if (ci_cmp(nm, name) == 0) {
                    out->ino = ent_ino;
                    uint8_t fi[256];
                    if (!read_inode(ent_ino, fi)) return false;
                    out->size = le32(fi + 4);
                    out->mode = le16(fi + 0);
                    return true;
                }
            }
            off += rec_len;
            done += rec_len;
        }
    }
    return false;
}

static bool_t resolve(const char* path, ext2_ent_t* out) {
    if (!present || !path) return false;
    uint32_t ino = 2;                       /* root inode */
    char comp[64];
    const char* p = path;
    while (*p == '/') p++;
    out->ino = ino;
    out->size = 0;
    out->mode = 0x4000;
    if (!*p) return true;

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/') p++;
        ext2_ent_t d;
        if (!lookup_in_dir(ino, comp, &d)) return false;
        if (*p) {
            if ((d.mode & 0xF000) != 0x4000) return false;
            ino = d.ino;
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

int ext2_list(const char* path, char* out, uint32_t cap) {
    ext2_ent_t d;
    if (!cap) return -1;
    out[0] = '\0';
    if (!resolve(path, &d) || (d.mode & 0xF000) != 0x4000) return -1;

    uint8_t inode[256];
    if (!read_inode(d.ino, inode)) return -1;
    uint32_t size = le32(inode + 4);
    int pos = 0;

    for (uint32_t logical = 0, done = 0; done < size; logical++) {
        uint32_t block;
        if (!inode_block(inode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return -1;
        for (uint32_t off = 0; off + 8 <= vol.block_size && done < size; ) {
            uint32_t ent_ino = le32(blkbuf + off);
            uint16_t rec_len = le16(blkbuf + off + 4);
            uint16_t name_len = le16(blkbuf + off + 6);
            if (rec_len < 8 || off + rec_len > vol.block_size) goto done;
            if (ent_ino && name_len && off + 8 + name_len <= vol.block_size) {
                char line[96];
                int lp = 0;
                uint8_t fi[256];
                uint16_t mode = 0;
                uint32_t fsize = 0;
                if (read_inode(ent_ino, fi)) {
                    mode = le16(fi + 0);
                    fsize = le32(fi + 4);
                }
                line[lp++] = (mode & 0xF000) == 0x4000 ? 'D' : 'F';
                line[lp++] = ' ';
                u32_to_dec(line + lp, fsize);
                while (line[lp]) lp++;
                line[lp++] = ' ';
                for (int i = 0; i < name_len && i < 40 && lp < 88; i++)
                    line[lp++] = (char)blkbuf[off + 8 + i];
                line[lp++] = '\n';
                if (app(out, cap, &pos, line, (uint32_t)lp) != 0) return -1;
            }
            off += rec_len;
            done += rec_len;
        }
    }
done:
    return pos;
}

int ext2_read_file(const char* path, void* buf, uint32_t cap) {
    ext2_ent_t d;
    if (!resolve(path, &d)) return -1;
    if ((d.mode & 0xF000) == 0x4000) return -1;

    uint8_t inode[256];
    if (!read_inode(d.ino, inode)) return -1;
    uint32_t want = d.size > cap ? cap : d.size;
    uint32_t done = 0;
    for (uint32_t logical = 0; done < want; logical++) {
        uint32_t block;
        if (!inode_block(inode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return -1;
        for (uint32_t i = 0; i < vol.block_size && done < want; i++)
            ((uint8_t*)buf)[done++] = blkbuf[i];
    }
    return (int)done;
}

const char* ext2_volume(void) { return vol.volume; }

static bool_t try_volume(uint64_t base) {
    uint8_t sb[SECTOR * 2];
    if (!rd(base + 1024, sb, SECTOR * 2)) return false;
    if (le16(sb + 56) != 0xEF53) return false;

    uint32_t log_bs = le32(sb + 24);
    if (log_bs > 2) return false;                      /* 1K..4K */
    uint32_t bs = 1024u << log_bs;
    uint32_t inodes = le32(sb + 0);
    uint32_t blocks = le32(sb + 4);
    uint32_t first = le32(sb + 20);
    uint32_t bpg = le32(sb + 32);
    uint32_t ipg = le32(sb + 40);
    uint32_t rev = le32(sb + 76);
    uint32_t isz = rev ? le32(sb + 88) : 128;
    if (!inodes || !blocks || !bpg || !ipg) return false;
    if (isz < 128 || isz > 256) isz = 128;

    vol.base = base;
    vol.block_size = bs;
    vol.inodes_count = inodes;
    vol.blocks_count = blocks;
    vol.first_data_block = first;
    vol.blocks_per_group = bpg;
    vol.inodes_per_group = ipg;
    vol.inode_size = isz;
    for (int i = 0; i < 32; i++) vol.volume[i] = 0;
    memcpy(vol.volume, sb + 120, 16);
    vol.volume[16] = '\0';

    uint8_t gd[MAX_BLOCK];
    if (!read_block(first + 1, gd)) return false;
    vol.inode_table = le32(gd + 8);
    uint32_t itb = (ipg * isz + bs - 1) / bs;
    vol.inode_table_blocks = itb;
    if (!vol.inode_table) return false;

    /* Sanity: the root inode must be readable. */
    uint64_t off = base + (uint64_t)vol.inode_table * bs + isz;
    uint8_t tmp[256];
    if (!rd(off, tmp, isz)) return false;
    return true;
}

bool_t ext2_init(void) {
    present = false;
    bool_t found = false;

    for (int i = 0; i < part_count(); i++) {
        part_t p;
        if (!part_get(i, &p)) continue;
        if (!p.gpt && p.type != 0x83) continue;
        if (try_volume((uint64_t)p.start_lba * SECTOR)) { found = true; break; }
    }
    if (!found && !part_is_partitioned()) {
        found = try_volume(0);
    }
    if (!found) return false;

    present = true;
    serial_write("[ext2] mounted ");
    serial_write(vol.volume);
    serial_write(" block_size=");
    { char b[12]; u32_to_dec(b, vol.block_size); serial_write(b); }
    serial_write("\n");
    return true;
}

bool_t ext2_present(void) { return present; }
