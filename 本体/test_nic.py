#!/usr/bin/env python3
"""LoongOS 阶段10 验收：多网卡 + 多型号 + 开机自动联网（QEMU 真硬件）。

复刻用户现场拓扑：8 张网卡，型号混杂
    Intel E1000 ×2 / Realtek RTL8139 ×2 / Intel E1000E ×2 / VirtIO ×2
每张卡挂独立 -netdev user 后端（各自 10.0.2.0/24，VM 拿 10.0.2.15，网关 10.0.2.2）。

验收点：
  1) 启动期自动扫描到全部 8 张卡，net list 列出 8 张，四种型号驱动都认领；
  2) 开机自动选「链路 up」的卡走 DHCP，拿到 IP（boot 日志出现「联网成功」）；
  3) ping 网关 10.0.2.2 通（收发路径在活动网卡上工作）；
  4) net select 切到另一张卡（型号不同），重新 DHCP 仍成功，ping 仍通 —— 证明多实例/活动切换。

用法：python3 test_nic.py
"""
import os
import re
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(BASE, "kernel.elf")
SOCK = "/tmp/loongos_nic.sock"

# 8 张网卡，型号顺序对齐用户现场：virtio/e1000/e1000/e1000e/e1000e/rtl8139/rtl8139/virtio
NICS = [
    ("virtio-net-pci", "52:54:00:00:00:01"),
    ("e1000",          "52:54:00:00:00:02"),
    ("e1000",          "52:54:00:00:00:03"),
    ("e1000e",         "52:54:00:00:00:04"),
    ("e1000e",         "52:54:00:00:00:05"),
    ("rtl8139",        "52:54:00:00:00:06"),
    ("rtl8139",        "52:54:00:00:00:07"),
    ("virtio-net-pci", "52:54:00:00:00:08"),
]


def qemu_cmd():
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-monitor", "none",
        "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % SOCK,
        "-serial", "chardev:s0",
    ]
    for i, (model, mac) in enumerate(NICS):
        nid = "n%d" % i
        cmd += ["-device", "%s,netdev=%s,mac=%s" % (model, nid, mac),
                "-netdev", "user,id=%s" % nid]
    return cmd


def drain(s, quiet=0.35, hard=4.0):
    buf = b""
    last = time.time()
    end = time.time() + hard
    while time.time() < end and time.time() - last < quiet:
        s.settimeout(0.12)
        try:
            d = s.recv(8192)
        except socket.timeout:
            continue
        if d:
            buf += d
            last = time.time()
    return buf


def send(s, line):
    """发一条 shell 命令，收到 `> ` 提示符才算完成。

    不能只用固定 quiet/hard：像 `net dhcp` 在等 OFFER 的 2 秒里串口没有任何输出，
    固定窗口会提前返回、把输出截断，后续命令就串台（曾把 e1000 的失败误判成
    「脚本时序问题」/「假通过」）。以提示符为准最可靠。
    """
    s.sendall((line + "\n").encode())
    buf = b""
    end = time.time() + 12.0
    while time.time() < end:
        buf += drain(s, quiet=0.35, hard=1.0)
        if b"> " in buf[-160:]:
            break
    return buf


