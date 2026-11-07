#!/usr/bin/env python3
"""端到端验收：把系统装到硬盘，再从硬盘启动，并验证文件持久。

三个阶段：
  ① 从 ISO（UEFI）启动，敲 `install yes` → 写 MBR + ESP + 格式化数据分区
  ② 关机，**不挂 ISO**，只挂硬盘启动 → 应该进 shell，且提示"已挂载磁盘分区"
  ③ 在 ② 里写一个文件 → 再重启一次 → 文件还在（持久化验证）

用法：python3 test_install.py [--keep]
"""
import os, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
from qemu_guard import preflight, release

SER = "/tmp/inst.ser"                      # socket 必须短路径，留 /tmp（不占内存）
DISK = os.path.join(BASE, "build", "inst_disk.img")   # ⚠️ 不能放 /tmp：tmpfs 会直接吃内存
VARS = os.path.join(BASE, "build", "inst_vars.fd")
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
ISO = os.path.join(BASE, "iso", "loongos.iso")
SIZE_MB = 64
KVM = True

if not preflight(need_mb=128, label="test_install.py"):
    sys.exit(2)


class VM:
    def __init__(self, with_iso):
        for p in (SER,):
            try:
                os.unlink(p)
            except (FileNotFoundError, PermissionError):
                pass
        if not os.path.exists(VARS):
            import shutil
            shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)
        cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3",
               "-accel", "kvm", "-cpu", "host", "-m", "128", "-smp", "1",
               "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
               "-drive", "if=pflash,format=raw,file=" + VARS,
               "-serial", "unix:%s,server,nowait" % SER,
               "-display", "none",
               "-drive", "if=none,id=d1,format=raw,file=" + DISK,
               "-device", "virtio-blk-pci,drive=d1"]
        if with_iso:
            cmd += ["-cdrom", ISO]
        self.err = open("/tmp/inst_qemu.err", "wb")
        self.p = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        self.s = None
        for _ in range(60):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(SER)
                break
            except OSError:
                self.s = None
                time.sleep(0.1)
        if self.s:
            self.s.settimeout(0.2)
        self.buf = ""

    def pump(self, sec):
        end = time.time() + sec
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d.decode("utf-8", "replace")
            except socket.timeout:
                pass
            except OSError:
                break

    def wait_for(self, token, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.3)
            if token in self.buf:
                return True
        return False

    def send(self, text):
        self.s.sendall(text.encode())

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
        try:
            self.s.close()
        except Exception:
            pass


def main():
    fails = []
    # ⚠️ 每次开跑都用全新空白盘：UEFI 会优先从「已装过系统的硬盘」启动（不走 ISO），
    #    用残留盘会让 phase① 从盘启动 → stub 找不到 payload.img → install 拒绝（2026-10-25 实测踩坑）。
    print("造全新空白盘 %d MB …" % SIZE_MB, flush=True)
    with open(DISK, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)

    def dump_tail(vm, n=400):
        print("   [repr 尾部] %r" % vm.buf[-n:], flush=True)

    # ---------- ① 从 ISO 安装 ----------
    print("① 从 ISO 启动，执行 install yes …", flush=True)
    vm = VM(with_iso=True)
    try:
        if not vm.wait_for("[shell]", 90):
            print("✗ ① 没进 shell"); print(vm.buf[-1500:]); dump_tail(vm); return 1
        for line in vm.buf.splitlines():
            if any(k in line for k in ("[stub]", "[disk]", "[fs  ]")):
                print("   " + line.strip())
        if "payload.img bytes" not in vm.buf:
            fails.append("stub 没读到 payload.img")
        mark = len(vm.buf)
        vm.send("install yes\r")
        if not vm.wait_for("[install] DONE", 120):
            print("✗ 安装没完成，关键行："); dump_tail(vm)
            for line in vm.buf[mark:].splitlines():
                if any(k in line for k in ("install", "install]", "ESP", "分区", "失败", "缺少", "磁盘")):
                    print("   " + line.strip())
            fails.append("install 没完成")
    finally:
        vm.close()

    if fails:
        print("FAIL:", fails); return 1

    # ---------- ② 只挂硬盘启动 ----------
    print("② 移除 ISO，仅从硬盘启动 …", flush=True)
    vm = VM(with_iso=False)
    try:
        if not vm.wait_for("[shell]", 90):
            print("✗ ② 没能从硬盘启动")
            print(vm.buf[-2000:])
            try:
                print(open("/tmp/inst_qemu.err").read()[-500:])
            except Exception:
                pass
            return 1
        got = [l.strip() for l in vm.buf.splitlines() if "[fs  ]" in l or "[disk]" in l or "[stub]" in l]
        for l in got:
            print("   " + l)
        if "[fs  ] MOUNTED-DISK" not in vm.buf:
            fails.append("② 没挂上磁盘分区（还在用内存盘）")
        # ---------- ③ 写文件 ----------
        vm.send("write /home/persist.txt hello-persist\r")
        vm.pump(3)
        print("③ 写入 /home/persist.txt，准备重启验证 …", flush=True)
    finally:
        vm.close()

    print("④ 再启动一次，检查文件是否还在 …", flush=True)
    vm = VM(with_iso=False)
    try:
        if not vm.wait_for("[shell]", 90):
            print("✗ ④ 重启失败"); print(vm.buf[-1500:]); return 1
        mark = len(vm.buf)
        vm.send("cat /home/persist.txt\r")
        vm.pump(5)
        out = vm.buf[mark:]
        for line in out.splitlines():
            if "persist" in line or "cat" in line:
                print("   " + line.strip())
        if "hello-persist" not in out:
            fails.append("④ 文件没持久（重启后丢了）")
    finally:
        vm.close()

    print()
    if fails:
        print("FAIL:", fails)
        return 1
    print("ALL PASS —— 装盘 → 从盘启动 → 文件持久 全部通过")
    return 0


release()
sys.exit(main())
