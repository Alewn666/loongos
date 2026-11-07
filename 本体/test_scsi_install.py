#!/usr/bin/env python3
"""SCSI 版端到端验收（复刻飞牛/libvirt 的磁盘布局）：
   控制器 virtio-scsi，光盘=LUN0，硬盘=LUN1（用户虚拟机就是这样）。
阶段：① 从 ISO 启动 → install yes → ② 只挂硬盘启动 → ③ 写文件 → 重启后文件还在。
用法：python3 test_scsi_install.py
"""
import os, socket, shutil, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
from qemu_guard import preflight, release

SER = os.path.join(BASE, "build", "scsi_install.ser")
DISK = os.path.join(BASE, "build", "scsi_inst_disk.img")
VARS = os.path.join(BASE, "build", "scsi_inst_vars.fd")
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
ISO = os.path.join(BASE, "iso", "loongos.iso")
SIZE_MB = 96

if not preflight(need_mb=128, label="test_scsi_install.py"):
    sys.exit(2)


class VM:
    def __init__(self, with_iso):
        try:
            os.unlink(SER)
        except OSError:
            pass
        # ⚠️ 每次都用全新 NVRAM：残留的启动项（指向 ISO）会让 UEFI 直接跳 PXE，
        #    表现为"装好的盘起不来"，其实是固件根本没去扫盘。2026-10-25 实测踩过。
        shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", VARS)
        cmd = ["qemu-system-aarch64", "-machine", "virt-7.2,gic-version=3",
               "-accel", "kvm", "-cpu", "host", "-m", "128", "-smp", "1",
               "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
               "-drive", "if=pflash,format=raw,file=" + VARS,
               "-serial", "unix:%s,server,nowait" % SER,
               "-display", "none",
               "-device", "virtio-scsi-pci,id=scsi0"]
        if with_iso:
            cmd += ["-drive", "if=none,id=cd0,format=raw,media=cdrom,file=" + ISO,
                    "-device", "scsi-cd,bus=scsi0.0,channel=0,scsi-id=0,lun=0,drive=cd0"]
        else:
            # ⚠️ 光驱位置必须保留（空托盘）：EDK2 的 virtio-scsi 按 LUN 顺序枚举，
            #    LUN0 不存在时不会去看 LUN1 → 表现为"装好的盘起不来、直接跳 PXE"（2026-10-25 实测）
            cmd += ["-device", "scsi-cd,bus=scsi0.0,channel=0,scsi-id=0,lun=0"]
        cmd += ["-drive", "if=none,id=hd0,format=raw,file=" + DISK,
                "-device", "scsi-hd,bus=scsi0.0,channel=0,scsi-id=0,lun=1,drive=hd0"]
        self.err = open(os.path.join(BASE, "build", "scsi_install_qemu.err"), "wb")
        self.p = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        self.s = None
        for _ in range(80):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(SER)
                break
            except OSError:
                self.s = None; time.sleep(0.1)
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
            self.p.terminate(); self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
        try:
            self.s.close()
        except Exception:
            pass


def ascii_lines(buf):
    return [l.strip() for l in buf.splitlines()
            if any(k in l for k in ("[scsi]", "[disk]", "[fs  ]", "[install]", "[stub]"))]


def main():
    fails = []
    print("造全新空白 SCSI 盘 %d MB" % SIZE_MB, flush=True)
    with open(DISK, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)

    print("① 从 ISO 启动并 install yes", flush=True)
    vm = VM(with_iso=True)
    try:
        if not vm.wait_for("[shell]", 100):
            print("✗ 没进 shell"); print(vm.buf[-1200:]); return 1
        for l in ascii_lines(vm.buf):
            print("   " + l)
        vm.send("install yes\r")
        if not vm.wait_for("[install] DONE", 180):
            print("✗ 安装没完成，关键行：")
            for l in ascii_lines(vm.buf[-2000:]):
                print("   " + l)
            fails.append("install 未完成")
    finally:
        vm.close()

    if fails:
        print("FAIL:", fails); return 1

    print("② 只挂硬盘启动", flush=True)
    vm = VM(with_iso=False)
    try:
        if not vm.wait_for("[shell]", 120):
            print("✗ 从硬盘起不来"); print(vm.buf[-1200:]); return 1
        for l in ascii_lines(vm.buf):
            print("   " + l)
        if "[fs  ] MOUNTED-DISK" not in vm.buf:
            fails.append("② 没挂上磁盘分区")
        vm.send("write /home/scsi_ok.txt scsi-persist-ok\r")
        vm.pump(3)
    finally:
        vm.close()

    print("③ 再启动一次，检查文件还在", flush=True)
    vm = VM(with_iso=False)
    try:
        if not vm.wait_for("[shell]", 120):
            print("✗ 第三次启动失败"); return 1
        mark = len(vm.buf)
        vm.send("cat /home/scsi_ok.txt\r")
        vm.pump(5)
        out = vm.buf[mark:]
        for l in out.splitlines():
            if "scsi" in l or "cat" in l:
                print("   " + l.strip())
        if "scsi-persist-ok" not in out:
            fails.append("③ 文件没持久")
    finally:
        vm.close()

    print()
    if fails:
        print("FAIL:", fails); return 1
    print("ALL PASS —— SCSI 盘：装盘 → 从盘启动 → 文件持久 全通过")
    return 0


release()
sys.exit(main())
