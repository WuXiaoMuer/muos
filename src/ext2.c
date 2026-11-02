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
/* Write path scratch buffers: kept out of the 8KB kernel stack because
 * alloc_block()/write_inode_data() can nest. */
static uint8_t mapbuf[MAX_BLOCK] __attribute__((aligned(4)));
static uint8_t indbuf_w[MAX_BLOCK] __attribute__((aligned(4)));

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

/* Read one file block (logical index) from an inode: 12 direct pointers,
 * then single, double and triple indirect blocks. */
static bool_t inode_block(const uint8_t* inode, uint32_t logical, uint32_t* out) {
    uint32_t ptrs = vol.block_size / 4;
    if (logical < 12) {
        *out = le32(inode + 40 + logical * 4);
        return *out != 0;
    }
    logical -= 12;
    uint8_t indbuf[MAX_BLOCK];

    if (logical < ptrs) {                       /* single indirect */
        uint32_t ind = le32(inode + 40 + 12 * 4);
        if (!ind || !read_block(ind, indbuf)) return false;
        *out = le32(indbuf + logical * 4);
        return *out != 0;
    }
    logical -= ptrs;

    uint32_t per_dind = ptrs * ptrs;
    if (logical < per_dind) {                   /* double indirect */
        uint32_t dind = le32(inode + 40 + 13 * 4);
        if (!dind || !read_block(dind, indbuf)) return false;
        uint32_t ind = le32(indbuf + (logical / ptrs) * 4);
        if (!ind || !read_block(ind, indbuf)) return false;
        *out = le32(indbuf + (logical % ptrs) * 4);
        return *out != 0;
    }
    logical -= per_dind;

    uint32_t per_tind = per_dind * ptrs;
    if (logical < per_tind) {                   /* triple indirect */
        uint32_t tind = le32(inode + 40 + 14 * 4);
        if (!tind || !read_block(tind, indbuf)) return false;
        uint32_t dind = le32(indbuf + (logical / per_dind) * 4);
        if (!dind || !read_block(dind, indbuf)) return false;
        uint32_t ind = le32(indbuf + ((logical / ptrs) % ptrs) * 4);
        if (!ind || !read_block(ind, indbuf)) return false;
        *out = le32(indbuf + (logical % ptrs) * 4);
        return *out != 0;
    }
    return false;                               /* beyond triple indirect */
}

typedef struct {
    uint32_t ino;
    uint32_t size;
    uint16_t mode;
} ext2_ent_t;

static bool_t ext2_adjust_links(uint32_t ino, int delta);

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

bool_t ext2_stat(const char* path, bool_t* is_dir, uint32_t* size) {
    ext2_ent_t d;
    if (!resolve(path, &d)) return false;
    if (is_dir) *is_dir = (d.mode & 0xF000) == 0x4000;
    if (size)   *size = d.size;
    return true;
}

