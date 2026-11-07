#!/usr/bin/env python3
"""真机同构复现（UEFI + ramfb + KVM），并在 bench 期间统计宿主 KVM 退出次数。

安全护栏（针对「跑测试把 NAS 拖死」这个教训）：
  1. 可用内存 < 需求 + 150MB → 直接拒绝运行
  2. 单实例锁：/tmp/loongos_probe.lock（同一时刻只允许一个测试 VM）
  3. 硬超时 + 退出时必杀（finally 里 terminate→kill）
  4. 默认 -m 256 -smp 1（绝不默认 512/2，那会顶到 NAS 的 swap）

用法：sudo python3 probe_fb_boot.py [--bench] [--mem 256]
"""
import fcntl, os, re, socket, subprocess, sys, time, shutil

BASE = os.path.dirname(os.path.abspath(__file__))
LOG = "/tmp/fb_probe.log"
MON = "/tmp/fb_probe_mon.sock"
VNC = "/tmp/fb_probe_vnc.sock"
VARS = "/tmp/fb_probe_vars.fd"
LOCK = "/tmp/loongos_probe.lock"
TR = "/sys/kernel/debug/tracing"
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
DO_BENCH = "--bench" in sys.argv
BENCH_N = 3 if "--bench3" in sys.argv else 1
if "--bench3" in sys.argv:
    DO_BENCH = True
MEM = sys.argv[sys.argv.index("--mem") + 1] if "--mem" in sys.argv else "256"


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


def wr(path, val):
    try:
        open(path, "w").write(val)
    except Exception:
        pass


def sendkey(name):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    s.settimeout(3)
    time.sleep(0.05)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall(("sendkey %s\n" % name).encode())
    time.sleep(0.28)
    s.close()


def main():
    need = int(MEM) + 150
    if avail_mb() < need:
        print("✗ 拒绝运行：可用内存 %d MB < 需要 %d MB（保护 NAS）" % (avail_mb(), need))
        return 2
    lockf = open(LOCK, "w")
    try:
        fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("✗ 已有另一个测试 VM 在跑（锁 %s 被占），拒绝并发" % LOCK)
        return 2

    for p in (LOG, MON, VNC):
        try:
            os.unlink(p)
        except (FileNotFoundError, PermissionError):
            pass
    if not os.path.exists(VARS):
        shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)

    cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3",
           "-accel", "kvm", "-cpu", "host", "-m", MEM, "-smp", "1",
           "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
           "-drive", "if=pflash,format=raw,file=" + VARS,
           "-cdrom", os.path.join(BASE, "iso", "loongos.iso"),
           "-display", "none", "-vnc", "unix:" + VNC,
           "-serial", "file:" + LOG,
           "-monitor", "unix:%s,server,nowait" % MON,
           "-device", "ramfb", "-device", "qemu-xhci,id=usb",
           "-device", "usb-tablet,bus=usb.0,port=1",
           "-device", "usb-kbd,bus=usb.0,port=2"]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        t0 = time.time()
        while time.time() - t0 < 60:
            if "[shell]" in log() and "USB 键盘就绪" in log():
                print("✓ 启动成功：%.1fs 到 shell（-m %s, 1 vCPU）" % (time.time() - t0, MEM))
                break
            time.sleep(0.3)
        else:
            print("✗ 60s 没到 shell —— 卡死。日志尾部：")
            print(log()[-2000:])
            return 1

        if not DO_BENCH:
            mark = len(log())
            for ch in "whoami\r":
                sendkey("ret" if ch == "\r" else ch)
            time.sleep(2)
            print("---- 交互 ----")
            print(log()[mark:][-300:])
            return 0

        # 只统计 bench 窗口的宿主 KVM MMIO 退出
        wr(TR + "/tracing_on", "0"); wr(TR + "/trace", "")
        wr(TR + "/events/kvm/kvm_mmio/enable", "1"); wr(TR + "/tracing_on", "1")
        mark = len(log())
        win_start = mark
        for _ in range(BENCH_N):
            for ch in "bench\r":
                sendkey("ret" if ch == "\r" else ch)
            t1 = time.time()
            while time.time() - t1 < 60:
                if log()[mark:].count("提示: us 越小越好") >= 1:
                    break
                time.sleep(0.3)
            mark = len(log())   # 下一轮只看新增
            if BENCH_N > 1:
                time.sleep(0.5)
        wr(TR + "/tracing_on", "0")
        try:
            trace = open(TR + "/trace").read()
        except Exception:
            trace = ""
        addrs = re.findall(r"addr 0x([0-9a-f]+)", trace)
        print("---- bench（%d 轮，取每项最小值）----" % BENCH_N)
        import re as _re
        best = {}
        for line in log()[win_start:].splitlines():
            m = _re.search(r"(memcpy 1MB|fb 清屏 1 屏|fb 滚动 x8|fb 画 1000 字符):\s*(\d+) us", line)
            if m:
                k, v = m.group(1), int(m.group(2))
                if k not in best or v < best[k]:
                    best[k] = v
        for k, v in best.items():
            print("  %-16s %6d us" % (k, v))
        for line in log()[win_start:].splitlines():
            if any(k in line for k in ("fb=", "memcpy", "fb 清屏", "fb 滚动", "fb 画", "提示")):
                print("  " + line.strip())
        print("---- 宿主 KVM MMIO 退出: %d 次；其中 0x4x/0x5x 段: %d 次 ----"
              % (trace.count("kvm_mmio:"), len([a for a in addrs if len(a) == 8 and a[0] in "45"])))
        wr(TR + "/events/kvm/kvm_mmio/enable", "0")
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
        fcntl.flock(lockf, fcntl.LOCK_UN)
        lockf.close()


sys.exit(main())
