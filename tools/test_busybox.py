"""test_busybox.py - drive the MuOS kernel shell through QMP.

Usage:
    python tools/test_busybox.py "run net" "run tcp" ...

Boots build/kernel.elf (adding the optional build/busybox initrd when it
exists), waits for the shell, types each command, and prints the serial tail.
"""
import os
import sys
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LOG = os.path.join(BUILD, "busybox_test.log")


def main():
    cmds = sys.argv[1:]
    if not cmds:
        print("usage: test_busybox.py <command> [command...]")
        return 2

    args = [qemu_path(), "-kernel", os.path.join(BUILD, "kernel.elf")]
    busybox = os.path.join(BUILD, "busybox")
    if os.path.exists(busybox):
        args += ["-initrd", busybox]
    args += ["-m", "256M", "-nic", "user,model=rtl8139",
             "-display", "none", "-serial", f"file:{LOG}",
             "-qmp", "tcp:127.0.0.1:4455,server,nowait",
             "-no-reboot", "-no-shutdown"]

    sh = Shell(args, LOG, 4455)
    try:
        sh.start()
        for cmd in cmds:
            sh.run(cmd)
            print(f"[cmd] {cmd} done")
    finally:
        sh.close()

    print(sh.text()[-6000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
