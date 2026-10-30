/* part.c - MBR and GPT partition table parsing.
 *
 * Sector 0 is shared with the legacy MuFS layout: an unpartitioned disk has
 * a MuFS superblock there, while a partitioned disk has the classic 0xAA55
 * signature. `part_is_partitioned` lets fs.c avoid formatting a FAT/ext2
 * disk that happens to be attached.
 */
#include "part.h"
#include "ata.h"
#include "string.h"
#include "serial.h"

#define PART_SECTOR 512

static part_t parts[PART_MAX];
static int    nparts;
static bool_t present;
static bool_t partitioned;

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t* p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int read_sector(uint64_t lba, void* buf) {
    if (lba > 0xFFFFFFFFu) return -1;
    return ata_read_bytes((uint32_t)lba * PART_SECTOR, buf, PART_SECTOR);
}

/* GPT basic-data partition type GUID, little-endian byte layout. */
static const uint8_t guid_basic_data[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static void guid_name(const uint8_t* entry, char* out) {
    /* GPT names are UTF-16LE; keep the ASCII low bytes. */
    int o = 0;
    const uint8_t* n = entry + 56;
    for (int i = 0; i < 36 && o < 23; i += 2) {
        if (n[i] == 0) break;
        out[o++] = (char)n[i];
    }
    out[o] = '\0';
}

static bool_t parse_gpt(void) {
    uint8_t hdr[PART_SECTOR];
    if (read_sector(1, hdr) != PART_SECTOR) return false;
    if (memcmp(hdr, "EFI PART", 8) != 0) return false;

    uint64_t entries_lba = le64(hdr + 72);
    uint32_t num = le32(hdr + 80);
    uint32_t esz = le32(hdr + 84);
    if (esz < 128 || esz > 512 || num == 0 || num > 128) return false;

    uint8_t ent[1024];
    for (uint32_t i = 0; i < num && nparts < PART_MAX; i++) {
        uint64_t lba = entries_lba + ((uint64_t)i * esz) / PART_SECTOR;
        uint32_t off = (uint32_t)(((uint64_t)i * esz) % PART_SECTOR);
        if (off + esz > PART_SECTOR) {
            /* Entry straddles a sector boundary: read the next one. */
            if (read_sector(lba, ent) != PART_SECTOR) return false;
            if (read_sector(lba + 1, ent + PART_SECTOR) != PART_SECTOR) return false;
        } else if (read_sector(lba, ent) != PART_SECTOR) {
            return false;
        }
        const uint8_t* e = ent + off;
        if (memcmp(e, guid_basic_data, 16) != 0) continue;
        uint64_t first = le64(e + 32);
        uint64_t last  = le64(e + 40);
        if (last < first) continue;
        parts[nparts].start_lba = first;
        parts[nparts].sectors   = last - first + 1;
        parts[nparts].type      = 0xEE;
        parts[nparts].gpt       = true;
        guid_name(e, parts[nparts].name);
        nparts++;
    }
    return nparts > 0;
}

bool_t part_init(void) {
    nparts = 0;
    present = false;
    partitioned = false;
    if (!ata_present()) return false;

    uint8_t mbr[PART_SECTOR];
    if (read_sector(0, mbr) != PART_SECTOR) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;

    partitioned = true;
    present = true;

    /* GPT: protective MBR (type 0xEE) in the first entry. */
    bool_t has_gpt = false;
    for (int i = 0; i < 4; i++) {
        if (mbr[446 + i * 16 + 4] == 0xEE) { has_gpt = true; break; }
    }
    if (has_gpt && parse_gpt()) {
        serial_write("[part] GPT: ");
        { char b[12]; u32_to_dec(b, (uint32_t)nparts); serial_write(b); }
        serial_write(" partition(s)\n");
        return true;
    }

    for (int i = 0; i < 4 && nparts < PART_MAX; i++) {
        const uint8_t* e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t first = le32(e + 8);
        uint32_t count = le32(e + 12);
        if (!type || !first || !count) continue;
        parts[nparts].start_lba = first;
        parts[nparts].sectors   = count;
        parts[nparts].type      = type;
        parts[nparts].gpt       = false;
        parts[nparts].name[0]   = '\0';
        nparts++;
    }
    serial_write("[part] MBR: ");
    { char b[12]; u32_to_dec(b, (uint32_t)nparts); serial_write(b); }
    serial_write(" partition(s)\n");
    return true;
}

bool_t part_present(void)     { return present; }
bool_t part_is_partitioned(void) { return partitioned; }
int    part_count(void)       { return nparts; }

bool_t part_get(int index, part_t* out) {
    if (index < 0 || index >= nparts || !out) return false;
    *out = parts[index];
    return true;
}

bool_t part_find_type(uint8_t mbr_type, int* index) {
    for (int i = 0; i < nparts; i++) {
        if (!parts[i].gpt && parts[i].type == mbr_type) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}
