"""make_fs_images.py - generate minimal FAT32 and ext2 disk images for tests.

Both images carry an MBR partition table so the kernel's part.c / fat.c /
ext2.c drivers are exercised through the same path as a real disk:

  build/fat32.img  64MB, MBR type 0x0C partition at LBA 2048
  build/ext2.img    5MB, MBR type 0x83 partition at LBA 2048

FAT32 contents:
  /HELLO.TXT          "FAT-HELLO-OK\n"
  /LONGFILE.TXT       "FAT-LONG-OK\n"   (VFAT long-name entry)
  /SUBDIR/INNER.TXT   "FAT-SUB-OK\n"

ext2 contents:
  /hello.txt          "EXT2-READ-OK\n"
  /subdir/inside.txt  "EXT2-SUB-OK\n"
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def _mbr(part_type, start_lba, sectors):
    mbr = bytearray(512)
    e = 446
    mbr[e + 0] = 0x00
    mbr[e + 4] = part_type
    mbr[e + 8:e + 12] = start_lba.to_bytes(4, "little")
    mbr[e + 12:e + 16] = sectors.to_bytes(4, "little")
    mbr[510], mbr[511] = 0x55, 0xAA
    return mbr


def _entry(name11, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = name11.encode("ascii")[:11]
    e[11] = attr
    e[20:22] = ((cluster >> 16) & 0xFFFF).to_bytes(2, "little")
    e[26:28] = (cluster & 0xFFFF).to_bytes(2, "little")
    e[28:32] = size.to_bytes(4, "little")
    return e


def _lfn(name):
    """One VFAT long-name entry (names up to 13 chars)."""
    e = bytearray(32)
    e[0] = 0x41                      # last, sequence 1
    e[11] = 0x0F
    chars = name.encode("ascii")
    slots = [1, 2, 3, 4, 5, 14, 15, 16, 17, 18, 19, 28, 29]
    for i, off in enumerate(slots):
        e[off] = chars[i] if i < len(chars) else 0
    return e


def _guid_basic_data():
    # Same GUID the kernel expects, kept in on-disk little-endian order.
    return bytes([0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                  0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7])


def _gpt(total_sectors, start_lba, sectors, name):
    """Protective MBR + GPT header at LBA1 + entry array at LBA2."""
    pmbr = _mbr(0xEE, 1, min(total_sectors - 1, 0xFFFFFFFF))
    hdr = bytearray(512)
    hdr[0:8] = b"EFI PART"
    hdr[8:12] = (0x00010000).to_bytes(4, "little")
    hdr[12:16] = (92).to_bytes(4, "little")
    hdr[24:32] = (1).to_bytes(8, "little")
    hdr[32:40] = (total_sectors - 1).to_bytes(8, "little")
    hdr[40:48] = (34).to_bytes(8, "little")
    hdr[48:56] = (total_sectors - 34).to_bytes(8, "little")
    hdr[72:80] = (2).to_bytes(8, "little")      # entries LBA
    hdr[80:84] = (128).to_bytes(4, "little")
    hdr[84:88] = (128).to_bytes(4, "little")

    entries = bytearray(128 * 128)
    e = entries
    e[0:16] = _guid_basic_data()
    e[32:40] = start_lba.to_bytes(8, "little")
    e[40:48] = (start_lba + sectors - 1).to_bytes(8, "little")
    utf16 = name.encode("utf-16-le")
    e[56:56 + len(utf16)] = utf16
    return pmbr, hdr, entries


def build_fat32(path, gpt=False):
    total = 64 * 1024 * 1024
    part_start = 2048
    part_sectors = total // 512 - part_start
    spc, reserved, nfats, spf = 1, 32, 2, 1024
    root_cluster = 2
    fat_start = part_start + reserved
    data_start = fat_start + nfats * spf

    img = bytearray(total)
    if gpt:
        pmbr, hdr, entries = _gpt(total // 512, part_start, part_sectors,
                                  "MUOSFAT")
        img[0:512] = pmbr
        img[512:1024] = hdr
        img[1024:1024 + len(entries)] = entries
    else:
        img[0:512] = _mbr(0x0C, part_start, part_sectors)

    bs = bytearray(512)
    bs[0:3] = bytes([0xEB, 0x58, 0x90])
    bs[3:11] = b"MUOSFAT "
    bs[11:13] = (512).to_bytes(2, "little")
    bs[13] = spc
    bs[14:16] = reserved.to_bytes(2, "little")
    bs[16] = nfats
    bs[17:19] = (0).to_bytes(2, "little")
    bs[19:21] = (0).to_bytes(2, "little")
    bs[21] = 0xF8
    bs[22:24] = (0).to_bytes(2, "little")
    bs[24:26] = (32).to_bytes(2, "little")
    bs[26:28] = (64).to_bytes(2, "little")
    bs[28:32] = part_start.to_bytes(4, "little")
    bs[32:36] = part_sectors.to_bytes(4, "little")
    bs[36:40] = spf.to_bytes(4, "little")
    bs[40:44] = (0).to_bytes(4, "little")
    bs[44:48] = root_cluster.to_bytes(4, "little")
    bs[48:50] = (1).to_bytes(2, "little")      # FSInfo
    bs[50:52] = (6).to_bytes(2, "little")      # backup boot sector
    bs[64] = 0x80
    bs[66] = 0x29
    bs[67:71] = (0x12345678).to_bytes(4, "little")
    bs[71:82] = b"MUOSFAT32  "
    bs[82:90] = b"FAT32   "
    bs[510], bs[511] = 0x55, 0xAA
    part_byte = part_start * 512
    img[part_byte:part_byte + 512] = bs

    # FSInfo
    fs = bytearray(512)
    fs[0:4] = (0x41615252).to_bytes(4, "little")
    fs[484:488] = (0x61417272).to_bytes(4, "little")
    fs[488:492] = (0xFFFFFFFF).to_bytes(4, "little")
    fs[492:496] = (0xFFFFFFFF).to_bytes(4, "little")
    fs[508:512] = (0xAA550000).to_bytes(4, "little")
    img[part_byte + 512:part_byte + 1024] = fs

    # FAT: root(2), HELLO(3), SUBDIR(4), INNER(5), LONGFILE(6)
    fat = bytearray(spf * 512)
    fat[0:4] = (0x0FFFFFF8).to_bytes(4, "little")
    fat[4:8] = (0x0FFFFFFF).to_bytes(4, "little")
    for c in (2, 3, 4, 5, 6):
        fat[c * 4:c * 4 + 4] = (0x0FFFFFFF).to_bytes(4, "little")
    img[fat_start * 512:fat_start * 512 + len(fat)] = fat
    img[(fat_start + spf) * 512:(fat_start + spf) * 512 + len(fat)] = fat

    hello = b"FAT-HELLO-OK\n"
    inner = b"FAT-SUB-OK\n"
    longf = b"FAT-LONG-OK\n"

    root = bytearray(512)
    root[0:32] = _entry("MUOSFAT32  ", 0x08, 0, 0)
    root[32:64] = _entry("HELLO   TXT", 0x20, 3, len(hello))
    root[64:96] = _entry("SUBDIR     ", 0x10, 4, 0)
    root[96:128] = _lfn("LONGFILE.TXT")
    root[128:160] = _entry("LONGFI~1TXT", 0x20, 6, len(longf))
    img[data_start * 512:data_start * 512 + 512] = root

    sub = bytearray(512)
    sub[0:32] = _entry(".          ", 0x10, 4, 0)
    sub[32:64] = _entry("..         ", 0x10, 2, 0)
    sub[64:96] = _entry("INNER   TXT", 0x20, 5, len(inner))
    img[(data_start + 2) * 512:(data_start + 2) * 512 + 512] = sub

    img[(data_start + 1) * 512:(data_start + 1) * 512 + len(hello)] = hello
    img[(data_start + 3) * 512:(data_start + 3) * 512 + len(inner)] = inner
    img[(data_start + 4) * 512:(data_start + 4) * 512 + len(longf)] = longf

    with open(path, "wb") as f:
        f.write(img)
    return path


def _ext2_inode(buf, off, mode, size, block0, links=1, blocks=2):
    buf[off + 0:off + 2] = mode.to_bytes(2, "little")
    buf[off + 4:off + 8] = size.to_bytes(4, "little")
    buf[off + 24:off + 26] = (0).to_bytes(2, "little")
    buf[off + 26:off + 28] = links.to_bytes(2, "little")
    buf[off + 28:off + 32] = blocks.to_bytes(4, "little")
    buf[off + 40:off + 44] = block0.to_bytes(4, "little")


def _ext2_dir(buf, entries):
    off = 0
    for i, (ino, name) in enumerate(entries):
        rec = 8 + len(name)
        rec = (rec + 3) & ~3
        if i == len(entries) - 1:
            rec = 1024 - off
        buf[off + 0:off + 4] = ino.to_bytes(4, "little")
        buf[off + 4:off + 6] = rec.to_bytes(2, "little")
        buf[off + 6:off + 8] = len(name).to_bytes(2, "little")
        buf[off + 8:off + 8 + len(name)] = name.encode("ascii")
        off += rec


def build_ext2(path):
    part_start = 2048
    part_sectors = 8192                  # 4MB
    total = (part_start + part_sectors) * 512
    img = bytearray(total)
    img[0:512] = _mbr(0x83, part_start, part_sectors)
    base = part_start * 512

    block_size = 1024
    blocks = 4096
    sb = bytearray(block_size)
    sb[0:4] = (32).to_bytes(4, "little")          # inodes_count
    sb[4:8] = blocks.to_bytes(4, "little")
    sb[12:16] = (blocks - 13).to_bytes(4, "little")
    sb[16:20] = (32 - 5).to_bytes(4, "little")
    sb[20:24] = (1).to_bytes(4, "little")         # first_data_block
    sb[24:28] = (0).to_bytes(4, "little")         # log_block_size
    sb[28:32] = (0).to_bytes(4, "little")
    sb[32:36] = blocks.to_bytes(4, "little")      # blocks_per_group
    sb[36:40] = blocks.to_bytes(4, "little")
    sb[40:44] = (32).to_bytes(4, "little")        # inodes_per_group
    sb[48:52] = (0x12345678).to_bytes(4, "little")
    sb[56:58] = (0xEF53).to_bytes(2, "little")
    sb[58:60] = (1).to_bytes(2, "little")
    sb[76:80] = (0).to_bytes(4, "little")         # rev 0
    sb[88:90] = (128).to_bytes(2, "little")       # inode size
    sb[120:136] = b"MUOS-EXT2      "
    img[base + 1024:base + 1024 + block_size] = sb

    gd = bytearray(block_size)
    gd[0:4] = (3).to_bytes(4, "little")           # block bitmap
    gd[4:8] = (4).to_bytes(4, "little")           # inode bitmap
    gd[8:12] = (5).to_bytes(4, "little")          # inode table
    gd[12:14] = (blocks - 13).to_bytes(2, "little")
    gd[14:16] = (32 - 5).to_bytes(2, "little")
    img[base + 2 * block_size:base + 3 * block_size] = gd

    bbm = bytearray(block_size)
    for b in range(0, 13):
        bbm[b // 8] |= 1 << (b % 8)
    img[base + 3 * block_size:base + 4 * block_size] = bbm

    ibm = bytearray(block_size)
    for i in range(0, 5):
        ibm[i // 8] |= 1 << (i % 8)
    img[base + 4 * block_size:base + 5 * block_size] = ibm

    itable = bytearray(4 * block_size)            # blocks 5..8, 32 inodes
    _ext2_inode(itable, 1 * 128, 0x41ED, 1024, 9, links=2)
    _ext2_inode(itable, 2 * 128, 0x81A4, 13, 10)
    _ext2_inode(itable, 3 * 128, 0x41ED, 1024, 11, links=2)
    _ext2_inode(itable, 4 * 128, 0x81A4, 13, 12)
    img[base + 5 * block_size:base + 9 * block_size] = itable

    root = bytearray(block_size)
    _ext2_dir(root, [(2, "."), (2, ".."), (3, "hello.txt"), (4, "subdir")])
    img[base + 9 * block_size:base + 10 * block_size] = root

    hello = b"EXT2-READ-OK\n"
    img[base + 10 * block_size:base + 10 * block_size + len(hello)] = hello

    sub = bytearray(block_size)
    _ext2_dir(sub, [(4, "."), (2, ".."), (5, "inside.txt")])
    img[base + 11 * block_size:base + 12 * block_size] = sub

    inner = b"EXT2-SUB-OK\n"
    img[base + 12 * block_size:base + 12 * block_size + len(inner)] = inner

    with open(path, "wb") as f:
        f.write(img)
    return path


def main():
    os.makedirs(BUILD, exist_ok=True)
    print("fat32:", build_fat32(os.path.join(BUILD, "fat32.img")))
    print("ext2 :", build_ext2(os.path.join(BUILD, "ext2.img")))


if __name__ == "__main__":
    main()
