#!/usr/bin/env python3
"""启动计时：量「QEMU 启动 → 各里程碑」的真实耗时（KVM / TCG 各跑一次）。

里程碑：第一条串口输出、MMU、fs、usb 就绪、shell 提示符。
用法：python3 bench_boot.py [--kvm]
"""
import os, subprocess, sys, time
import sys
# 宿主资源护栏（内存门槛 + 单实例锁）：NAS 只有 2GB，别把宿主拖死
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release
if not preflight(need_mb=400, label=os.path.basename(__file__)):
    sys.exit(2)

BASE = os.path.dirname(os.path.abspath(__file__))
KVM = "--kvm" in sys.argv
SUF = "_kvm" if KVM else ""
LOG = "/tmp/boot_bench%s.log" % SUF

MARKS = [("第一行输出", "灵珑系统"), ("MMU 开启", "[mmu ] MMU 已开启"),
         ("文件系统", "[fs  ] 内存盘"), ("用户/权限", "[auth]"),
         ("USB 键盘就绪", "USB 键盘就绪"), ("shell 提示符", "[shell]")]


def main():
    try:
        os.unlink(LOG)
    except (FileNotFoundError, PermissionError):
        pass
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=%s" % ("3" if KVM else "2"),
           "-cpu", "host" if KVM else "cortex-a57", "-m", "128",
           "-kernel", os.path.join(BASE, "kernel.elf"), "-display", "none",
           "-serial", "file:" + LOG,
           "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-device", "usb-mouse,bus=xhci.0,port=2",
           "-device", "usb-kbd,bus=xhci.0,port=3"]
    if KVM:
        cmd[1:1] = ["-accel", "kvm"]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        seen = {}
        while time.time() - t0 < 40 and len(seen) < len(MARKS):
            try:
                txt = open(LOG, encoding="utf-8", errors="replace").read()
            except FileNotFoundError:
                txt = ""
            for name, pat in MARKS:
                if name not in seen and pat in txt:
                    seen[name] = (time.time() - t0) * 1000
            time.sleep(0.01)
        print("=== %s 启动计时（ms，含 QEMU 自身启动 ≈ 数百 ms）===" % ("KVM" if KVM else "TCG"))
        prev = 0.0
        for name, _ in MARKS:
            if name in seen:
                t = seen[name]
                print("  %-14s %8.1f ms   (+%.1f)" % (name, t, t - prev))
                prev = t
            else:
                print("  %-14s    超时未出现" % name)
        try:
            txt = open(LOG, encoding="utf-8", errors="replace").read()
            print("  串口输出字节数: %d" % len(txt.encode()))
        except FileNotFoundError:
            pass
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


main()

release()
