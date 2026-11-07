#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""诊断：同 test_install 的 QEMU 配置下，串口中文为什么变 '?'。
抓原始字节 repr()，看是内核就没发对，还是传输层丢字节。"""
import os, sys, time, socket, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release

BASE = os.path.dirname(os.path.abspath(__file__))
DISK = os.path.join(BASE, "build", "inst_disk.img")
SER = "/tmp/diag.ser"
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
VARS = "/tmp/diag_vars.fd"
if not preflight(128, "diag_serial.py"):
    sys.exit(2)

if not os.path.exists(VARS):
    import shutil
    shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)

cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3", "-cpu", "host", "-accel", "kvm",
       "-m", "128", "-smp", "1",
       "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
       "-drive", "if=pflash,format=raw,file=" + VARS,
       "-serial", "unix:%s,server,nowait" % SER, "-display", "none",
       "-drive", "if=none,id=d1,format=raw,file=" + DISK,
       "-device", "virtio-blk-pci,drive=d1",
       "-cdrom", os.path.join(BASE, "iso", "loongos.iso")]
err = open("/tmp/diag_qemu.err", "wb")
p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
s = None
for _ in range(60):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SER); break
    except OSError:
        s = None; time.sleep(0.1)
s.settimeout(0.2)
buf = b""

def pump(sec):
    global buf
    end = time.time() + sec
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            buf += d
        except socket.timeout:
            pass

try:
    for _ in range(120):
        pump(0.3)
        if b"[shell]" in buf: break
    print("=== 开机串口中文行的原始字节 ===")
    for line in buf.split(b"\n"):
        if b"disk" in line or b"\xe6\xa0" in line or b"fs" in line:
            print(repr(line))
    mark = len(buf)
    s.sendall(b"df\r")
    pump(3)
    print("=== df 的回显（ASCII，应正常）===")
    print(repr(buf[mark:mark+400]))
    mark = len(buf)
    s.sendall(b"whoami\r")
    pump(2)
    print("=== whoami 回显 ===")
    print(repr(buf[mark:mark+200]))
finally:
    try: p.terminate(); p.wait(timeout=5)
    except Exception: p.kill()
    s.close(); err.close()
    try: os.unlink(SER)
    except Exception: pass
release()
