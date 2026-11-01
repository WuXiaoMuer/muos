"""test_wm.py - boot the ISO, open `pixel`, click the Widgets list control.

Verifies the list control selects the clicked row by comparing the highlight
colour of row 0 before and row 4 after the click. Requires muos.iso.
"""
import os
import sys
import time
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
ISO = os.path.join(ROOT, "muos.iso")
LOG = os.path.join(BUILD, "wmtest.log")

SEL = (42, 111, 219)      # selected row background
BG = (26, 32, 42)         # normal row background


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]

    def px(x, y):
        i = (y * w + x) * 3
        return (pix[i], pix[i + 1], pix[i + 2])
    return w, h, px


def close(a, b, tol=6):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def main():
    if not os.path.exists(ISO):
        print("SKIP: muos.iso missing (run build.ps1 -Iso)")
        return 0

    before = os.path.join(BUILD, "wmtest_before.ppm")
    after = os.path.join(BUILD, "wmtest_after.ppm")
    args = [qemu_path(), "-cdrom", ISO, "-m", "256M",
            "-nic", "user,model=rtl8139",
            "-display", "none", "-serial", f"file:{LOG}",
            "-qmp", "tcp:127.0.0.1:4499,server,nowait",
            "-no-reboot", "-no-shutdown"]
    sh = Shell(args, LOG, 4499)

    def ev(events):
        sh._cmd({"execute": "input-send-event", "arguments": {"events": events}})

    try:
        sh.start()
        sh.type("pixel\n")
        time.sleep(1.5)
        # Pointer starts at virtual (320,352) -> screen (400,528).
        # List row 4 centre is screen (300,462): rel x = -40, y = -22.
        ev([{"type": "rel", "data": {"axis": "x", "value": -40}},
            {"type": "rel", "data": {"axis": "y", "value": -22}}])
        time.sleep(0.5)
        sh._cmd({"execute": "screendump", "arguments": {"filename": before}})
        ev([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.2)
        ev([{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.5)
        sh._cmd({"execute": "screendump", "arguments": {"filename": after}})
    finally:
        sh.close()

    w1, h1, pb = read_ppm(before)
    w2, h2, pa = read_ppm(after)
    row0_b, row4_b = pb(300, 388), pb(300, 460)
    row0_a, row4_a = pa(300, 388), pa(300, 460)

    checks = {
        "screen size 800x600": (w1, h1) == (800, 600) and (w2, h2) == (800, 600),
        "row0 selected before": close(row0_b, SEL),
        "row4 not selected before": close(row4_b, BG),
        "row0 cleared after": close(row0_a, BG),
        "row4 selected after": close(row4_a, SEL),
    }
    for k, v in checks.items():
        print(f"{k}: {v}")
    print("RESULT:", "PASS" if all(checks.values()) else "FAIL")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
