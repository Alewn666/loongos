#!/usr/bin/env python3
"""NVMe 原生盘端到端装盘验收（2026-11-08）。

与 test_scsi_install.py 的关键差异：
  * AAVMF/EDK2 **没有 NVMe 驱动**，无法从 NVMe 盘引导；因此三个阶段都从 ISO
    （CD 走 virtio-scsi）启动内核，由内核自己的 nvme 驱动去认盘 / 挂载分区。
    这正好验证「原生 NVMe 拓扑下识别硬盘、MOUNTED-DISK、install yes」。
  * 沙箱无 /dev/kvm → 用 TCG（-cpu cortex-a57），不用 -accel kvm。
  * 盘 96MB：install 需要 ESP(1MB 偏移 + 4MB 镜像按 1MB 对齐) + >=4MB 数据区，
    8MB 的 disk_nvme.img 只够识别、不够装盘。

阶段：
  ① 从 ISO 启动 → install yes → [install] DONE（写 MBR / ESP / 格式化数据区）
  ② 重启（仍从 ISO）→ 内核认 NVMe 盘 → [fs] MOUNTED-DISK
  ③ 写文件 → 再重启 → cat 文件还在（验证 nv_read/nv_write 持久化）

用法：python3 test_nvme_install.py
"""
import os, socket, shutil, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
from qemu_guard import preflight, release

SER = os.path.join(BASE, "build", "nvme_install.ser")
DISK = os.path.join(BASE, "build", "nvme_inst_disk.img")
VARS = os.path.join(BASE, "build", "nvme_inst_vars.fd")
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
ISO = os.path.join(BASE, "iso", "loongos.iso")
SIZE_MB = 96

if not preflight(need_mb=384, label="test_nvme_install.py"):
    sys.exit(2)


class VM:
    """一个 QEMU 实例：CD=ISO（引导用），NVMe=DISK（装盘目标）。"""

    def __init__(self):
        try:
            os.unlink(SER)
        except OSError:
            pass
        # 每次都用全新 NVRAM：残留启动项会让 UEFI 跳 PXE，表现为"起不来"
        shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=3",
               "-cpu", "cortex-a57", "-m", "256M", "-smp", "1",
               "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
               "-drive", "if=pflash,format=raw,file=" + VARS,
               "-serial", "unix:%s,server,nowait" % SER,
               "-display", "none", "-monitor", "none",
               # CD：AAVMF 从它引导 stub + kernel.elf
               "-device", "virtio-scsi-pci,id=scsihost,bus=pcie.0",
               "-drive", "if=none,id=cd0,format=raw,media=cdrom,file=" + ISO,
               "-device", "scsi-cd,drive=cd0,bus=scsihost.0",
               # NVMe：挂在根端口后（真实 PCIe 拓扑）
               "-device", "pcie-root-port,id=rp0,bus=pcie.0,chassis=1",
               "-device", "nvme,drive=dp,serial=loongos-test,bus=rp0",
               "-drive", "if=none,id=dp,format=raw,file=" + DISK]
        self.err = open(os.path.join(BASE, "build", "nvme_install_qemu.err"), "wb")
        self.p = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        self.s = None
        for _ in range(80):
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

    def wait_for(self, tok, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.3)
            if tok in self.buf:
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
        try:
            self.err.close()
        except Exception:
            pass


def ascii_lines(buf):
    return [l.strip() for l in buf.splitlines()
            if any(k in l for k in ("[nvme]", "[disk]", "[fs  ]", "[install]",
                                    "[stub]", "MOUNTED", "MB (", "容量"))]


def main():
    fails = []
    print("造全新空白 NVMe 盘 %d MB" % SIZE_MB, flush=True)
    with open(DISK, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)

    # ---------- ① 从 ISO 启动并 install yes ----------
    print("① 从 ISO 启动，执行 install yes …", flush=True)
    vm = VM()
    try:
        if not vm.wait_for("[shell]", 150):
            print("✗ 没进 shell")
            print(vm.buf[-1500:])
            return 1
        if "payload.img bytes" not in vm.buf:
            fails.append("stub 没读到 payload.img（install 会 FAIL no-esp-payload）")
        for l in ascii_lines(vm.buf):
            print("   " + l)
        vm.send("install yes\r")
        if not vm.wait_for("[install] DONE", 240):
            print("✗ 安装没完成，关键行：")
            for l in ascii_lines(vm.buf[-2500:]):
                print("   " + l)
            fails.append("install 未完成")
        else:
            for l in ascii_lines(vm.buf[-1500:]):
                if "[install]" in l:
                    print("   " + l)
    finally:
        vm.close()

    if fails:
        print("FAIL:", fails)
        return 1

    # ---------- ② 重启（仍从 ISO），内核应挂载 NVMe 盘上的数据分区 ----------
    print("② 重启，检查 MOUNTED-DISK …", flush=True)
    vm = VM()
    try:
        if not vm.wait_for("[shell]", 150):
            print("✗ 起不来")
            print(vm.buf[-1500:])
            return 1
        for l in ascii_lines(vm.buf):
            print("   " + l)
        if "MOUNTED-DISK" not in vm.buf:
            fails.append("② 没挂上 NVMe 磁盘分区")
        vm.send("write /home/nvme_ok.txt nvme-persist-ok\r")
        vm.pump(4)
        if "nvme-persist-ok" not in vm.buf:
            # 有些 write 实现不回显内容，不强求；但至少不能报失败
            if "FAIL" in vm.buf[-800:]:
                fails.append("② 写文件失败")
    finally:
        vm.close()

    # ---------- ③ 再重启，检查文件持久化 ----------
    print("③ 再启动，检查文件还在 …", flush=True)
    vm = VM()
    try:
        if not vm.wait_for("[shell]", 150):
            print("✗ 第三次启动失败")
            return 1
        mark = len(vm.buf)
        vm.send("cat /home/nvme_ok.txt\r")
        vm.pump(5)
        out = vm.buf[mark:]
        for l in out.splitlines():
            if "nvme" in l or "cat" in l:
                print("   " + l.strip())
        if "nvme-persist-ok" not in out:
            fails.append("③ 文件没持久（nv_read 从 NVMe 读回失败）")
    finally:
        vm.close()

    print()
    if fails:
        print("FAIL:", fails)
        return 1
    print("ALL PASS —— NVMe 盘：装盘 → MOUNTED-DISK → 文件持久 全通过")
    return 0


try:
    rc = main()
finally:
    release()
sys.exit(rc)
