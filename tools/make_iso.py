"""make_iso.py - build a bootable El Torito ISO for MuOS.

The boot image is the raw boot_cd.bin (assembled with `nasm -f bin`) with
the flat kernel appended at offset 4096. The kernel size is patched into the
dword at offset 4092, which boot_cd.s reads before copying the kernel to
0x100000. pycdlib writes the El Torito boot catalog and Boot Info Table
(bytes 8-63 are reserved by the loader for it).
"""
import os
import struct
import subprocess
import sys
import pycdlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def find_tool(name, extra):
    for p in extra:
        if os.path.exists(p):
            return p
    from shutil import which
    got = which(name)
    if got:
        return got
    raise SystemExit(f"{name} not found")


NASM = find_tool("nasm", [
    os.path.join(ROOT, "tools", "nasm", "nasm.exe"),
    r"C:\Program Files\NASM\nasm.exe",
    r"C:\Program Files (x86)\NASM\nasm.exe",
])
OBJCOPY = find_tool("i686-elf-objcopy", [
    os.path.join(ROOT, "toolchain", "bin", "i686-elf-objcopy.exe"),
])


def run(args):
    subprocess.run(args, check=True, cwd=ROOT)


def main():
    os.makedirs(BUILD, exist_ok=True)
    boot_bin = os.path.join(BUILD, "boot_cd.bin")
    kernel_flat = os.path.join(BUILD, "kernel_flat.bin")
    image_path = os.path.join(BUILD, "boot_image.bin")
    iso_path = os.path.join(ROOT, "muos.iso")

    run([NASM, "-f", "bin", os.path.join(ROOT, "src", "boot_cd.s"),
         "-o", boot_bin])
    run([OBJCOPY, "-O", "binary", os.path.join(BUILD, "kernel.elf"),
         kernel_flat])

    with open(boot_bin, "rb") as f:
        boot = bytearray(f.read())
    with open(kernel_flat, "rb") as f:
        kernel = f.read()

    if len(boot) < 4096:
        boot += b"\x00" * (4096 - len(boot))
    boot = boot[:4096]
    struct.pack_into("<I", boot, 4092, len(kernel))
    payload = bytes(boot) + kernel

    with open(image_path, "wb") as f:
        f.write(payload)

    if os.path.exists(iso_path):
        os.remove(iso_path)
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="MUOS")
    iso.add_file(image_path, iso_path="/BOOT.BIN")
    iso.add_eltorito("/BOOT.BIN", bootcatfile="/BOOT.CAT",
                     boot_info_table=True, media_name="noemul")
    iso.write(iso_path)
    iso.close()
    print(f"muos.iso created ({os.path.getsize(iso_path)} bytes)")


if __name__ == "__main__":
    main()
