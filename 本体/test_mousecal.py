#!/usr/bin/env python3
"""鼠标校准（P2.1）验证。

A) 纯串口：mousecal 查询 / 设置 / reset，并确认写进 /etc/mousecal。
B) 图形：模拟 VNC 客户端固定偏移（+40,+30），走
   「开始菜单 → 设置 → 鼠标校准 → 点靶心」，确认偏移被算出、保存，
   且之后同一坐标注入时光标从 (440,330) 对齐到靶心 (400,300)。
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
OFF_X, OFF_Y = 40, 30          # 模拟客户端固定偏移
CENTER = (W // 2, H // 2)      # 靶心 (400,300)
RECEIVED = (CENTER[0] + OFF_X, CENTER[1] + OFF_Y)   # 内核实际收到 (440,330)


# ---------------------------------------------------------------- A
def test_serial():
    print("\n===== A) 纯串口 mousecal 命令 + 持久化 =====")
    sock_path = "/tmp/loongos_cal_serial.sock"
    for p in (sock_path,):
        try: os.unlink(p)
        except OSError: pass
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-monitor", "none",
        # 不挂 ramfb → 无帧缓冲 → 桌面优雅降级为串口 shell
        "-serial", "unix:%s,server=on,wait=off" % sock_path,
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    s = None
    try:
        end = time.time() + 20
        while time.time() < end:
            if os.path.exists(sock_path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(sock_path)
                    break
                except OSError:
                    s = None
            time.sleep(0.15)
        if s is None:
            print("  ❌ 连不上串口 socket"); return False
        s.settimeout(0.4)
        buf = b""

        def ask(line, expect, timeout=6):
            nonlocal buf
            buf = b""
            s.sendall((line + "\n").encode())
            end = time.time() + timeout
            while time.time() < end:
                try: buf += s.recv(8192)
                except socket.timeout: pass
                if expect.encode("utf-8") in buf:
                    return True
            return False

        # 等提示符
        end = time.time() + 20
        while time.time() < end:
            try: buf += s.recv(8192)
            except socket.timeout: pass
            if b"> " in buf: break
            time.sleep(0.1)

        cases = [
            ("mousecal",            "dx=0 dy=0",        "默认偏移 0"),
            ("mousecal 40 30",      "/etc/mousecal",    "手动设置并保存"),
            ("mousecal",            "dx=40 dy=30",      "查询到 40/30"),
            ("cat /etc/mousecal",   "40 30",            "文件内容正确"),
            ("mousecal reset",      "mousecal",         "清零"),
            ("mousecal",            "dx=0 dy=0",        "清零后查询"),
        ]
        ok = True
        for line, expect, desc in cases:
            r = ask(line, expect)
            print("  %s %-22s -> %s" % ("✓" if r else "✗", desc,
                                        expect if r else "未命中"))
            ok &= r
        return ok
    finally:
        if s: s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()


# ---------------------------------------------------------------- B
class Qmp:
    def __init__(self, path):
        end = time.time() + 15
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError:
                if time.time() > end: raise
                time.sleep(0.1)
        self.s.settimeout(5); self.buf = b""
        self._line(); self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8", "replace"))

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments: msg["arguments"] = arguments
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
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.05)
        png = os.path.join(SHOTS, name + ".png")
        try:
            from PIL import Image
            Image.open(ppm).convert("RGB").save(png)
        except Exception as e:
            print("  [warn] PPM->PNG: %s" % e)
        return png

    def abs_move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]})

    def btn(self, down):
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": "left"}},
        ]})

    def click(self, x, y, hold=0.35):
        self.abs_move(x, y); time.sleep(0.15)
        self.btn(True); time.sleep(hold)
        self.btn(False); time.sleep(0.35)


def region_diff(a, b, box):
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB").crop(box)
    ib = Image.open(b).convert("RGB").crop(box)
    d = ImageChops.difference(ia, ib).convert("L")
    hist = d.histogram()
    return sum(hist[12:]) / float(ia.size[0] * ia.size[1])


def red_pixels(png, box):
    from PIL import Image
    im = Image.open(png).convert("RGB").crop(box)
    n = 0
    for r, g, b in im.getdata():
        if r > 150 and g < 100 and b < 100: n += 1
    return n


LOG = "/tmp/loongos_cal_serial.log"
QMP = "/tmp/loongos_cal_qmp.sock"
VARS = "/tmp/loongos_cal_vars.fd"
SHOTS = "/tmp/loongos_cal_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP, VARS):
    try: os.unlink(p)
    except OSError: pass
subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", VARS], check=True)


def wait_log(pat, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pat.encode() in open(LOG, "rb").read(): return True
        except OSError: pass
        time.sleep(0.15)
    return False


def test_gfx():
    print("\n===== B) 图形校准向导（模拟固定偏移 +40,+30）=====")
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
        if not wait_log("图形桌面已就绪", 35):
            print("  ❌ 桌面未就绪"); return False
        q = Qmp(QMP)
        time.sleep(0.5)
        ok = True

        # 校准前：同一注入坐标 (440,330) → 光标画在 440,330
        q.abs_move(*RECEIVED); time.sleep(0.4)
        before = q.shot("00_before")

        # 开始菜单 → 「设置」→ 设置窗口里的「鼠标校准」按钮
        # ⚠️ 第四十五/四十六轮修正：旧脚本点固定坐标 (110,535) 并注释「第 11 项
        #    鼠标校准」，但「鼠标校准」从来不是开始菜单的独立项——它是「设置」
        #    App 里的一个按钮（desktop.c set_press）。fa3a2b5 那轮之后菜单也再无
        #    该独立项，固定坐标实际落到「重启」附近，B 段长期稳定假失败。
        #    坐标推导（装好的系统 A_INSTALL 置空 → 菜单 12 项）：
        #      menu_rect: m.y = 600-42-(8+12*26+8) = 230
        #      第 7 项「设置」(idx 6): y = 230+8+6*26 .. +22 = [394,416] → 405
        #    桌面开机默认开 4 个窗口(TERM/INFO/DEMO/HELP)，G_open_seq=4：
        #      open_app 设置窗 r = (40+4*26, 38+6+4*24, 380, 300) = (144,140,...)
        #      内容区 cr = (r.x+1, r.y+28, ...) = (145,168,...)
        #      按钮 app_button(cr,14,60,162,26) → 绝对中心 (145+14+81, 168+60+13)
        q.click(49, 579)
        menu = q.shot("01_menu")
        q.click(110, 405)                 # 「设置」App
        q.shot("01b_settings")
        q.click(240, 241)                 # 设置窗口里的「鼠标校准」按钮
        cal = q.shot("02_cal_screen")

        rp = red_pixels(cal, (CENTER[0]-18, CENTER[1]-18,
                             CENTER[0]+18, CENTER[1]+18))
        print("  %s 校准层红色靶心像素=%d" % ("✓" if rp > 100 else "✗", rp))
        ok &= rp > 100

        # 用户点靶心，但客户端送偏了 → 内核收到 (440,330)
        q.click(*RECEIVED)
        result = q.shot("03_cal_result")
        diff_msg = region_diff(cal, result, (150, H-95, 650, H-80))
        print("  %s 结果文字区域差异=%.3f" % ("✓" if diff_msg > 0.02 else "✗", diff_msg))
        ok &= diff_msg > 0.02

        # 再点一下返回桌面
        q.click(*RECEIVED)
        q.shot("04_back")

        # 校准后同一坐标：光标应落在靶心 (400,300)
        q.abs_move(*RECEIVED); time.sleep(0.4)
        after = q.shot("05_aligned")
        box_new = (CENTER[0]-6, CENTER[1]-6, CENTER[0]+18, CENTER[1]+22)
        box_old = (RECEIVED[0]-6, RECEIVED[1]-6, RECEIVED[0]+18, RECEIVED[1]+22)
        d_new = region_diff(before, after, box_new)
        d_old = region_diff(before, after, box_old)
        print("  %s 靶心区差异=%.3f 旧光标区差异=%.3f" %
              ("✓" if d_new > 0.03 and d_old > 0.03 else "✗", d_new, d_old))
        ok &= d_new > 0.03 and d_old > 0.03

        print("  截图目录:", SHOTS)
        return ok
    finally:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()


def main():
    a = test_serial()
    b = test_gfx()
    print("\n===== 结果 =====")
    print("  A 串口命令/持久化:", "PASS" if a else "FAIL")
    print("  B 图形校准向导  :", "PASS" if b else "FAIL")
    print("  ", "ALL PASS" if (a and b) else "有失败项")
    return 0 if (a and b) else 1


if __name__ == "__main__":
    sys.exit(main())
