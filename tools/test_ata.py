"""test_ata.py - two-boot ATA/MuFS persistence check.

Boot 1 writes a MuFS file (`run fsata`) and a raw sector (`run ata`).
Boot 2 (`run fsread`, `run ataread`) must still find both.
"""
import os
import sys
from qmp_shell import Shell, qemu_path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
KERNEL = os.path.join(BUILD, "kernel.elf")
DISK = os.path.join(BUILD, "ata_disk.img")


def boot(commands, port, tag):
    log = os.path.join(BUILD, f"ata_{tag}.log")
    args = [qemu_path(), "-kernel", KERNEL,
            "-drive", f"file={DISK},format=raw,if=ide",
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
    os.makedirs(BUILD, exist_ok=True)
    if not os.path.exists(DISK):
        with open(DISK, "wb") as f:
            f.truncate(8 * 1024 * 1024)

    t1 = boot(["run fsata", "run ata"], 4466, "boot1")
    t2 = boot(["run fsread", "run ataread"], 4467, "boot2")

    checks = {
        "boot1 FS-PERSIST-OK": "FS-PERSIST-OK" in t1,
        "boot1 ATA-OK": "ATA-OK" in t1,
        "boot2 FS-PERSIST-OK": "FS-PERSIST-OK" in t2,
        "boot2 ATA-PERSIST-OK": "ATA-PERSIST-OK" in t2,
    }
    for k, v in checks.items():
        print(f"{k}: {v}")
    print("RESULT:", "PASS" if all(checks.values()) else "FAIL")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
