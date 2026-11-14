#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第四十五轮验收：安装盘「安装系统」App + 可选目标硬盘。

拓扑（QEMU virt / TCG / UEFI）：
  - 从 ISO 启动（UEFI stub 提供 payload.img → 这才是「安装盘」）
  - 挂 2 块不同类型硬盘：
      磁盘 0 = virtio-blk-pci  (d0)
      磁盘 1 = nvme            (d1, 挂 pcie-root-port)
  - 由此 blk_disk_count() 应 >= 2，且两块盘可分别选择。

验收点：
  1. 安装盘启动，`disks` 能列出 >= 2 块盘，且带盘号 / 型号 / 容量
  2. `install` 无参数：列出可用盘 + 要确认，且**不写任何盘**（安全）
  3. `install 9 yes`：盘号越界被拒（FAIL bad-disk），不写盘
  4. `install 1 yes`：装到磁盘 1
       - 输出含 [install] START 目标磁盘 1 与 [install] DONE
       - 磁盘 1 有 MBR 签名 55AA；磁盘 0 仍全零（选盘真的生效、没误伤）
  5. 装好的系统（不挂 ISO、从磁盘 1 启动）：
       - install 被 no-esp-payload 守卫挡掉
       - 桌面开始菜单不显示安装程序（串口诊断日志）
  6. `install yes` 旧语法仍兼容（默认磁盘 0）——单独一台干净 VM 验证

