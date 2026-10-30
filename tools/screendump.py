"""screendump.py - boot the ISO, open the pixel desktop and capture the screen.

Usage: python tools/screendump.py [build/pixel.ppm]
Requires muos.iso (build with `build.ps1 -Iso`).
"""
import os
import sys
import time
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
ISO = os.path.join(ROOT, "muos.iso")
LOG = os.path.join(BUILD, "screendump.log")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "pixel.ppm")
    args = [qemu_path(), "-cdrom", ISO, "-m", "256M",
            "-nic", "user,model=rtl8139",
            "-display", "none", "-serial", f"file:{LOG}",
            "-qmp", "tcp:127.0.0.1:4477,server,nowait",
            "-no-reboot", "-no-shutdown"]
    sh = Shell(args, LOG, 4477)
    try:
        sh.start()
        sh.type("pixel\n")
        time.sleep(1.5)
        sh._cmd({"execute": "screendump", "arguments": {"filename": out}})
    finally:
        sh.close()

    data = open(out, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    nonblack = sum(1 for i in range(0, len(pix) - 2, 3)
                   if pix[i] or pix[i + 1] or pix[i + 2])
    print(f"{out}: {w}x{h}, non-black pixels: {nonblack}")


if __name__ == "__main__":
    main()
