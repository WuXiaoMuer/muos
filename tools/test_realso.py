"""test_realso.py - real musl shared-library end-to-end check.

Boots MuOS with three Multiboot modules:
  build/alpine/files/bin/busybox             (dynamic Alpine BusyBox)
  build/alpine/files/lib/ld-musl-i386.so.1   (real musl loader)
  build/alpine/files/lib/libc.musl-x86.so.1  (real musl libc copy)

The kernel resolves PT_INTERP through /lib/<module name>, the musl loader
opens /lib/libc.musl-x86.so.1 through the module-backed VFS, and BusyBox runs
an applet. See README for how to extract these files from Alpine APKs.
"""
import os
import sys
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
KERNEL = os.path.join(BUILD, "kernel.elf")
FILES = [
    os.path.join(BUILD, "alpine", "files", "bin", "busybox"),
    os.path.join(BUILD, "alpine", "files", "lib", "ld-musl-i386.so.1"),
    os.path.join(BUILD, "alpine", "files", "lib", "libc.musl-x86.so.1"),
]


def main():
    missing = [p for p in FILES if not os.path.exists(p)]
    if missing:
        print("SKIP: missing", ", ".join(missing))
        print("See README (real musl .so) for the Alpine APK extraction steps.")
        return 0

    log = os.path.join(BUILD, "realso.log")
    args = [qemu_path(), "-kernel", KERNEL,
            "-initrd", ",".join(FILES),
            "-m", "256M", "-nic", "user,model=rtl8139",
            "-display", "none", "-serial", f"file:{log}",
            "-qmp", "tcp:127.0.0.1:4493,server,nowait",
            "-no-reboot", "-no-shutdown"]
    sh = Shell(args, log, 4493)
    try:
        sh.start()
        sh.run("mods")
        sh.run("execmod 0 echo REAL-SO-OK")
        sh.run("execmod 0 uname -a")
        sh.run("execmod 0 ls /")
    finally:
        sh.close()

    txt = sh.text()
    checks = {
        "kernel self-tests": "88 passed, 0 failed" in txt,
        "three modules": "[mods] count=3" in txt,
        "real loader entry": "entry=0x08" in txt and "size=820500" in txt,
        "musl applet echo": "REAL-SO-OK" in txt,
        "musl applet uname": "Linux" in txt or "GNU/Linux" in txt or "musl" in txt,
        "musl applet ls": "notes.txt" in txt or "hello.elf" in txt,
        "clean exit": "[user] exit code=0" in txt,
    }
    for k, v in checks.items():
        print(f"{k}: {v}")
    print("RESULT:", "PASS" if all(checks.values()) else "FAIL")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
