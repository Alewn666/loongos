#!/usr/bin/env python3
"""X86 闪烁光标专项验证（P3.3 共用绘图层能力在两个架构的实测证据）。

复用 test_x86.py 的 QMP 框架思路：启动 X86 ISO，静置采样终端输入行光标小块，
比较相邻帧像素差异。相位由 PIT g_ticks(100Hz) 驱动：打字后 500ms 常亮，
之后 500ms 亮 / 500ms 灭；hlt 每 10ms 被 PIT 唤醒，翻转即 draw_all 重绘。

输入行坐标（desktop_x86.c）：终端窗 w2={40,220,520,150} →
inr={48,250,504,24}，空行 prompt="> " 宽 16 → caret_x=48+4+16=68，
白色 8x16 方块画在 (68, 254)。放宽裁剪框取 (62,250,86,276)。

用法：python3 test_x86_blink.py
"""
import json
import os
import socket
import subprocess
import time

BASE = os.path.dirname(os.path.abspath(__file__))
ISO = os.path.join(BASE, "iso", "loongos_x86.iso")
SOCK = "/tmp/loongos_x86_blink_qmp.sock"
SER = "/tmp/loongos_x86_blink_serial.log"
SHOTS = "/tmp/loongos_x86_blink_shots"
os.makedirs(SHOTS, exist_ok=True)
CARET_BOX = (62, 250, 86, 276)     # (left, upper, right, lower)


class QMP:
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
        self.s.settimeout(5)
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

    def shot(self, name):
        from PIL import Image
        ppm = os.path.join(SHOTS, name + ".ppm")
        try:
            os.unlink(ppm)
        except OSError:
            pass
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.02)
        png = os.path.join(SHOTS, name + ".png")
        Image.open(ppm).convert("RGB").save(png)
        return png


def diff(a, b):
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB").crop(CARET_BOX)
    ib = Image.open(b).convert("RGB").crop(CARET_BOX)
    h = ImageChops.difference(ia, ib).convert("L").histogram()
    return sum(h[12:]) / float(ia.size[0] * ia.size[1])


def main():
    print("===== X86 闪烁光标专项验证 =====")
    print("    ISO:", ISO)
    for f in (SOCK, SER):
        try:
            os.remove(f)
        except OSError:
            pass
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "256M", "-cdrom", ISO,
        "-serial", "file:" + SER, "-qmp", "unix:" + SOCK + ",server=on,wait=off",
        "-no-reboot", "-display", "none", "-vga", "std",
    ], stdout=open("/tmp/loongos_x86_blink_qemu.log", "w"), stderr=subprocess.STDOUT)
    try:
        # 等内核启动并进桌面（串口出现诊断串）
        end = time.time() + 20
        ready = False
        while time.time() < end:
            try:
                if b"\xe5\x9b\xbe\xe5\xbd\xa2\xe6\xa1\x8c\xe9\x9d\xa2" in open(SER, "rb").read():
                    ready = True
                    break
            except OSError:
                pass
            time.sleep(0.2)
        print("    桌面已启动:", ready)
        c = QMP(SOCK)
        time.sleep(1.0)     # 越过打字后 500ms 常亮期，进入闪烁区
        shots = []
        for i in range(5):
            shots.append(c.shot("blink_%d" % i))
            time.sleep(0.55)
        ds = [diff(shots[i], shots[i + 1]) for i in range(4)]
        mx = max(ds)
        print("    相邻采样光标差异 =", " ".join("%.3f" % x for x in ds))
        print("    最大 = %.3f  → %s" % (mx, "在闪烁 ✓" if mx > 0.08 else "不闪（恒亮/恒灭）✗"))
        ok = mx > 0.08
        print("\n=== 结果：%s ===" % ("PASS ✅" if ok else "FAIL ❌"))
        return 0 if ok else 2
    finally:
        q.terminate()
        try:
            q.wait(timeout=5)
        except subprocess.TimeoutExpired:
            q.kill()


if __name__ == "__main__":
    raise SystemExit(main())
