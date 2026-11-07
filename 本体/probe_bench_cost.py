#!/usr/bin/env python3
"""量 bench 期间宿主的 KVM 退出次数（判断帧缓冲访问是否被陷进宿主）。

用法：sudo python3 probe_bench_cost.py [--mem 256]
安全：单实例、内存够才跑、跑完必杀、有硬超时。
"""
import os, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
LOG = "/tmp/bb_cost.log"
MON = "/tmp/bb_mon.sock"
VNC = "/tmp/bb_vnc.sock"
TR = "/sys/kernel/debug/tracing"
MEM = "256"
for i, a in enumerate(sys.argv):
    if a == "--mem":
        MEM = sys.argv[i + 1]


def avail_mb():
    for l in open("/proc/meminfo"):
        if l.startswith("MemAvailable"):
            return int(l.split()[1]) // 1024
    return 0


def log():
    try:
        return open(LOG, "rb").read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def w(path, val):
    open(path, "w").write(val)


def main():
    need = int(MEM) + 300
    if avail_mb() < need:
        print("✗ 内存不够：可用 %d MB，需要 %d MB（为保护 NAS 拒绝运行）" % (avail_mb(), need))
        return 2
    for p in (LOG, MON, VNC):
        try:
            os.unlink(p)
        except (FileNotFoundError, PermissionError):
            pass
    cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3",
           "-accel", "kvm", "-cpu", "host", "-m", MEM, "-smp", "1",
           "-kernel", os.path.join(BASE, "kernel.elf"), "-display", "none",
           "-vnc", "unix:" + VNC, "-serial", "file:" + LOG,
           "-monitor", "unix:%s,server,nowait" % MON,
           "-device", "ramfb", "-device", "qemu-xhci,id=usb",
           "-device", "usb-kbd,bus=usb.0,port=1"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        t0 = time.time()
        while time.time() - t0 < 45:
            if "USB 键盘就绪" in log() and "[shell]" in log():
                break
            time.sleep(0.3)
        else:
            print("✗ 启动失败/超时")
            print(log()[-1200:])
            return 1

        # 只在这段时间统计 KVM MMIO 退出
        w(TR + "/tracing_on", "0")
        w(TR + "/trace", "")
        w(TR + "/events/kvm/kvm_mmio/enable", "1")
        w(TR + "/tracing_on", "1")

        mark = len(log())
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(MON)
        s.settimeout(2)
        time.sleep(0.3)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
        for ch in "bench\r":
            s.sendall(("sendkey %s\n" % ("ret" if ch == "\r" else ch)).encode())
            time.sleep(0.25)
        s.close()

        t1 = time.time()
        while time.time() - t1 < 40:
            if "提示: us 越小越好" in log()[mark:]:
                break
            time.sleep(0.3)
        w(TR + "/tracing_on", "0")

        try:
            trace = open(TR + "/trace").read()
        except Exception:
            trace = ""
        n_mmio = trace.count("kvm_mmio:")
        out = log()[mark:]
        print("---- bench 输出 ----")
        for line in out.splitlines():
            if any(k in line for k in ("fb=", "memcpy", "fb 清屏", "fb 滚动", "fb 画", "提示")):
                print("  " + line.strip())
        print("---- 同期宿主 KVM MMIO 退出: %d 次 ----" % n_mmio)
        # 按地址粗分：fb 在 0x4xxxxxxx/0x5xxxxxxx，串口在 0x09000000
        import re
        addrs = re.findall(r"addr 0x([0-9a-f]+)", trace)
        fb = [a for a in addrs if a.startswith(("4", "5")) and len(a) == 8]
        print("  其中落在 0x4x/0x5x（疑似帧缓冲/RAM）: %d 次" % len(fb))
        w(TR + "/events/kvm/kvm_mmio/enable", "0")
        return 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        for f in (MON, VNC):
            try:
                os.unlink(f)
            except FileNotFoundError:
                pass


sys.exit(main())
