#!/usr/bin/env python3
"""用 QEMU monitor 的 `xp` 从 QEMU 侧直接读 guest 物理地址，
判定 0x10100440（xHCI PORTSC）到底是「QEMU 没这个内存区域」还是「guest 自己翻译就炸」。

  xp 读的是 QEMU 的 system memory 视图 —— 有区域就返回真值，没区域返回 0（unassigned）。
  而 guest 侧的 ldr 若被 QEMU 当 unassigned 处理，会注入 abort（我们看到的那颗）。

用法：python3 probe_mmio.py [--kvm]
"""
import os, socket, subprocess, sys, time

SOCK = "/tmp/probe_mon.sock"
SER = "/tmp/probe_serial.log"
KVM = "--kvm" in sys.argv

def main():
    for p in (SOCK, SER):
        try: os.unlink(p)
        except FileNotFoundError: pass
    cmd = ["qemu-system-aarch64",
           "-machine", "virt,gic-version=%s" % ("3" if KVM else "2"),
           "-cpu", "host" if KVM else "cortex-a57",
           "-m", "128", "-kernel", os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel.elf"),
           "-display", "none", "-serial", "file:" + SER,
           "-monitor", "unix:%s,server,nowait" % SOCK,
           "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0"]
    if KVM:
        cmd[1:1] = ["-accel", "kvm"]
    p = subprocess.Popen(cmd)
    try:
        time.sleep(9)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
        s.settimeout(5)
        time.sleep(0.5)
        s.recv(65536)  # banner
        addrs = [
            ("BAR+0x000  capability", 0x10100000),
            ("BAR+0x040  USBCMD(oper)", 0x10100040),
            ("BAR+0x440  PORTSC port1", 0x10100440),
            ("BAR+0x448  PORTSC port2", 0x10100448),
            ("BAR+0x480  PORTSC 之后", 0x10100480),
        ]
        print("=== QEMU 侧 xp 读 guest 物理地址（KVM=%s）===" % KVM)
        for name, a in addrs:
            s.sendall(("xp /4wx 0x%x\n" % a).encode())
            time.sleep(0.5)
            txt = b""
            s.settimeout(0.4)
            try:
                while True:
                    d = s.recv(8192)
                    if not d:
                        break
                    txt += d
            except socket.timeout:
                pass
            s.settimeout(5)
            lines = txt.decode(errors="replace").splitlines()
            data = [l.strip() for l in lines if ":" in l and l.strip().startswith("0x")]
            print("  %-24s 0x%08x → %s" % (name, a, data[0] if data else repr(lines[:3])))
        s.close()
    finally:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass

main()
