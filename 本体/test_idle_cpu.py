"""系统资源基线：量化「桌面空闲」期间的宿主 CPU 占用。

为什么这样测：
  - 不用 QMP/截图（那些只能测 guest 里发生了什么），而是直接读宿主机
    /proc/<qemu-pid>/task/*/stat 的 utime+stime（jiffies），这是 Linux 内核
    记的、guest 无法伪造的真实 CPU 时间。
  - 图形拓扑（ramfb + xHCI + tablet/mouse/kbd）成功进桌面、不注入任何输入，
    稳定静置 WINDOW 秒，测这段窗口里 QEMU 进程所有线程累计消耗的 CPU 秒数。
  - 忙等版：桌面主循环全速轮询 → 单核 ≈100%（CPU≈wall）。
    事件驱动 wfi 版：CPU 大部分时间睡眠，只在定时唤醒时poll → 占用大幅下降。

用法：
  python3 test_idle_cpu.py                       # 默认当前 kernel.elf
  KERNEL=/path/to/kernel.elf TAG=base WINDOW=8 python3 test_idle_cpu.py
输出一行：TAG: CPU=x.xx s / wall=x.xx s = xx.x%  (100%=占满一核)
"""
import os
import sys
import time
import glob
import subprocess

ROOT = "/workspace/安卓/loongos/本体"
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
WINDOW = float(os.environ.get("WINDOW", "8"))
SETTLE = float(os.environ.get("SETTLE", "2"))       # 就绪后再静置，避开首帧全屏绘制
TAG = os.environ.get("TAG", "cur")

LOG = "/tmp/loongos_idle_%s.log" % TAG
VARS = "/tmp/loongos_idle_%s_vars.fd" % TAG
AAVMF_VARS = "/usr/share/AAVMF/AAVMF_VARS.fd"


def cpu_jiffies(pid):
    """汇总 QEMU 进程所有线程的 utime+stime（/proc 里 1-based 第14/15列）。"""
    total = 0
    for f in glob.glob("/proc/%d/task/*/stat" % pid):
        try:
            parts = open(f).read().split()
            total += int(parts[13]) + int(parts[14])   # utime, stime
        except (OSError, ValueError, IndexError):
            pass
    return total


def wait_ready(timeout=25):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if "图形桌面已就绪" in open(LOG, "rb").read().decode("utf8", "replace"):
                return True
        except OSError:
            pass
        time.sleep(0.2)
    return False


def main():
    for p in (LOG, VARS):
        try:
            os.unlink(p)
        except OSError:
            pass
    if os.path.exists(AAVMF_VARS):
        subprocess.run(["cp", AAVMF_VARS, VARS], check=True)

    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-serial", "file:" + LOG,
        "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    HZ = os.sysconf("SC_CLK_TCK") or 100
    try:
        if not wait_ready():
            print("%s: FAIL 桌面未就绪（串口日志尾部：%r）" %
                  (TAG, open(LOG, "rb").read()[-200:]))
            return 2
        time.sleep(SETTLE)
        a = cpu_jiffies(p.pid)
        t0 = time.monotonic()
        time.sleep(WINDOW)
        b = cpu_jiffies(p.pid)
        t1 = time.monotonic()
        used = (b - a) / float(HZ)
        wall = t1 - t0
        pct = 100.0 * used / wall if wall else 0
        print("%s: CPU=%.2f s / wall=%.2f s = %.1f%%  (100%%=占满一核; jiffies %d→%d @%dHz)" %
              (TAG, used, wall, pct, a, b, HZ))
        return 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
