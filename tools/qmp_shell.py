"""qmp_shell.py - helper to boot MuOS in QEMU and drive its shell over QMP."""
import json
import os
import socket
import subprocess
import time

# QEMU qcode per ASCII character. Chords are used for shifted characters.
_KEYS = {"\n": ["ret"], " ": ["spc"], "-": ["minus"], "=": ["equal"],
         "[": ["bracketleft"], "]": ["bracketright"], "\\": ["backslash"],
         ";": ["semicolon"], "'": ["apostrophe"], ",": ["comma"],
         ".": ["dot"], "/": ["slash"], "`": ["grave"],
         "_": ["shift", "minus"], "+": ["shift", "equal"],
         "{": ["shift", "bracketleft"], "}": ["shift", "bracketright"],
         "|": ["shift", "backslash"], ":": ["shift", "semicolon"],
         '"': ["shift", "apostrophe"], "<": ["shift", "comma"],
         ">": ["shift", "dot"], "?": ["shift", "slash"], "~": ["shift", "grave"],
         "!": ["shift", "1"], "@": ["shift", "2"], "#": ["shift", "3"],
         "$": ["shift", "4"], "%": ["shift", "5"], "^": ["shift", "6"],
         "&": ["shift", "7"], "*": ["shift", "8"], "(": ["shift", "9"],
         ")": ["shift", "0"]}
for _c in "abcdefghijklmnopqrstuvwxyz":
    _KEYS[_c] = [_c]
    _KEYS[_c.upper()] = ["shift", _c]
for _d in "1234567890":
    _KEYS[_d] = [_d]


def qemu_path():
    env = os.environ.get("QEMU")
    if env and os.path.exists(env):
        return env
    for p in (r"X:\qemu\qemu-system-i386.exe",
              r"C:\qemu\qemu-system-i386.exe",
              r"C:\Program Files\qemu\qemu-system-i386.exe"):
        if os.path.exists(p):
            return p
    return "qemu-system-i386"


class Shell:
    def __init__(self, qemu_args, log_path, port):
        self.args = qemu_args
        self.log_path = log_path
        self.port = port
        self.proc = None
        self.qmp = None

    def text(self):
        try:
            return open(self.log_path, errors="replace").read()
        except FileNotFoundError:
            return ""

    def wait_for(self, needle, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.text():
                return True
            time.sleep(0.2)
        return False

    def start(self, ready="[shell] ready", timeout=120):
        open(self.log_path, "w").close()
        self.proc = subprocess.Popen(self.args, cwd=os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        sock = None
        for _ in range(40):
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), 2)
                break
            except OSError:
                time.sleep(0.3)
        if sock is None:
            raise RuntimeError("QMP connection failed")
        self.qmp = sock.makefile("rw", encoding="utf-8", newline="\n")
        json.loads(self.qmp.readline())
        self._cmd({"execute": "qmp_capabilities"})
        if not self.wait_for(ready, timeout):
            raise RuntimeError("shell did not start:\n" + self.text()[-3000:])
        return self

    def _cmd(self, obj):
        self.qmp.write(json.dumps(obj) + "\n")
        self.qmp.flush()
        try:
            return json.loads(self.qmp.readline())
        except Exception:
            return {}

    def type(self, text):
        for ch in text:
            keys = _KEYS.get(ch)
            if keys is None:
                continue
            self._cmd({"execute": "send-key", "arguments": {
                "keys": [{"type": "qcode", "data": q} for q in keys]}})
            time.sleep(0.05)

    def run(self, cmd, timeout=None):
        """Type a command and wait for the user exit/fault marker (run/exec)
        or a short settle time for plain shell commands."""
        before = self.text().count("[user] exit code=")
        before_fault = self.text().count("[user] fault")
        self.type(cmd + "\n")
        is_run = cmd.split()[0] in ("run", "exec")
        end = time.time() + (timeout or (180 if is_run else 3))
        while time.time() < end:
            text = self.text()
            if text.count("[user] exit code=") > before:
                break
            if text.count("[user] fault") > before_fault:
                break
            if not is_run and time.time() > end - 1.0:
                break
            time.sleep(0.2)
        time.sleep(0.15)
        return self.text()

    def wait_count(self, needle, minimum, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if self.text().count(needle) >= minimum:
                return True
            time.sleep(0.2)
        return False

    def close(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except Exception:
                self.proc.kill()
        self.proc = None
