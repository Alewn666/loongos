#!/usr/bin/env python3
"""R 轮资源优化诊断：直接读 guest 物理内存计数器，判定 wfi 是真省电还是睡死。

identity map 下 .bss/.data 符号 VMA 即物理地址，用 QMP human-monitor-command
的 pmemsave 把 8 字节读出来。若高频 timer 正常触发，g_timer_irqs 应随墙钟增长；
g_wfi_hits 增长说明桌面确实进入了 wfi；两者都增长而画面不变，则是呈现逻辑问题。
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(BASE, "kernel.elf"))
TAG = os.environ.get("TAG", "resdiag")
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
VARS = "/tmp/loongos_%s_vars.fd" % TAG

# 由 aarch64-linux-gnu-nm kernel.elf 取得（identity map）
ADDR = {
    "tick":             0x400b6008,
    "g_desktop_active": 0x400b6c80,
    "g_timer_irqs":     0x400b6c88,
    "g_wfi_hits":       0x400b6c90,
    "g_wake_hz":        0x400ae5d4,
    "g_wake_div":       0x400ae5dc,
}


class Qmp:
    def __init__(self, path):
        end = time.time() + 15
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.1)
        self.s.settimeout(6)
        self.buf = b""
        self._line()
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8", "replace"))

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall((json.dumps(msg) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                return r

    def read64(self, addr, name):
        """用 HMP xp /1xg 读物理内存（pmemsave 在此 QEMU 版本语法不兼容）。
        返回形如 '00000000400b6008: 0x0000000000000015\r\n'。"""
        r = self.cmd("human-monitor-command",
                     {"command-line": "xp /1xg 0x%x" % addr})
        text = r.get("return", "") or ""
        import re
        m = re.search(r"0x([0-9A-Fa-f]{16})", text)
        if not m:
            m = re.search(r"0x([0-9A-Fa-f]{8})", text)
        if not m:
            raise RuntimeError("xp 读 0x%x 失败: %r" % (addr, text))
        return int(m.group(1), 16)

    def read32(self, addr, name):
        r = self.cmd("human-monitor-command",
                     {"command-line": "xp /1xw 0x%x" % addr})
        text = r.get("return", "") or ""
        import re
        m = re.search(r"0x([0-9A-Fa-f]{8})", text)
        if not m:
            raise RuntimeError("xp 读 0x%x 失败: %r" % (addr, text))
        return int(m.group(1), 16)

    def pc(self):
        r = self.cmd("human-monitor-command", {"command-line": "info registers"})
        import re
        m = re.search(r"PC=([0-9A-Fa-f]{8,16})", r.get("return", "") or "")
        return m.group(1) if m else "?"

    def snapshot(self):
        out = {}
        for k, a in ADDR.items():
            out[k] = self.read32(a, k) if k.startswith("g_wake") or k == "g_desktop_active" \
                else self.read64(a, k)
        return out

    def abs_move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 799)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 599)}},
        ]})

    def shot(self, name):
        ppm = "/tmp/loongos_%s_%s.ppm" % (TAG, name)
        try:
            os.unlink(ppm)
        except OSError:
            pass
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.02)
        from PIL import Image, ImageChops
        png = "/tmp/loongos_%s_%s.png" % (TAG, name)
        Image.open(ppm).convert("RGB").save(png)
        return png


def framediff(a, b, box):
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB").crop(box)
    ib = Image.open(b).convert("RGB").crop(box)
    h = ImageChops.difference(ia, ib).convert("L").histogram()
    return sum(h[12:]) / float(ia.size[0] * ia.size[1])


def wait_log(pat, timeout=35):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pat.encode() in open(LOG, "rb").read():
                return True
        except OSError:
            pass
        time.sleep(0.15)
    return False


def main():
    for p in (LOG, QMP, VARS):
        try:
            os.unlink(p)
        except OSError:
            pass
    subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", VARS], check=True)
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_log("图形桌面已就绪"):
            print("桌面未就绪，日志尾部:", open(LOG, "rb").read()[-300:])
            return 2
        q = Qmp(QMP)
        time.sleep(0.5)
        print("addr pc=%s" % q.pc())
        s0 = q.snapshot()
        print("T0", s0, "pc=%s" % q.pc())

        # ① 静置 2s：看 timer IRQ / tick / wfi 计数是否增长
        time.sleep(2.0)
        s1 = q.snapshot()
        print("T1(+2.0s静置)", s1, "pc=%s" % q.pc())
        print("  静置增量: timer_irqs=%d tick=%d wfi_hits=%d" %
              (s1["g_timer_irqs"] - s0["g_timer_irqs"],
               s1["tick"] - s0["tick"],
               s1["g_wfi_hits"] - s0["g_wfi_hits"]))

        # ② 注入鼠标移动，看画面是否变化（桌面是否还活着）
        a0 = q.shot("before")
        q.abs_move(500, 300)
        time.sleep(0.6)
        a1 = q.shot("after_move")
        dm = framediff(a0, a1, (480, 280, 540, 340))
        print("  鼠标移动到(500,300)后画面差异(光标框)=%.4f  pc=%s" % (dm, q.pc()))
        s2 = q.snapshot()
        print("  移动后 wake_hz=%d div=%d timer_irqs=%d wfi_hits=%d" %
              (s2["g_wake_hz"], s2["g_wake_div"],
               s2["g_timer_irqs"], s2["g_wfi_hits"]))

        # ③ 再静置看是否自由回落到低频（自适应）
        time.sleep(3.0)
        s3 = q.snapshot()
        print("T3(+3.0s静置)", s3)
        print("  久静后 wake_hz=%d（期望 120）" % s3["g_wake_hz"])
        return 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
