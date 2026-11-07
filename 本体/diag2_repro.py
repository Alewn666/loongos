#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""控制变量：与 test_install 完全同款 QEMU 配置（同 SER/VARS/空白盘），开机后抓 repr。
乱码复现 → 确定性，可二分；干净 → 随机性（竞态/内存破坏）。"""
import os, sys, time, socket, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release

BASE = os.path.dirname(os.path.abspath(__file__))
DISK = os.path.join(BASE, "build", "inst_disk.img")
SER = "/tmp/inst.ser"
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
VARS = "/tmp/inst_vars.fd"          # 与 test_install 同一个 VARS
ISO = os.path.join(BASE, "iso", "loongos.iso")
SIZE_MB = 64
if not preflight(need_mb=128, label="diag2_repro.py"):
    sys.exit(2)


for p in (SER,):
    try: os.unlink(p)
    except (FileNotFoundError, PermissionError): pass
if not os.path.exists(VARS):
    import shutil
    shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)
cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3",
       "-accel", "kvm", "-cpu", "host", "-m", "128", "-smp", "1",
       "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
       "-drive", "if=pflash,format=raw,file=" + VARS,
       "-serial", "unix:%s,server,nowait" % SER,
       "-display", "none",
       "-drive", "if=none,id=d1,format=raw,file=" + DISK,
       "-device", "virtio-blk-pci,drive=d1",
       "-cdrom", ISO]
err = open("/tmp/inst_qemu.err", "wb")
p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
s = None
for _ in range(60):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SER); break
    except OSError:
        s = None; time.sleep(0.1)
s.settimeout(0.2)
buf = ""
end = time.time() + 90
while time.time() < end and "[shell]" not in buf:
    try:
        d = s.recv(65536)
        if not d: break
        buf += d.decode("utf-8", "replace")
    except socket.timeout:
        pass
print("进 shell:", "[shell]" in buf)
print("=== repr 开机串口（前 1500 字节）===")
print(repr(buf[:1500]))
print("=== 判定 ===")
print("出现字面问号串 '????':", "????" in buf)
try: p.terminate(); p.wait(timeout=5)
except Exception: p.kill()
s.close(); err.close()
try: os.unlink(SER)
except Exception: pass
release()
