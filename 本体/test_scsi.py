#!/usr/bin/env python3
"""验证 virtio-scsi 驱动：用一块 **scsi 总线** 的盘启动 LoongOS（和用户虚拟机同款）。

期望串口出现：
  [disk] virtio-scsi 就绪，容量 129 MB
  [fs  ] MOUNTED-DISK lba=... blocks=... (formatted)   ← 空白盘首次自动格式化
低配：-m 160 -smp 1，TCG，一次一个。
"""
import os, re, select, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ISO = os.path.join(HERE, "iso", "loongos.iso")
DISK_SRC = os.path.join(os.path.dirname(HERE), "linglong-linux", "install", "disk.img")
DISK = os.path.join(HERE, "build", "scsi_disk.img")
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
VARS_SRC = "/usr/share/AAVMF/AAVMF_VARS.fd"
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b[=>][0-9]?[a-zA-Z]?")


def mem_mb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) // 1024
    return 9999


def main():
    avail = mem_mb()
    print("可用内存 %d MB（需 450）" % avail, flush=True)
    if avail < 450:
        print("✗ 内存不足，拒绝起虚拟机"); return 2
    if not os.path.exists(DISK_SRC):
        print("缺安装镜像", DISK_SRC); return 1
    subprocess.run(["cp", DISK_SRC, DISK], check=True)     # 带 p2 空白数据区的镜像

    varsf = os.path.join(HERE, "build", "scsi_vars.fd")
    subprocess.run(["cp", VARS_SRC, varsf])
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
           "-m", "160", "-smp", "1",
           "-drive", f"if=pflash,format=raw,readonly=on,file={CODE}",
           "-drive", f"if=pflash,format=raw,file={varsf}",
           "-nographic", "-no-reboot",
           # 追踪：看设备侧有没有收到通知/SCSI 命令
           "-trace", "virtio_pci_notify_write", "-trace", "virtio_queue_notify",
           "-trace", "virtio_scsi_cmd_req", "-trace", "virtio_scsi_cmd_resp",
           "-device", "virtio-scsi-pci,id=scsi0",
           "-drive", f"if=none,id=cd0,media=cdrom,readonly=on,file={ISO}",
           "-device", "scsi-cd,drive=cd0,bus=scsi0.0",
           "-drive", f"if=none,id=d0,format=raw,file={DISK}",
           "-device", "scsi-hd,drive=d0,bus=scsi0.0,scsi-id=0,lun=1"]      # ← 硬盘走 scsi 总线
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)
    raw = b""

    def text():
        nonlocal raw
        while True:
            r, _, _ = select.select([p.stdout], [], [], 0.4)
            if not r: break
            c = p.stdout.read(65536)
            if not c: break
            raw += c
        return ANSI.sub(b"", raw).decode("utf-8", "replace")

    ok = []
    try:
        t0 = time.time()
        while time.time() - t0 < 150 and not re.search(r"\n> ", text()):
            time.sleep(0.3)
        t = text()
        s1 = "[disk] virtio-scsi" in t
        print(("  ✅ " if s1 else "  ❌ ") + "识别 virtio-scsi 磁盘", flush=True)
        ok.append(s1)
        m = re.search(r"\[disk\] virtio-scsi 就绪，容量 (\d+) MB", t)
        if m: print("      容量 = %s MB" % m.group(1))
        s2 = bool(re.search(r"\[fs  \] MOUNTED-DISK lba=\d+ blocks=\d+", t))
        print(("  ✅ " if s2 else "  ❌ ") + "挂载/格式化持久分区", flush=True)
        ok.append(s2)
        if not (s1 and s2):
            print("---- 串口尾部 ----"); print(t[-1800:])
            return 1
        # 写文件 → 断电重启 → 验证持久化
        p.stdin.write(b"write /home/scsi.txt hi-scsi\r"); p.stdin.flush(); time.sleep(3)
        print("  ✅ 写入 /home/scsi.txt", flush=True)
    finally:
        try: p.kill(); p.wait(timeout=5)
        except Exception: pass

    # 第二次启动：只挂盘
    cmd2 = [c for c in cmd if c not in ()]
    cmd2 = [c for c in cmd if not (c == ISO or c == "scsi-cd,drive=cd0,bus=scsi0.0"
                                   or c == "if=none,id=cd0,media=cdrom,readonly=on,file=" + ISO)]
    p = subprocess.Popen(cmd2, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)
    raw = b""
    try:
        t0 = time.time()
        while time.time() - t0 < 150 and not re.search(r"\n> ", text()):
            time.sleep(0.3)
        p.stdin.write(b"cat /home/scsi.txt\r"); p.stdin.flush(); time.sleep(3)
        t = text()
        s3 = "hi-scsi" in t
        print(("  ✅ " if s3 else "  ❌ ") + "重启后文件还在（持久化）", flush=True)
        ok.append(s3)
        if not s3:
            print(t[-1200:])
    finally:
        try: p.kill(); p.wait(timeout=5)
        except Exception: pass

    print("\n== %s （%d/%d）==" % ("PASS" if all(ok) else "FAIL", sum(ok), len(ok)))
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