/* VFS random access: jump straight to the logical block holding offset. */
int ext2_read_at(const char* path, uint32_t offset, void* buf, uint32_t cap) {
    ext2_ent_t d;
    if (!resolve(path, &d) || (d.mode & 0xF000) == 0x4000) return -1;
    if (offset >= d.size) return 0;
    uint32_t want = d.size - offset;
    if (want > cap) want = cap;

    uint8_t inode[256];
    if (!read_inode(d.ino, inode)) return -1;
    uint32_t bs = vol.block_size;
    uint32_t logical = offset / bs;
    uint32_t within = offset % bs;
    uint32_t done = 0;
    while (done < want) {
        uint32_t block;
        if (!inode_block(inode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return -1;
        for (uint32_t i = within; i < bs && done < want; i++)
            ((uint8_t*)buf)[done++] = blkbuf[i];
        within = 0;
        logical++;
    }
    return (int)done;
}

/* ── Write path ───────────────────────────
 * ext2 rev 0 layout, group 0 only (the test images are a single group).
 * Allocation walks the block/inode bitmaps and updates the free counters
 * in the superblock and the group descriptor. Files use the 12 direct
 * pointers plus one single-indirect block, matching the read path. */

#define EXT2_SB_FREE_BLOCKS  12u
#define EXT2_SB_FREE_INODES  16u
#define EXT2_GD_FREE_BLOCKS  12u
#define EXT2_GD_FREE_INODES  14u
#define EXT2_ENTRY_MIN       8u

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool_t wr(uint64_t byte_off, const void* buf, uint32_t n) {
    if (byte_off > 0xFFFFFFFFu || n > 0xFFFFFFFFu - (uint32_t)byte_off)
        return false;
    return ata_write_bytes((uint32_t)byte_off, buf, n) == (int)n;
}

static bool_t write_block(uint32_t block, const void* buf) {
    return wr(vol.base + (uint64_t)block * vol.block_size,
              buf, vol.block_size);
}

static uint64_t gd0_off(void) {
    return vol.base + (uint64_t)(vol.first_data_block + 1) * vol.block_size;
}

static bool_t gd0_read(uint8_t* out, uint32_t n) {
    return rd(gd0_off(), out, n);
}

/* Adjust a 32-bit superblock counter and the matching 16-bit group
 * descriptor counter in one place. */
static bool_t adjust_free(uint32_t sb_off, uint32_t gd_off, int32_t delta) {
    uint8_t b[4];
    if (!rd(vol.base + 1024 + sb_off, b, 4)) return false;
    int32_t v = (int32_t)le32(b);
    if (delta < 0 && v < -delta) return false;
    put32(b, (uint32_t)(v + delta));
    if (!wr(vol.base + 1024 + sb_off, b, 4)) return false;

    uint8_t g[2];
    if (!rd(gd0_off() + gd_off, g, 2)) return false;
    int32_t w = (int32_t)le16(g);
    if (delta < 0 && w < -delta) return false;
    put16(g, (uint16_t)(w + delta));
    return wr(gd0_off() + gd_off, g, 2);
}

static bool_t bitmap_set(uint32_t block, uint32_t bit, bool_t used) {
    if (!read_block(block, mapbuf)) return false;
    uint32_t idx = bit / 8;
    if (idx >= vol.block_size) return false;
    if (used) mapbuf[idx] |= (uint8_t)(1u << (bit % 8));
    else      mapbuf[idx] &= (uint8_t)~(1u << (bit % 8));
    return write_block(block, mapbuf);
}

/* Allocate one data block from group 0. Returns the absolute block number
 * or -1. */
static int alloc_block(void) {
    uint8_t gd[8];
    if (!gd0_read(gd, 8)) return -1;
    uint32_t bbm = le32(gd);
    if (!read_block(bbm, mapbuf)) return -1;

    uint32_t limit = vol.blocks_per_group;
    if (limit > vol.block_size * 8) limit = vol.block_size * 8;
    for (uint32_t bit = 0; bit < limit; bit++) {
        uint32_t block = vol.first_data_block + bit;
        if (block >= vol.blocks_count) break;
        if (!(mapbuf[bit / 8] & (1u << (bit % 8)))) {
            mapbuf[bit / 8] |= (uint8_t)(1u << (bit % 8));
            if (!write_block(bbm, mapbuf)) return -1;
            if (!adjust_free(EXT2_SB_FREE_BLOCKS, EXT2_GD_FREE_BLOCKS, -1))
                return -1;
            return (int)block;
        }
    }
    return -1;
}

static bool_t free_block(uint32_t block) {
    if (block < vol.first_data_block || block >= vol.blocks_count)
        return false;
    uint32_t bit = block - vol.first_data_block;
    if (bit >= vol.block_size * 8) return false;

    uint8_t gd[8];
    if (!gd0_read(gd, 8)) return false;
    uint32_t bbm = le32(gd);
    if (!bitmap_set(bbm, bit, false)) return false;
    adjust_free(EXT2_SB_FREE_BLOCKS, EXT2_GD_FREE_BLOCKS, 1);
    return true;
}

static int alloc_inode(void) {
    uint8_t gd[8];
    if (!gd0_read(gd, 8)) return -1;
    uint32_t ibm = le32(gd + 4);
    if (!read_block(ibm, mapbuf)) return -1;

    uint32_t limit = vol.inodes_per_group;
    if (limit > vol.block_size * 8) limit = vol.block_size * 8;
    for (uint32_t bit = 0; bit < limit; bit++) {
        uint32_t ino = bit + 1;
        if (ino > vol.inodes_count) break;
        if (!(mapbuf[bit / 8] & (1u << (bit % 8)))) {
            mapbuf[bit / 8] |= (uint8_t)(1u << (bit % 8));
            if (!write_block(ibm, mapbuf)) return -1;
            if (!adjust_free(EXT2_SB_FREE_INODES, EXT2_GD_FREE_INODES, -1))
                return -1;
            return (int)ino;
        }
    }
    return -1;
}

static bool_t inode_off(uint32_t ino, uint64_t* off) {
    if (ino < 1 || ino > vol.inodes_count) return false;
    uint32_t group = (ino - 1) / vol.inodes_per_group;
    uint32_t index = (ino - 1) % vol.inodes_per_group;
    uint8_t gd[64];
    if (!rd(gd0_off() + (uint64_t)group * 32, gd, 32)) return false;
    uint32_t itable = le32(gd + 8);
    if (!itable) return false;
    *off = vol.base + (uint64_t)itable * vol.block_size +
           (uint64_t)index * vol.inode_size;
    return true;
}

static bool_t write_inode(uint32_t ino, const uint8_t* inode) {
    uint64_t off;
    if (!inode_off(ino, &off)) return false;
    return wr(off, inode, vol.inode_size);
}

static void write_dirent(uint8_t* p, uint32_t ino, uint16_t rec,
                         const char* name, uint16_t nl) {
    put32(p, ino);
    put16(p + 4, rec);
    put16(p + 6, nl);
    for (uint16_t i = 0; i < nl; i++) p[8 + i] = (uint8_t)name[i];
    for (uint32_t i = 8u + nl; i < rec; i++) p[i] = 0;
}

/* Insert (name -> child) into dir_ino. Reuses free slots and splits an
 * existing entry's slack; otherwise appends one new directory block. */
static bool_t dir_add(uint32_t dir_ino, const char* name, uint32_t child) {
    uint16_t nl = (uint16_t)strlen(name);
    if (!nl || nl > 63) return false;

    uint8_t dinode[256];
    if (!read_inode(dir_ino, dinode)) return false;
    uint32_t bs = vol.block_size;
    uint32_t need = (EXT2_ENTRY_MIN + nl + 3u) & ~3u;
    uint32_t size = le32(dinode + 4);

    for (uint32_t logical = 0, done = 0; done < size; logical++) {
        uint32_t block;
        if (!inode_block(dinode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return false;
        for (uint32_t off = 0; off + 8 <= bs; ) {
            uint32_t ent_ino = le32(blkbuf + off);
            uint16_t rec = le16(blkbuf + off + 4);
            uint16_t eln = le16(blkbuf + off + 6);
            if (rec < 8 || off + rec > bs) break;

            if (ent_ino == 0 && rec >= need) {
                write_dirent(blkbuf + off, child, rec, name, nl);
                return write_block(block, blkbuf);
            }
            uint32_t minrec = (EXT2_ENTRY_MIN + eln + 3u) & ~3u;
            if (eln && rec >= minrec + need) {
                put16(blkbuf + off + 4, (uint16_t)minrec);
                write_dirent(blkbuf + off + minrec, child,
                             (uint16_t)(rec - minrec), name, nl);
                return write_block(block, blkbuf);
            }
            off += rec;
        }
        done += bs;
    }

    /* No slack: append a fresh block (direct pointers only). */
    uint32_t nlog = size / bs;
    if (nlog >= 12) return false;
    int block = alloc_block();
    if (block < 0) return false;
    for (uint32_t i = 0; i < bs; i++) blkbuf[i] = 0;
    write_dirent(blkbuf, child, (uint16_t)bs, name, nl);
    if (!write_block((uint32_t)block, blkbuf)) return false;

    put32(dinode + 40 + nlog * 4, (uint32_t)block);
    put32(dinode + 4, size + bs);
    put32(dinode + 28, le32(dinode + 28) + bs / 512u);
    return write_inode(dir_ino, dinode);
}

/* Release the data blocks referenced by an inode and clear its pointers. */
static void free_inode_blocks(uint8_t* inode) {
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t block = le32(inode + 40 + i * 4);
        if (block) free_block(block);
        put32(inode + 40 + i * 4, 0);
    }
    uint32_t ind = le32(inode + 40 + 12 * 4);
    if (ind) {
        if (read_block(ind, indbuf_w)) {
            for (uint32_t i = 0; i < vol.block_size / 4; i++) {
                uint32_t block = le32(indbuf_w + i * 4);
                if (block) free_block(block);
            }
        }
        free_block(ind);
        put32(inode + 40 + 12 * 4, 0);
    }
}

/* Write len bytes into freshly allocated blocks and update the inode. */
static bool_t write_inode_data(uint8_t* inode, const uint8_t* data,
                               uint32_t len) {
    uint32_t bs = vol.block_size;
    uint32_t ptrs = bs / 4;
    uint32_t nblocks = (len + bs - 1) / bs;
    if (nblocks > 12 + ptrs) return false;

    uint32_t ind_block = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        int block = alloc_block();
        if (block < 0) return false;

        if (i < 12) {
            put32(inode + 40 + i * 4, (uint32_t)block);
        } else {
            if (!ind_block) {
                int ib = alloc_block();
                if (ib < 0) return false;
                ind_block = (uint32_t)ib;
                for (uint32_t j = 0; j < bs; j++) indbuf_w[j] = 0;
                put32(inode + 40 + 12 * 4, ind_block);
            }
            put32(indbuf_w + (i - 12) * 4, (uint32_t)block);
        }

        uint32_t done = i * bs;
        uint32_t chunk = len - done > bs ? bs : len - done;
        for (uint32_t j = 0; j < bs; j++) blkbuf[j] = 0;
        if (chunk) memcpy(blkbuf, data + done, chunk);
        if (!write_block((uint32_t)block, blkbuf)) return false;
    }
    if (ind_block && !write_block(ind_block, indbuf_w)) return false;

    put32(inode + 4, len);
    put32(inode + 28, (nblocks + (ind_block ? 1u : 0u)) * (bs / 512u));
    return true;
}

static bool_t split_parent(const char* path, char* parent, char* name) {
    const char* p = path;
    while (*p == '/') p++;
    if (!*p) return false;

    int slash = -1;
    for (int i = 0; p[i]; i++) if (p[i] == '/') slash = i;
    if (slash < 0) {
        parent[0] = '/'; parent[1] = '\0';
        strcpy(name, p);
    } else {
        if (slash == 0) {
            parent[0] = '/'; parent[1] = '\0';
        } else {
            memcpy(parent, "/", 2);
            for (int i = 0; i < slash; i++) parent[i + 1] = p[i];
            parent[slash + 1] = '\0';
        }
        strcpy(name, p + slash + 1);
    }
    return name[0] != '\0';
}

int ext2_write_file(const char* path, const void* data, uint32_t len) {
    if (!present || !path || !data) return -1;

    char parent[64], name[64];
    if (!split_parent(path, parent, name)) return -1;
    if (strlen(name) > 63) return -1;

    ext2_ent_t par;
    if (!resolve(parent, &par) || (par.mode & 0xF000) != 0x4000)
        return -1;

    uint8_t inode[256];
    ext2_ent_t existing;
    uint32_t ino;

    if (lookup_in_dir(par.ino, name, &existing)) {
        if ((existing.mode & 0xF000) == 0x4000) return -1;
        ino = existing.ino;
        if (!read_inode(ino, inode)) return -1;
        free_inode_blocks(inode);           /* truncate before rewriting */
    } else {
        int n = alloc_inode();
        if (n < 0) return -1;
        ino = (uint32_t)n;
        for (uint32_t i = 0; i < 256; i++) inode[i] = 0;
        put16(inode + 0, 0x81A4);           /* regular file, 0644 */
        put16(inode + 26, 1);               /* links */
        if (!write_inode(ino, inode)) return -1;
        if (!dir_add(par.ino, name, ino)) {
            /* Roll the inode allocation back so a failed create leaks
             * neither the bitmap bit nor the free count. */
            uint8_t gd[8];
            if (gd0_read(gd, 8)) {
                uint32_t ibm = le32(gd + 4);
                bitmap_set(ibm, ino - 1, false);
                adjust_free(EXT2_SB_FREE_INODES, EXT2_GD_FREE_INODES, 1);
            }
            return -1;
        }
    }

    if (!write_inode_data(inode, (const uint8_t*)data, len)) return -1;
    if (!write_inode(ino, inode)) return -1;
    return (int)len;
}

/* Free an inode bitmap bit, its free count, and zero the inode. */
static bool_t inode_release(uint32_t ino) {
    if (ino < 1 || ino > vol.inodes_count) return false;
    uint8_t gd[8];
    if (!gd0_read(gd, 8)) return false;
    uint32_t ibm = le32(gd + 4);
    if (!bitmap_set(ibm, ino - 1, false)) return false;
    adjust_free(EXT2_SB_FREE_INODES, EXT2_GD_FREE_INODES, 1);
    uint8_t zero[256];
    for (uint32_t i = 0; i < 256; i++) zero[i] = 0;
    return write_inode(ino, zero);
}

/* Remove `name` from dir_ino, merging its record with the previous entry. */
static bool_t dir_remove(uint32_t dir_ino, const char* name,
                         uint32_t* child_out) {
    uint8_t dinode[256];
    if (!read_inode(dir_ino, dinode)) return false;
    uint32_t bs = vol.block_size;
    uint32_t size = le32(dinode + 4);

    for (uint32_t logical = 0, done = 0; done < size; logical++) {
        uint32_t block;
        if (!inode_block(dinode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return false;
        uint32_t prev = 0xFFFFFFFFu;
        for (uint32_t off = 0; off + 8 <= bs && done < size; ) {
            uint32_t ent_ino = le32(blkbuf + off);
            uint16_t rec = le16(blkbuf + off + 4);
            uint16_t eln = le16(blkbuf + off + 6);
            if (rec < 8 || off + rec > bs) return false;
            if (ent_ino && eln && off + 8 + eln <= bs) {
                char nm[64];
                uint32_t n = eln > 63 ? 63 : eln;
                memcpy(nm, blkbuf + off + 8, n);
                nm[n] = '\0';
                if (ci_cmp(nm, name) == 0) {
                    if (child_out) *child_out = ent_ino;
                    if (prev == 0xFFFFFFFFu) put32(blkbuf + off, 0);
                    else put16(blkbuf + prev + 4,
                               (uint16_t)(le16(blkbuf + prev + 4) + rec));
                    return write_block(block, blkbuf);
                }
            }
            prev = off;
            off += rec;
            done += rec;
        }
    }
    return false;
}

int ext2_delete_file(const char* path) {
    if (!present || !path) return -1;
    char parent[64], name[64];
    if (!split_parent(path, parent, name)) return -1;

    ext2_ent_t par, child;
    if (!resolve(parent, &par) || (par.mode & 0xF000) != 0x4000) return -1;
    if (!lookup_in_dir(par.ino, name, &child)) return -1;
    if ((child.mode & 0xF000) == 0x4000) return -1;

    uint8_t inode[256];
    if (!read_inode(child.ino, inode)) return -1;
    free_inode_blocks(inode);
    uint32_t removed = 0;
    if (!dir_remove(par.ino, name, &removed)) return -1;
    if (!inode_release(child.ino)) return -1;
    return 0;
}

int ext2_mkdir(const char* path) {
    if (!present || !path) return -1;
    char parent[64], name[64];
    if (!split_parent(path, parent, name)) return -1;

    ext2_ent_t par, existing;
    if (!resolve(parent, &par) || (par.mode & 0xF000) != 0x4000) return -1;
    if (lookup_in_dir(par.ino, name, &existing)) return -1;

    int n = alloc_inode();
    if (n < 0) return -1;
    uint32_t ino = (uint32_t)n;

    int blk = alloc_block();
    if (blk < 0) { inode_release(ino); return -1; }

    uint32_t bs = vol.block_size;
    for (uint32_t i = 0; i < bs; i++) blkbuf[i] = 0;
    write_dirent(blkbuf, ino, 12, ".", 1);
    write_dirent(blkbuf + 12, par.ino, (uint16_t)(bs - 12), "..", 2);
    if (!write_block((uint32_t)blk, blkbuf)) {
        free_block((uint32_t)blk);
        inode_release(ino);
        return -1;
    }

    uint8_t inode[256];
    for (uint32_t i = 0; i < 256; i++) inode[i] = 0;
    put16(inode + 0, 0x41ED);           /* directory, 0755 */
    put16(inode + 26, 2);               /* "." and ".." links */
    put32(inode + 4, bs);
    put32(inode + 28, bs / 512u);
    put32(inode + 40, (uint32_t)blk);
    if (!write_inode(ino, inode) || !dir_add(par.ino, name, ino)) {
        free_block((uint32_t)blk);
        inode_release(ino);
        return -1;
    }
    ext2_adjust_links(par.ino, +1);      /* child's ".." link */
    return 0;
}

int ext2_rename_file(const char* old_path, const char* new_path) {
    if (!present || !old_path || !new_path) return -1;
    char oparent[64], oname[64], nparent[64], nname[64];
    if (!split_parent(old_path, oparent, oname)
        || !split_parent(new_path, nparent, nname))
        return -1;
    if (strcmp(oparent, nparent) == 0 && strcmp(oname, nname) == 0) return 0;

    ext2_ent_t op, np, child, existing;
    if (!resolve(oparent, &op) || (op.mode & 0xF000) != 0x4000) return -1;
    if (!lookup_in_dir(op.ino, oname, &child)) return -1;
    if ((child.mode & 0xF000) == 0x4000) return -1;

    if (!resolve(nparent, &np) || (np.mode & 0xF000) != 0x4000) return -1;
    if (lookup_in_dir(np.ino, nname, &existing)) return -1;

    if (!dir_add(np.ino, nname, child.ino)) return -1;
    uint32_t removed = 0;
    if (!dir_remove(op.ino, oname, &removed)) return -1;
    return 0;
}

/* Directory is empty when only "." and ".." remain. */
static bool_t ext2_dir_empty(uint32_t dir_ino) {
    uint8_t dinode[256];
    if (!read_inode(dir_ino, dinode)) return false;
    uint32_t size = le32(dinode + 4);
    for (uint32_t logical = 0, done = 0; done < size; logical++) {
        uint32_t block;
        if (!inode_block(dinode, logical, &block)) break;
        if (!read_block(block, blkbuf)) return false;
        for (uint32_t off = 0; off + 8 <= vol.block_size && done < size; ) {
            uint32_t ino = le32(blkbuf + off);
            uint16_t rec = le16(blkbuf + off + 4);
            uint16_t nl = le16(blkbuf + off + 6);
            if (rec < 8 || off + rec > vol.block_size) return false;
            if (ino && nl && off + 8 + nl <= vol.block_size) {
                if (!(nl == 1 && blkbuf[off + 8] == '.') &&
                    !(nl == 2 && blkbuf[off + 8] == '.' && blkbuf[off + 9] == '.'))
                    return false;
            }
            off += rec;
            done += rec;
        }
    }
    return true;
}

/* Inode number stored in the ".." entry (second dirent of the first block). */
static uint32_t ext2_parent_inode(uint32_t dir_ino) {
    uint8_t inode[256];
    if (!read_inode(dir_ino, inode)) return 0;
    uint32_t block;
    if (!inode_block(inode, 0, &block)) return 0;
    if (!read_block(block, blkbuf)) return 0;
    return le32(blkbuf + 12);
}

/* Follow ".." links: is `start_parent` or any ancestor the `old_ino`? */
static bool_t ext2_parent_inside(uint32_t old_ino, uint32_t start_parent) {
    uint32_t cur = start_parent;
    for (int depth = 0; depth < 32 && cur >= 2; depth++) {
        if (cur == old_ino) return true;
        cur = ext2_parent_inode(cur);
    }
    return false;
}

static bool_t ext2_set_dotdot(uint32_t child_ino, uint32_t parent_ino) {
    uint8_t inode[256];
    if (!read_inode(child_ino, inode)) return false;
    uint32_t block;
    if (!inode_block(inode, 0, &block)) return false;
    if (!read_block(block, blkbuf)) return false;
    put32(blkbuf + 12, parent_ino);       /* ".." dirent, after the 12-byte "." */
    return write_block(block, blkbuf);
}

static bool_t ext2_adjust_links(uint32_t ino, int delta) {
    uint8_t inode[256];
    if (!read_inode(ino, inode)) return false;
    uint16_t links = le16(inode + 26);
    int32_t v = (int32_t)links + delta;
    if (v < 0) return false;
    put16(inode + 26, (uint16_t)v);
    return write_inode(ino, inode);
}

int ext2_rmdir(const char* path) {
    if (!present || !path) return -1;
    char parent[64], name[64];
    if (!split_parent(path, parent, name)) return -1;
    if (strcmp(parent, "/") == 0 && !name[0]) return -1;

    ext2_ent_t par, child;
    if (!resolve(parent, &par) || (par.mode & 0xF000) != 0x4000) return -1;
    if (!lookup_in_dir(par.ino, name, &child)) return -1;
    if ((child.mode & 0xF000) != 0x4000 || child.ino == 2) return -1;
    if (!ext2_dir_empty(child.ino)) return -2;       /* not empty */

    uint8_t inode[256];
    if (!read_inode(child.ino, inode)) return -1;
    free_inode_blocks(inode);
    uint32_t removed = 0;
    if (!dir_remove(par.ino, name, &removed)) return -1;
    if (!inode_release(child.ino)) return -1;
    ext2_adjust_links(par.ino, -1);
    return 0;
}

int ext2_rename_dir(const char* old_path, const char* new_path) {
    if (!present || !old_path || !new_path) return -1;
    char oparent[64], oname[64], nparent[64], nname[64];
    if (!split_parent(old_path, oparent, oname)
        || !split_parent(new_path, nparent, nname))
        return -1;
    if (strcmp(oparent, nparent) == 0 && strcmp(oname, nname) == 0) return 0;

    ext2_ent_t op, np, child, existing;
    if (!resolve(oparent, &op) || (op.mode & 0xF000) != 0x4000) return -1;
    if (!lookup_in_dir(op.ino, oname, &child)) return -1;
    if ((child.mode & 0xF000) != 0x4000 || child.ino == 2) return -1;

    if (!resolve(nparent, &np) || (np.mode & 0xF000) != 0x4000) return -1;
    if (lookup_in_dir(np.ino, nname, &existing)) return -1;
    if (np.ino != op.ino && ext2_parent_inside(child.ino, np.ino)) return -1;

    if (np.ino != op.ino) {
        if (!ext2_set_dotdot(child.ino, np.ino)) return -1;
        ext2_adjust_links(op.ino, -1);
        ext2_adjust_links(np.ino, +1);
    }
    if (!dir_add(np.ino, nname, child.ino)) return -1;
    uint32_t removed = 0;
    if (!dir_remove(op.ino, oname, &removed)) return -1;
    return 0;
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
