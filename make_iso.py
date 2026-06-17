# make_iso.py - Build bootable MuOS ISO (custom bootloader)
import os, struct, subprocess, errno, math

PROJECT = r"C:\Users\wuxiaomu\Desktop\muos"
SRC     = os.path.join(PROJECT, "src")
TOOLS   = os.path.join(PROJECT, "tools", "nasm")
NASM    = os.path.join(TOOLS, "nasm.exe")
GCC_BIN = os.path.join(PROJECT, "toolchain", "bin")
OBJCOPY = os.path.join(GCC_BIN, "i686-elf-objcopy.exe")
BUILD   = os.path.join(PROJECT, "build")
ISO_OUT = os.path.join(PROJECT, "muos.iso")

BOOTBIN  = os.path.join(BUILD, "boot_cd.bin")
KERNBIN  = os.path.join(BUILD, "kernel.bin")
COMBINED = os.path.join(BUILD, "boot.img")

# 1. Compile boot_cd.s → flat binary
subprocess.run([NASM, "-f", "bin", "-o", BOOTBIN,
                os.path.join(SRC, "boot_cd.s")], check=True)

# 2. kernel.elf → flat binary
subprocess.run([OBJCOPY, "-O", "binary",
                os.path.join(BUILD, "kernel.elf"), KERNBIN], check=True)

kern_size = os.path.getsize(KERNBIN)
print(f"kernel.bin: {kern_size} bytes")

# 3. Patch kernel size into bootloader at offset 4092
with open(BOOTBIN, "rb") as f:
    boot_data = bytearray(f.read())
struct.pack_into("<I", boot_data, 4092, kern_size)

# 4. Combine bootloader + kernel flat binary
with open(KERNBIN, "rb") as f:
    kern_data = f.read()

combined = bytes(boot_data) + kern_data
with open(COMBINED, "wb") as f:
    f.write(combined)
print(f"boot.img: {len(combined)} bytes")

# 5. Build ISO
import pycdlib
iso = pycdlib.PyCdlib()
iso.new(interchange_level=1, sys_ident="MUOS", vol_ident="MUOS_BOOT")

boot_sectors = math.ceil(len(combined) / 512)
print(f"boot sectors: {boot_sectors}")

iso.add_file(COMBINED, "/BOOT.IMG")
iso.add_eltorito("/BOOT.IMG", bootcatfile="/BOOT.CAT",
                 boot_load_size=boot_sectors, boot_info_table=True)

# Write (handle locked file)
try:
    os.remove(ISO_OUT)
except OSError as e:
    if e.errno == errno.EACCES:
        tmp = ISO_OUT + ".tmp"
        iso.write(tmp)
        iso.close()
        os.replace(tmp, ISO_OUT)
        print(f"muos.iso created ({os.path.getsize(ISO_OUT)} bytes)")
        exit(0)
    elif e.errno != errno.ENOENT:
        raise

iso.write(ISO_OUT)
iso.close()
print(f"muos.iso created ({os.path.getsize(ISO_OUT)} bytes)")
