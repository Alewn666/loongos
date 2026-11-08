#!/usr/bin/env python3
"""复现「鼠标根本滑动不了，要点击一下才能知道鼠标的位置」。

覆盖 P2 / P2.1 全部既有测试的盲区：**只移动、不点击**时，桌面上内核画出来的
光标是否实时跟随。既有 test_desktop_p2.py / test_mousecal.py 的 click() 都自带
一次 abs_move，因此即使「纯移动不刷新」也照样能全绿。

阶段：
  0 基线：纯 abs_move 到屏幕中心 (400,300)
  1 单次纯移动 -> (200,150)，不点击
  2 连续 8 步纯移动轨迹，不点击
  3 纯相对移动 (rel)，不点击
  4 坐标不变，只按下/松开左键（模拟「点击一下」）
每阶段截图，输出「新位置框」「旧位置框」的像素差异比例，并保存截图供人工核对。
"""
import json
import os
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(BASE, "kernel.elf")
W, H = 800, 600

LOG = "/tmp/loongos_move_serial.log"
QMP = "/tmp/loongos_move_qmp.sock"
VARS = "/tmp/loongos_move_vars.fd"
SHOTS = "/tmp/loongos_move_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP, VARS):
    try:
        os.unlink(p)
    except OSError:
        pass


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
                if "error" in r:
                    print("  [QMP error] %s -> %s" % (execute, r["error"]))
                return r

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.05)
        png = os.path.join(SHOTS, name + ".png")
        try:
            from PIL import Image
            Image.open(ppm).convert("RGB").save(png)
        except Exception as e:
            print("  [warn] PPM->PNG: %s" % e)
        return png

    def abs_move(self, x, y):
        """只发绝对坐标事件，不发任何按键。"""
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]})

    def rel_move(self, dx, dy):
        """只发相对位移事件，不发任何按键。"""
        self.cmd("input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": int(dx)}},
            {"type": "rel", "data": {"axis": "y", "value": int(dy)}},
        ]})

    def btn(self, down):
        """只发按键事件，不附带任何坐标（关键：模拟「就点一下」）。"""
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": "left"}},
        ]})


def box(x, y):
    return (max(0, x - 8), max(0, y - 8), min(W, x + 26), min(H, y + 30))


def diff(a, b, bx):
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB").crop(bx)
    ib = Image.open(b).convert("RGB").crop(bx)
    d = ImageChops.difference(ia, ib).convert("L")
    hist = d.histogram()
    return sum(hist[12:]) / float(ia.size[0] * ia.size[1])


def report(tag, before, after, new_xy, old_xy):
    """打印新位置 / 旧位置两个框的差异比例。"""
    dn = diff(before, after, box(*new_xy))
    do = diff(before, after, box(*old_xy))
    moved = dn > 0.03 and do > 0.03
    print("  %s %-26s 新位置框=%.3f 旧位置框=%.3f  -> %s" %
          ("✓" if moved else "✗", tag, dn, do,
           "光标跟随" if moved else "光标没动"))
    return moved


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
    print("===== 只移动不点击：光标是否实时跟随 =====")
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
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        if not wait_log("图形桌面已就绪"):
            print("  ❌ 桌面未就绪")
            return 1
        q = Qmp(QMP)
        time.sleep(0.6)
        ok = True

        # 0 基线：纯移动到中心
        q.abs_move(400, 300)
        time.sleep(0.6)
        s0 = q.shot("00_center")

        # 1 单次纯移动（不点击）
        q.abs_move(200, 150)
        time.sleep(0.6)
        s1 = q.shot("01_abs_single")
        ok &= report("1 单次绝对移动", s0, s1, (200, 150), (400, 300))

        # 2 连续 8 步纯移动轨迹（不点击）
        for x, y in [(240, 180), (280, 210), (320, 240), (360, 270),
                     (400, 300), (440, 330), (480, 360), (520, 390)]:
            q.abs_move(x, y)
            time.sleep(0.06)
        time.sleep(0.6)
        s2 = q.shot("02_abs_track")
        ok &= report("2 连续绝对轨迹", s1, s2, (520, 390), (200, 150))

        # 3 纯相对移动（不点击）
        q.rel_move(-70, -50)
        time.sleep(0.6)
        s3 = q.shot("03_rel_move")
        ok &= report("3 相对移动", s2, s3, (450, 340), (520, 390))

        # 4 坐标完全不变，只按一下左键（信息项：点击不应让光标「跳」过去）
        q.btn(True)
        time.sleep(0.25)
        q.btn(False)
        time.sleep(0.6)
        s4 = q.shot("04_click_only")
        d_click = diff(s3, s4, box(450, 340))
        print("  · 4 只点击(坐标不变) 光标框差异=%.3f（越小说明越不靠点击才现形）"
              % d_click)

        print("\n  截图目录:", SHOTS)
        log = open(LOG, "rb").read().decode("utf-8", "replace")
        for line in log.splitlines():
            if "HID 枚举完成" in line or "识别 kind" in line:
                print("  [串口]", line)
        print("\n  ===>", "ALL 光标跟随正常" if ok else "复现：纯移动时光标不跟随")
        return 0 if ok else 2
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
