#!/usr/bin/env python3
"""崩了之后从 QEMU 侧 dump 页表，验证「DMA 把页表冲了」这个推断。

  l2_dev[128] @ 0x400b0400 管 0x10000000-0x101FFFFF（xHCI BAR 所在块）
  正常值应为 0x0060000000100705（VALID|AF|SH_INNER|Device-nGnRE|PXN|UXN）
  如果被改成别的值 → 页表确实被踩了；如果完好 → 我的推断错了，得换方向。

用法：sudo python3 probe_pte.py
"""
import os, socket, subprocess, sys, time

SOCK = "/tmp/pte_mon.sock"
SER = "/tmp/pte_serial.log"
EXPECT = 0x00A0000000000705 & 0  # 占位，下面按位算
# PROT_DEV_BLK = VALID(1) | AF(1<<10) | SH_INNER(3<<8) | ATTR_DEV(1<<2) | PXN(1<<53) | UXN(1<<54)
PROT_DEV_BLK = 1 | (3 << 8) | (1 << 10) | (1 << 2) | (1 << 53) | (1 << 54)
EXPECT_L2DEV_128 = 0x10000000 | PROT_DEV_BLK

DUMPS = [
    ("l2_dev[128]  (管 xHCI BAR 块)", 0x400b0400, 8),
    ("l2_dev[124..131] 邻域",          0x400b03e0, 8),
    ("l2_dev[0..3]",                  0x400b0000, 4),
    ("l1_table[0]",                   0x400b1000, 2),
    ("l0_table[0]",                   0x400b2000, 2),
]


def read_until_prompt(s, timeout=0.6):
    buf = b""
    s.settimeout(timeout)
    try:
        while True:
            d = s.recv(16384)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    return buf.decode(errors="replace")


def main():
    for p in (SOCK, SER):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    here = os.path.dirname(os.path.abspath(__file__))
    cmd = ["qemu-system-aarch64", "-accel", "kvm", "-machine", "virt,gic-version=3",
           "-cpu", "host", "-m", "128", "-kernel", os.path.join(here, "kernel.elf"),
           "-display", "none", "-serial", "file:" + SER,
           "-monitor", "unix:%s,server,nowait" % SOCK,
           "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0"]
    p = subprocess.Popen(cmd)
    try:
        time.sleep(12)          # 等它崩过几轮（每轮自愈后继续，日志里能看到）
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
        time.sleep(0.4)
        read_until_prompt(s)

        print("=== 期望 l2_dev[128] = 0x%016x ===" % EXPECT_L2DEV_128)
        for name, addr, n in DUMPS:
            s.sendall(("xp /%dgx 0x%x\n" % (n, addr)).encode())
            time.sleep(0.5)
            txt = read_until_prompt(s)
            vals = []
            for line in txt.splitlines():
                line = line.replace("\x1b[K", "").replace("\x1b[D", "")
                if ":" in line and len(line.split(":")[0].strip()) >= 8:
                    vals += [v for v in line.split(":", 1)[1].split() if v.startswith("0x")]
            print("  %-32s @0x%08x → %s" % (name, addr, " ".join(vals[:n]) or "<无数据>"))
        s.close()
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        try:
            os.unlink(SOCK)
        except FileNotFoundError:
            pass
        print("\n=== 串口异常次数: %d ===" % open(SER, encoding="utf-8", errors="replace").read().count("内核异常"))


main()
