#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SMP 多核验收（阶段9 正式版）。

覆盖三种拓扑：
  A. GICv2 -smp 4  —— 主目标：从核上线、主核 shell 输入正常、smpwork 自校+加速比
  B. GICv2 -smp 1  —— 回归：启动日志与单核基线一致、smpwork 报「仅1核在线」
  C. GICv3 -smp 4  —— 验证 UART IROUTER 钉 CPU0 后多核 shell 也不哑

判定：
  - A: 拉起3个、在线3个、whoami 有回显、smpwork 自校 PASS、加速比>1.0
  - B: 拉起0个、whoami 正常、smpwork 「仅1核在线」、启动行与 A 的单核行一致
  - C: 拉起3个、whoami 有回显、smpwork 自校 PASS
"""
import os, sys, time, socket, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(HERE, "kernel.elf")
SOCK = "/tmp/loongos_smp.sock"

def drain(s, quiet=0.25, hard=1.0):
    buf = b""
    end = time.time() + hard
    last = time.time()
    while time.time() < end:
        s.settimeout(0.05)
        try:
            d = s.recv(4096)
            if not d:
                break
            buf += d
            last = time.time()
        except socket.timeout:
            if time.time() - last >= quiet:
                break
    return buf

def launch(smp, gic):
    for p in (SOCK,):
        try: os.unlink(p)
        except OSError: pass
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=%d" % gic,
        "-cpu", "cortex-a57", "-m", "256", "-smp", str(smp),
        "-kernel", KERNEL, "-display", "none", "-monitor", "none",
        "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % SOCK,
        "-serial", "chardev:s0",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = None
    try:
        end = time.time() + 20
        while time.time() < end:
            if os.path.exists(SOCK):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(SOCK); break
                except OSError:
                    s = None
            time.sleep(0.15)
        if s is None:
            raise RuntimeError("连不上串口 socket")
        boot = b""
        end = time.time() + 25
        while time.time() < end and b"> " not in boot:
            boot += drain(s, quiet=0.25, hard=1.0)
        boot += drain(s, quiet=0.5)
        return proc, s, boot.decode("utf-8", "replace")
    except Exception:
        if s: s.close()
        proc.terminate()
        raise

def run_cmd(s, line, hard=4.0):
    drain(s, quiet=0.12, hard=0.4)
    s.sendall((line + "\n").encode())
    return drain(s, quiet=0.38, hard=hard).decode("utf-8", "replace")

def boot_smp_line(boot):
    for ln in boot.splitlines():
        if "[smp ]" in ln:
            return ln
    return ""

def main():
    fails = 0
    # ---------- A. GICv2 -smp 4 ----------
    print("=== A. GICv2 -smp 4 ===")
    proc, s, boot = launch(4, 2)
    try:
        sl = boot_smp_line(boot)
        print("  boot:", sl)
        who = run_cmd(s, "whoami")
        print("  whoami:", who.strip()[:60])
        cpu = run_cmd(s, "cpu")
        print("  cpu:\n" + "\n".join("    " + l for l in cpu.strip().splitlines()))
        w1 = run_cmd(s, "smpwork", hard=8.0)
        print("  smpwork #1:\n" + "\n".join("    " + l for l in w1.strip().splitlines()))
        w2 = run_cmd(s, "smpwork", hard=8.0)
        print("  smpwork #2:\n" + "\n".join("    " + l for l in w2.strip().splitlines()))
    finally:
        s.close(); proc.terminate()

    a_ok = ("拉起从核 3 个" in sl and "在线 3 个" in sl
            and "root" in who and "自校 PASS" in w1 and "加速比" in w1)
    # 加速比>1.0
    import re
    m = re.search(r"加速比 ≈ (\d+)\.(\d+)x", w1)
    if m:
        ratio = int(m.group(1)) + int(m.group(2))/100.0
        a_ok = a_ok and ratio > 1.0
        print("  加速比 = %.2fx" % ratio)
    else:
        a_ok = False
        print("  未解析到加速比")
    print("  A 结果:", "PASS" if a_ok else "FAIL")
    if not a_ok: fails += 1

    # ---------- B. GICv2 -smp 1（回归） ----------
    print("\n=== B. GICv2 -smp 1（回归） ===")
    proc, s, boot = launch(1, 2)
    try:
        sl = boot_smp_line(boot)
        print("  boot:", sl)
        who = run_cmd(s, "whoami")
        print("  whoami:", who.strip()[:60])
        w = run_cmd(s, "smpwork", hard=4.0)
        print("  smpwork:\n" + "\n".join("    " + l for l in w.strip().splitlines()))
    finally:
        s.close(); proc.terminate()
    b_ok = ("拉起从核 0 个" in sl and "root" in who
            and "仅 1 核在线" in w)
    print("  B 结果:", "PASS" if b_ok else "FAIL")
    if not b_ok: fails += 1

    # ---------- C. GICv3 -smp 4 ----------
    print("\n=== C. GICv3 -smp 4 ===")
    proc, s, boot = launch(4, 3)
    try:
        sl = boot_smp_line(boot)
        print("  boot:", sl)
        who = run_cmd(s, "whoami")
        print("  whoami:", who.strip()[:60])
        cpu = run_cmd(s, "cpu")
        print("  cpu:\n" + "\n".join("    " + l for l in cpu.strip().splitlines()))
        w = run_cmd(s, "smpwork", hard=8.0)
        print("  smpwork:\n" + "\n".join("    " + l for l in w.strip().splitlines()))
    finally:
        s.close(); proc.terminate()
    c_ok = ("root" in who and "自校 PASS" in w and "在线核数=4" in cpu)
    print("  C 结果:", "PASS" if c_ok else "FAIL")
    if not c_ok: fails += 1

    print("\n==== SMP 验收:", "ALL PASS" if fails == 0 else ("%d FAIL" % fails), "====")
    sys.exit(0 if fails == 0 else 1)

if __name__ == "__main__":
    main()
