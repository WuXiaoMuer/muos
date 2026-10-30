"""test_fs_read.py - boot MuOS against generated FAT32 and ext2 images.

Uses tools/make_fs_images.py to build MBR-partitioned images, then drives
the shell through QMP:
  fat32: part / fatls / fatcat / long-file / subdir
  ext2 : part / ext2ls / ext2cat / subdir
"""
import os
import sys
from qmp_shell import Shell, qemu_path
import make_fs_images

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
KERNEL = os.path.join(BUILD, "kernel.elf")


def boot(image, commands, port, tag):
    log = os.path.join(BUILD, f"fsread_{tag}.log")
    args = [qemu_path(), "-kernel", KERNEL,
            "-drive", f"file={image},format=raw,if=ide",
            "-m", "256M", "-nic", "user,model=rtl8139",
            "-display", "none", "-serial", f"file:{log}",
            "-qmp", f"tcp:127.0.0.1:{port},server,nowait",
            "-no-reboot", "-no-shutdown"]
    sh = Shell(args, log, port)
    try:
        sh.start()
        for cmd in commands:
            sh.run(cmd)
    finally:
        sh.close()
    return sh.text()


def main():
    fat = make_fs_images.build_fat32(os.path.join(BUILD, "fat32.img"))
    gpt = make_fs_images.build_fat32(os.path.join(BUILD, "fat32_gpt.img"),
                                     gpt=True)
    ext = make_fs_images.build_ext2(os.path.join(BUILD, "ext2.img"))

    tf = boot(fat, ["part", "fatls /", "fatcat /HELLO.TXT",
                    "fatcat /LONGFILE.TXT", "fatls /SUBDIR",
                    "fatcat /SUBDIR/INNER.TXT"], 4477, "fat")
    tg = boot(gpt, ["part", "fatcat /HELLO.TXT"], 4478, "gpt")
    te = boot(ext, ["part", "ext2ls /", "ext2cat /hello.txt",
                    "ext2ls /subdir", "ext2cat /subdir/inside.txt"],
              4479, "ext2")

    checks = {
        "fat part table": "[part] table ok" in tf,
        "fat ls root": "D SUBDIR" in tf and "HELLO.TXT" in tf,
        "fat read": "FAT-HELLO-OK" in tf,
        "fat long name": "FAT-LONG-OK" in tf,
        "fat subdir": "FAT-SUB-OK" in tf,
        "fat markers": tf.count("[fat] ls ok") >= 2 and
                       tf.count("[fat] cat ok") >= 3,
        "gpt partition": "GPT" in tg and "[part] table ok" in tg,
        "gpt fat read": "FAT-HELLO-OK" in tg,
        "ext2 part table": "[part] table ok" in te,
        "ext2 ls root": "hello.txt" in te and "subdir" in te,
        "ext2 read": "EXT2-READ-OK" in te,
        "ext2 subdir": "EXT2-SUB-OK" in te,
        "ext2 markers": te.count("[ext2] ls ok") >= 2 and
                        te.count("[ext2] cat ok") >= 2,
    }
    for k, v in checks.items():
        print(f"{k}: {v}")
    print("RESULT:", "PASS" if all(checks.values()) else "FAIL")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
