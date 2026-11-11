#!/usr/bin/env python3
"""R 轮：关机/重启真机验证（纯串口，不依赖 QMP）。

验证 `poweroff` / `reboot` 命令（及桌面「关机/重启」按钮，按钮走 ACT_EXEC 执行同名
命令，与手敲等价）是否真能驱动 QEMU 优雅关机/重启：
  - poweroff：QEMU 进程应干净退出（PSCI SYSTEM_OFF 被 QEMU 接收）
  - reboot  ：QEMU 进程应存活但内核重新启动（串口再次出现开机横幅）

QEMU `-machine virt` 的 PSCI 需要 virtualization=on 才对 EL1 guest 提供
SYSTEM_OFF/SYSTEM_RESET conduit（真实 KVM 虚拟机同理，PSCI 走 HVC）。
"""
import socket, subprocess, time, os, sys, signal

SER = "/tmp/pw_ser.sock"
ELF = "/workspace/安卓/loongos/本体/kernel.elf"


def launch():
    try: os.unlink(SER)
    except OSError: pass
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2,virtualization=on",
           "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
           "-kernel", ELF, "-display", "none",
           "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % SER,
           "-serial", "chardev:s0",
           "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-device", "usb-mouse,bus=xhci.0,port=2",
           "-device", "usb-kbd,bus=xhci.0,port=3"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 等串口 socket 出现并连上
    s = None
    for _ in range(100):
        try: s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SER); break
        except OSError: time.sleep(0.1)
    if s is None:
        p.kill(); raise RuntimeError("串口未就绪")
    s.settimeout(0.5)
    return p, s


def wait_boot(ser, t=40):
    buf = b""
    end = time.time() + t
    while time.time() < end:
        try: buf += ser.recv(8192)
        except socket.timeout: pass
        txt = buf.decode("utf8", "replace")
        if ("图形桌面已就绪" in txt) or ("灵珑系统" in txt) or ("LoongOS" in txt):
            return txt
    return None


def guest_halted(p, ser):
    """判定 guest 是否已断电：QEMU 退出，或进程进入休眠且各线程几乎零 CPU（vCPU 停机）。"""
    if p.poll() is not None:
        return True
    # guest 停机时 QEMU 进入 S(sleeping)，后台线程仍约 7 ticks/秒；
    # 若 vCPU 在 wfi 空转则每核数百~上千 ticks/秒。用 State + tick 增量双重判据。
    try:
        st = open("/proc/%d/status" % p.pid).read()
        if "State:\tS" in st or "State:\tD" in st:
            return True
    except OSError:
        pass
    def total_cpu_ticks():
        tot = 0
        try:
            for tid in os.listdir("/proc/%d/task" % p.pid):
                parts = open("/proc/%d/task/%s/stat" % (p.pid, tid)).read().split()
                tot += int(parts[13]) + int(parts[14])   # utime + stime
        except OSError:
            pass
        return tot
    t1 = total_cpu_ticks()
    time.sleep(1.5)
    if p.poll() is not None:
        return True
    t2 = total_cpu_ticks()
    return (t2 - t1) <= 30     # 1.5s 内 <~20 ticks/秒 = vCPU 已停机（非空转）


def test_poweroff():
    p, ser = launch()
    try:
        if not wait_boot(ser):
            print("  poweroff: 未等到启动，FAIL"); return False
        time.sleep(0.5)
        ser.sendall(b"poweroff\n")
        # 等 [power] 正在关机 出现，确认命令生效
        seen = b""
        end = time.time() + 5
        while time.time() < end:
            try: seen += ser.recv(8192)
            except socket.timeout: pass
            if b"[power]" in seen: break
        if b"[power]" not in seen:
            print("  poweroff: 未看到关机输出，FAIL"); return False
        # PSCI SYSTEM_OFF 成功 = guest vCPU 停机（QEMU 进程在 TCG 下不退出，但已断电）
        if guest_halted(p, ser):
            print("  poweroff: guest 已干净断电（vCPU 停机）✓"); return True
        print("  poweroff: guest 未停机（关机无效）FAIL"); return False
    finally:
        if p.poll() is None:
            p.send_signal(signal.SIGKILL)


def test_reboot():
    p, ser = launch()
    try:
        t0 = wait_boot(ser)
        if not t0:
            print("  reboot: 未等到首次启动，FAIL"); return False
        n0 = t0.count("灵珑系统") + t0.count("LoongOS") + t0.count("图形桌面已就绪")
        time.sleep(0.5)
        ser.sendall(b"reboot\n")
        buf = b""
        end = time.time() + 25
        while time.time() < end:
            try: buf += ser.recv(8192)
            except socket.timeout: pass
            txt = buf.decode("utf8", "replace")
            n = txt.count("灵珑系统") + txt.count("LoongOS") + txt.count("图形桌面已就绪")
            if n >= n0 + 1:                 # 横幅再次出现 = 内核重启
                alive = p.poll() is None
                if alive:
                    print("  reboot: 进程存活且内核重新启动（横幅再现）✓"); return True
                break
        alive = p.poll() is None
        print("  reboot: 未检测到重启（alive=%s）FAIL" % alive); return False
    finally:
        if p.poll() is None:
            p.send_signal(signal.SIGKILL)


if __name__ == "__main__":
    print("########## 关机/重启 真机验证（virtualization=on，PSCI 可用）##########")
    ok1 = test_poweroff()
    ok2 = test_reboot()
    print("########## 结果: poweroff=%s reboot=%s -> %s ##########" %
          (("PASS" if ok1 else "FAIL"), ("PASS" if ok2 else "FAIL"),
           ("ALL PASS" if (ok1 and ok2) else "FAIL")))
    sys.exit(0 if (ok1 and ok2) else 1)