用法：python3 test_install_disk.py
"""
import os, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
from qemu_guard import preflight, release

SER   = "/tmp/inst_disk.ser"
D0    = os.path.join(BASE, "build", "inst_disk_d0.img")   # virtio-blk
D1    = os.path.join(BASE, "build", "inst_disk_d1.img")   # nvme
VARSDIR = os.path.join(BASE, "build")
CODE  = "/usr/share/AAVMF/AAVMF_CODE.fd"
ISO   = os.path.join(BASE, "iso", "loongos.iso")
SIZE_MB = 64

if not preflight(need_mb=256, label="test_install_disk.py"):
    sys.exit(2)


def fresh(path):
    with open(path, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)


class VM:
    """from_iso=True 时挂光盘当安装盘；否则只挂已装好的硬盘。
    disks: 'both' | 'd0' | 'd1' —— 装好后重启应只挂那块系统盘，
    否则内核 blk_probe_all 会绑到先枚举到的 virtio-blk（磁盘0）。"""
    def __init__(self, from_iso, with_desktop=False, disks="both", tag="a"):
        for p in (SER,):
            try: os.unlink(p)
            except (FileNotFoundError, PermissionError): pass
        # 每阶段独立 UEFI 变量文件：装过系统的 VARS 会记住从硬盘启动，
        # 复用会让下一台「挂 ISO」的 VM 直接从旧盘启动、看不到安装盘。
        vars = os.path.join(VARSDIR, "inst_disk_vars_%s.fd" % tag)
        import shutil
        shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", vars)
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=3",
               "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
               "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
               "-drive", "if=pflash,format=raw,file=" + vars,
               "-serial", "unix:%s,server,nowait" % SER,
               "-display", "none", "-no-reboot"]
        # 磁盘 0：virtio-blk
        if disks in ("both", "d0"):
            cmd += ["-drive", "if=none,id=d0,format=raw,file=" + D0,
                    "-device", "virtio-blk-pci,drive=d0"]
        # 磁盘 1：NVMe（挂根端口，参考 test_nvme_install.py 的可用拓扑）
        if disks in ("both", "d1"):
            cmd += ["-device", "pcie-root-port,id=rp0,bus=pcie.0,chassis=1",
                    "-drive", "if=none,id=d1,format=raw,file=" + D1,
                    "-device", "nvme,drive=d1,serial=loongos-d1,bus=rp0"]
        if with_desktop:
            cmd += ["-device", "ramfb"]
        if from_iso:
            cmd += ["-cdrom", ISO]
        self.err = open(os.path.join(BASE, "build", "inst_disk_qemu.err"), "wb")
        self.p = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        self.s = None
        for _ in range(80):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(SER)
                break
            except OSError:
                self.s = None; time.sleep(0.15)
        if self.s: self.s.settimeout(0.2)
        self.raw = b""      # 按字节累积，避免 recv 分片截断 UTF-8 多字节汉字
        self.buf = ""

    def pump(self, sec):
        end = time.time() + sec
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d: break
                self.raw += d
                self.buf = self.raw.decode("utf-8", "replace")
            except socket.timeout: pass
            except OSError: break

    def wait(self, token, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.3)
            if token in self.buf: return True
        return False

    def send(self, line, settle=1.5):
        self.raw = b""; self.buf = ""
        self.s.sendall((line + "\n").encode())
        end = time.time() + settle
        while time.time() < end:
            self.pump(0.3)
            if "> " in self.buf[-200:]: break
        return self.buf

    def close(self):
        try: self.p.terminate(); self.p.wait(timeout=5)
        except Exception: self.p.kill()
        try: self.s.close()
        except Exception: pass


def disk_mbr(path):
    with open(path, "rb") as f:
        sec = f.read(512)
    return len(sec) == 512 and sec[510] == 0x55 and sec[511] == 0xAA


def disk_all_zero(path):
    with open(path, "rb") as f:
        b = f.read()
    return len(b) == SIZE_MB * 1024 * 1024 and not any(b)


def main():
    fails = []
    def ck(name, cond, extra=""):
        print(("  ✅ " if cond else "  ❌ ") + name + (("  " + extra) if extra else ""), flush=True)
        if not cond: fails.append(name)

    print("造 2 块全新空白盘（各 %d MB）…" % SIZE_MB, flush=True)
    fresh(D0); fresh(D1)

    # ---------- ①~② 从 ISO 启动，验证多盘 + 选盘安装 ----------
    print("\n① 安装盘启动（ISO + 2 块硬盘），验证多盘枚举与安全确认", flush=True)
    vm = VM(from_iso=True, tag="p1")
    if not vm.wait("> ", 90):
        print("  ❌ 未等到 shell 提示符；串口尾部：\n" + vm.buf[-1500:], flush=True)
        vm.close(); fails.append("启动"); release(); return fails

    out = vm.send("disks", 3.0)
    ck("disks 列出磁盘 0", "磁盘 0 (" in out)
    ck("disks 列出磁盘 1", "磁盘 1 (" in out)
    ck("disks 带型号", ("virtio" in out or "nvme" in out))

    out = vm.send("install", 3.0)
    ck("install 无参列出可用磁盘", "可用磁盘" in out)
    ck("install 无参要求确认", "install 0 yes" in out)
    ck("install 无参不写盘（磁盘0仍全零）", disk_all_zero(D0))
    ck("install 无参不写盘（磁盘1仍全零）", disk_all_zero(D1))

    out = vm.send("install 9 yes", 3.0)
    ck("越界盘号被拒", "bad-disk" in out or "FAIL" in out)
    ck("越界不写盘", disk_all_zero(D0) and disk_all_zero(D1))

    print("\n② install 1 yes → 应装到磁盘 1，磁盘 0 保持空白", flush=True)
    out = vm.send("install 1 yes", 12.0)
    ck("报告目标磁盘 1", "目标磁盘 1" in out, out[-400:].replace("\n", " | ") if "目标磁盘 1" not in out else "")
    ck("安装完成 DONE", "[install] DONE" in out)
    vm.close()

    d1_ok, d0_clean = disk_mbr(D1), disk_all_zero(D0)
    ck("磁盘 1 写入了 MBR 签名", d1_ok)
    ck("磁盘 0 未被误写（仍全零）", d0_clean)

    # ---------- ③ 装好的系统：不挂 ISO，从磁盘 1 启动 ----------
    # ⚠️ 只挂装好的那块盘：blk_probe_all 才会绑到它（真实「系统盘」场景）。
    print("\n③ 从装好的系统启动（不挂 ISO）：安装程序应消失", flush=True)
    vm2 = VM(from_iso=False, with_desktop=True, disks="d1", tag="p2")
    booted = vm2.wait("> ", 90)
    ck("已装系统能启动到 shell", booted)
    if booted:
        # 完整开机日志里检查桌面诊断（不 send，避免清空缓冲）
        vm2.pump(3.0)
        ck("桌面：开始菜单不显示安装程序",
           "开始菜单不显示安装程序" in vm2.buf,
           vm2.buf[-300:].replace("\n", " | ") if "开始菜单不显示安装程序" not in vm2.buf else "")
        out = vm2.send("install yes", 3.0)
        ck("install 被 no-esp-payload 守卫挡掉", "no-esp-payload" in out)
    vm2.close()

    # ---------- ④ 安装盘正向验证：开始菜单确实启用「安装系统」 ----------
    print("\n④ 安装盘（ISO 启动）：开始菜单应启用「安装系统」", flush=True)
    fresh(D0); fresh(D1)
    vm4 = VM(from_iso=True, with_desktop=True, tag="p3")
    booted4 = vm4.wait("> ", 90)
    ck("安装盘能启动到 shell", booted4)
    if booted4:
        vm4.pump(3.0)
        ck("桌面：开始菜单已启用「安装系统」",
           "开始菜单已启用" in vm4.buf,
           vm4.buf[-300:].replace("\n", " | ") if "开始菜单已启用" not in vm4.buf else "")
    vm4.close()

    # ---------- ⑤ 旧语法 install yes 兼容性（默认磁盘 0） ----------
    print("\n⑤ 兼容旧语法 install yes（默认磁盘 0）", flush=True)
    fresh(D0); fresh(D1)
    vm3 = VM(from_iso=True, tag="p4")
    if not vm3.wait("> ", 90):
        ck("旧语法 VM 启动", False)
    else:
        # TCG 下写 4MB ESP 可能超过 12s；直接等到 DONE 标记（最长 60s）
        vm3.send("install yes", 1.0)          # 只发命令，不等提示符
        got = vm3.wait("[install] DONE", 60)
        ck("install yes 仍完成安装", got,
           (vm3.buf[-300:].replace("\n", " | ") if not got else ""))
    vm3.close()
    ck("默认写到磁盘 0", disk_mbr(D0))

    print("\n==== 结果：%s ====" % ("全部通过 ✅" if not fails else "%d 项失败 ❌" % len(fails)), flush=True)
    for f in fails: print("   - " + f, flush=True)
    release()
    return fails


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
