"""test_e1000.py - boot MuOS with an Intel E1000 NIC and drive the network stack.

Runs `run net` (ICMP + UDP/DNS), `run tcp` (TCP echo) and `run baccept`
(blocking accept) with QEMU `-nic user,model=e1000`, then checks the
driver banner and the NET/TCP/BACC markers in the serial log.
"""
import os
import sys
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LOG = os.path.join(BUILD, "e1000_test.log")

MARKS = ("E1000 initialized", "NET-OK", "TCP-OK", "BACC-OK")


def main():
    args = [qemu_path(), "-kernel", os.path.join(BUILD, "kernel.elf"),
            "-m", "256M", "-nic", "user,model=e1000",
            "-display", "none", "-serial", f"file:{LOG}",
            "-qmp", "tcp:127.0.0.1:4456,server,nowait",
            "-no-reboot", "-no-shutdown"]

    sh = Shell(args, LOG, 4456)
    try:
        sh.start()
        for cmd in ("run net", "run tcp", "run baccept"):
            sh.run(cmd)
            print(f"[cmd] {cmd} done")
    finally:
        sh.close()

    text = sh.text()
    print(text[-6000:])
    results = {m: (m in text) for m in MARKS}
    for m, ok in results.items():
        print(f"[check] {m}: {'OK' if ok else 'MISSING'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