def connect():
    for p in (SOCK,):
        try:
            os.unlink(p)
        except OSError:
            pass
    proc = subprocess.Popen(qemu_cmd(), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    s = None
    try:
        end = time.time() + 25
        while time.time() < end:
            if os.path.exists(SOCK):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(SOCK)
                    break
                except OSError:
                    s = None
            time.sleep(0.15)
        if s is None:
            raise RuntimeError("连不上串口 socket")
        # 启动日志：等提示符，再收干净（含开机自动联网的 DHCP 输出）
        boot = b""
        end = time.time() + 30
        while time.time() < end and b"> " not in boot:
            boot += drain(s, quiet=0.25, hard=1.0)
        boot += drain(s, quiet=0.6)
        return proc, s, boot
    except Exception:
        if s:
            s.close()
        proc.terminate()
        raise


def count_nics(text):
    m = re.search(r"清单（\s*(\d+)\s*张）", text)
    return int(m.group(1)) if m else -1


def main():
    proc, s, boot = connect()
    fails = 0
    try:
        boot_txt = boot.decode("utf-8", "replace")
        print("==== 启动日志（含开机自动联网）====")
        # 只打印网络相关行，避免刷屏
        for ln in boot_txt.splitlines():
            if any(k in ln for k in ("[net ]", "网卡", "联网", "DHCP", "IP ", "网关", "链路", "活动", "<==")):
                print("  " + ln.rstrip())

        # 验收点 1：net list 列出 8 张，四种型号都在
        r_list = send(s, "net list").decode("utf-8", "replace")
        cnt = count_nics(r_list)
        print("\n[验收1] net list 卡数 = %s（期望 8）" % cnt)
        ok1 = (cnt == 8)
        models = ["virtio", "e1000", "e1000e", "rtl8139"]
        for m in models:
            present = (m in r_list)
            print("       型号 %-8s 出现: %s" % (m, "✅" if present else "❌"))
            ok1 = ok1 and present
        print("   net list 全表:")
        for ln in r_list.splitlines():
            if ln.strip().startswith("["):
                print("      " + ln.rstrip())
        fails += 0 if ok1 else 1

        # 验收点 2：开机已自动 DHCP（boot 日志里有「联网成功」或 ifconfig 显示已配 IP）
        r_if = send(s, "ifconfig").decode("utf-8", "replace")
        ok2 = ("联网成功" in boot_txt) or ("IP:" in r_if and "未配置" not in r_if)
        print("[验收2] 开机自动联网/DHCP: %s" % ("✅" if ok2 else "❌"))
        fails += 0 if ok2 else 1
        print("   ifconfig 片段:")
        for ln in r_if.splitlines()[:6]:
            print("    " + ln.rstrip())

        # 验收点 3：ping 网关 10.0.2.2
        r_ping = send(s, "ping 10.0.2.2 1").decode("utf-8", "replace")
        ok3 = ("来自 10.0.2.2 的回复" in r_ping)
        print("[验收3] ping 网关 10.0.2.2: %s" % ("✅" if ok3 else "❌"))
        for ln in r_ping.splitlines():
            if "10.0.2.2" in ln or "成功" in ln or "超时" in ln:
                print("    " + ln.rstrip())
        fails += 0 if ok3 else 1

        # 验收点 4：切到另一张卡（选索引 5 = rtl8139，与首张 virtio 完全不同型号，验证多实例独立）
        r_sel = send(s, "net select 5").decode("utf-8", "replace")
        r_dhcp = send(s, "net dhcp").decode("utf-8", "replace")
        r_list2 = send(s, "net list").decode("utf-8", "replace")
        # 切卡成功后 net_rebind_active 会打印新 MAC（索引5 的 rtl8139 MAC 末段 06）
        ok4a = ("52:54:00:00:00:06" in r_sel)
        # 活动标记应落在索引 5 那行
        active_on_5 = False
        for ln in r_list2.splitlines():
            if ln.strip().startswith("[5]") and "<== 活动" in ln:
                active_on_5 = True
        ok4b = ("联网成功" in r_dhcp) or ("IP:" in r_list2 and "未配置" not in r_list2)
        r_ping2 = send(s, "ping 10.0.2.2 1").decode("utf-8", "replace")
        ok4c = ("来自 10.0.2.2 的回复" in r_ping2)
        print("[验收4] net select 5（rtl8139）切换+重 DHCP:")
        print("        net select 5 原始输出:")
        for ln in r_sel.splitlines():
            print("            " + ln.rstrip())
        print("        切卡生效(新MAC末段06): %s" % ("✅" if ok4a else "❌"))
        print("        活动标记在 [5]: %s" % ("✅" if active_on_5 else "❌"))
        print("        换卡后 DHCP: %s" % ("✅" if ok4b else "❌"))
        print("        换卡后 ping: %s" % ("✅" if ok4c else "❌"))
        fails += 0 if (ok4a and active_on_5 and ok4b and ok4c) else 1

        # 验收点 5：逐卡切换，覆盖全部 8 张卡 / 4 种型号，每卡都真实重 DHCP + ping。
        # 为什么必须逐卡测：阶段10 曾在 e1000/e1000e 上踩过「TX 描述符 cmd 字节
        # 位置错」的坑 —— TDH 照常推进（驱动误以为发送成功），但帧根本没发出去，
        # DHCP 永远收不到 OFFER。只测一张 rtl8139 是发现不了的。
        print("[验收5] 逐卡切换（8 张 / 4 型号，每卡真实 DHCP + ping）：")
        all_ok5 = True
        for idx, (model, mac) in enumerate(NICS):
            r_sel5  = send(s, "net select %d" % idx).decode("utf-8", "replace")
            r_dhcp5 = send(s, "net dhcp").decode("utf-8", "replace")
            r_ping5 = send(s, "ping 10.0.2.2 1").decode("utf-8", "replace")
            mac_ok  = (mac in r_sel5)
            dhcp_ok = ("OFFER" in r_dhcp5) and ("联网成功" in r_dhcp5)
            ping_ok = ("来自 10.0.2.2 的回复" in r_ping5)
            ok5 = mac_ok and dhcp_ok and ping_ok
            all_ok5 = all_ok5 and ok5
            print("        [%d] %-9s MAC=%s DHCP=%s PING=%s  %s" %
                  (idx, model, "✅" if mac_ok else "❌",
                   "✅" if dhcp_ok else "❌", "✅" if ping_ok else "❌",
                   "✅" if ok5 else "❌"))
        fails += 0 if all_ok5 else 1

        print("\n==== 结果: %s ====" % ("全部通过 ✅" if fails == 0 else "%d 项失败 ❌" % fails))
        return 1 if fails else 0
    finally:
        s.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
