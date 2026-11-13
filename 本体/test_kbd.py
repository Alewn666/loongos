#!/usr/bin/env python3
"""LoongOS 键盘驱动 TCG 冒烟：真 QEMU + qemu-xhci + usb-kbd，用 monitor sendkey 敲字。

判定标准（缺一不可）：
  1. 串口出现 "[usb ] ✅ USB 键盘就绪"
  2. 输入 "help\r" 后，屏幕上出现回显 "help" 且输出含 "命令清单"
  3. 输入 "whoami\r" 后，输出含身份信息

用法：python3 test_kbd.py [--kvm]
"""
import os, re, socket, subprocess, sys, time, signal
import sys
# 宿主资源护栏（内存门槛 + 单实例锁）：NAS 只有 2GB，别把宿主拖死
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release
if not preflight(need_mb=400, label=os.path.basename(__file__)):
    sys.exit(2)

BASE = os.path.dirname(os.path.abspath(__file__))
KVM = "--kvm" in sys.argv
# ⚠️ KVM 要 sudo 跑、TCG 普通用户跑：日志/套接字必须分开，否则 root 建的 /tmp 文件
#    会让普通用户那趟 QEMU 直接 "'Permission denied'" 退出（踩过）
SUF = "_kvm" if KVM else ""
LOG = "/tmp/kbd_serial%s.log" % SUF
MON = "/tmp/kbd_mon%s.sock" % SUF

def cleanup():
    for p in (LOG, MON):
        try: os.unlink(p)
        except (FileNotFoundError, PermissionError): pass

def qemu_cmd():
    cmd = ["qemu-system-aarch64",
           "-machine", "virt,gic-version=%s" % ("3" if KVM else "2"),
           "-cpu", "host" if KVM else "cortex-a57",
           "-m", "128",
           "-kernel", os.path.join(BASE, "kernel.elf"),
           "-display", "none",
           "-serial", "file:" + LOG,
           "-monitor", "unix:%s,server,nowait" % MON,
           "-device", "qemu-xhci,id=xhci",
           # ⚠️ 复刻用户真机拓扑：tablet/mouse 排在键盘前面，驱动必须能跳过非键盘设备
           #    （2026-10-24 踩过：只认第一个有设备的端口 → 抓到 usb-tablet → 打不了字）
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-device", "usb-mouse,bus=xhci.0,port=2",
           "-device", "usb-kbd,bus=xhci.0,port=3"]
    if KVM:
        cmd.insert(1, "-accel")
        cmd.insert(2, "kvm")
    return cmd

def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""

def wait_for(pat, timeout=25, since=0):
    end = time.time() + timeout
    while time.time() < end:
        log = read_log()
        if pat in log[since:]:
            return True
        time.sleep(0.15)
    return False

def sendkey(name, delay=0.35):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    s.settimeout(3)
    time.sleep(0.05)
    try: s.recv(65536)
    except socket.timeout: pass
    s.sendall(("sendkey %s\n" % name).encode())
    time.sleep(delay)
    s.close()

def type_text(text):
    for ch in text:
        if ch == " ":
            sendkey("spc")
        elif ch == "\r":
            sendkey("ret")
        elif ch == "\b":
            sendkey("backspace")      # 退格
        else:
            sendkey(ch)

def main():
    cleanup()
    fails = []
    proc = subprocess.Popen(qemu_cmd(), stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        if not wait_for("[shell] help=", timeout=30):
            print("✗ 启动失败：没等到 shell 提示符")
            print(read_log()[-3000:])
            return 1
        print("✓ 启动到 shell")

        log = read_log()
        for line in log.splitlines():
            if "[usb" in line:
                print("  kernel:", line.strip())
        # 内核日志文案早已改成「[usb ] HID 枚举完成：键盘=1 指针设备=2」，
        # 旧判据 "USB 键盘就绪" 会误报 FAIL（第三十七轮 A/B 用上一版内核复现，
        # 确认与本轮改动无关）。两种文案都认，语义都是「HID 枚举成功」。
        if "USB 键盘就绪" not in log and "HID 枚举完成" not in log:
            fails.append("键盘没就绪（见上面 [usb] 日志）")
        else:
            print("✓ 键盘驱动初始化成功")

        mark = len(log)
        type_text("help\r")
        ok = wait_for("命令清单:", timeout=10, since=mark)
        if ok:
            print("✓ 鍵盘输入生效：help 命令被执行（屏幕出现命令清单）")
        else:
            fails.append("help 命令没执行（键盘输入没进去？）")

        mark = len(read_log())
        type_text("whoami\r")
        time.sleep(1.5)
        log = read_log()
        if "root" in log[mark:].lower() or "uid" in log[mark:].lower():
            print("✓ whoami 执行，身份信息输出")
        else:
            fails.append("whoami 没执行")

        # 退格键：打 "helz" → 退格 → "p" → 回车。
        # 退格若无效，命令会变成 "helzp"（不存在的命令）→ 不会出现命令清单
        mark = len(read_log())
        type_text("helz\bp\r")
        if wait_for("命令清单:", timeout=12, since=mark):
            print("✓ 退格键生效（helz + 退格 + p → help 被执行）")
        else:
            fails.append("退格键没生效（helz\\bp 没有变成 help）")

        # 回显判定：看输入行里有没有 "help" 字样（shell 回显）
        # ⚠️ 先剔掉后台心跳行（[task1]…）：它会插在输入回显中间，把 "help\n" 打断
        full = "\n".join(l for l in read_log().splitlines() if not l.startswith("[task1]"))
        if re.search(r"help\s*\n", full):
            print("✓ 回显正常（屏幕上看得见敲进去的字）")
        else:
            fails.append("没看到回显")
    finally:
        proc.send_signal(signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()

    print()
    if fails:
        print("FAIL:")
        for f in fails: print("  ✗", f)
        print("---- 串口尾部 ----")
        print(read_log()[-4000:])
        return 1
    print("ALL PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())

release()
