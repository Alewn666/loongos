#!/usr/bin/env python3
"""R+2 轮：窗口最小化 + 任务栏恢复 行为验收（QMP 鼠标注入）。

验证（确定性、不依赖 OCR）：
  T1  点默认首窗口(A_TERM, open_seq=0 → (40,44), big 460x340)标题栏「最小化」按钮
      → 该窗口区域显著变化（窗口被收起 = 最小化生效）
  T2  点任务栏首个按钮（A_TERM 仍驻留）→ 该窗口区域再次显著变化（窗口恢复 = 任务栏恢复生效）

坐标依源码实算（800x600, TOPBAR_H=38, TITLE_H=28, TASKBAR_H=42）：
  窗口 r = {40, 44, 460, 340}
  min_rect(r) = {r.x+r.w-48, r.y+4, 20, 22} = {452,48,20,22} → 中心 (462,59)
  首个任务栏按钮 taskbtn_rect_dyn(0) = {96, 600-42+8, 96-6, 26} = {96,566,90,26} → 中心 (141,579)
"""
import json
import os
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(BASE, "kernel.elf"))
LOG = "/tmp/loongos_min_serial.log"
QMP = "/tmp/loongos_min_qmp.sock"
SHOTS = "/tmp/loongos_min_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass

W, H = 800, 600
WIN_BOX = (40, 44, 460, 340)            # A_TERM 窗口区域（排除标题栏按钮等不影响主体判定）
MIN_XY = (462, 59)                      # A_TERM 标题栏最小化按钮
TB_XY = (141, 579)                      # 任务栏首个按钮（A_TERM）


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
            if "return" in r or "error" in r: return r

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        png = os.path.join(SHOTS, name + ".png")
        try: os.unlink(ppm)
        except OSError: pass
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.02)
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png)
        return png

    def click(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]}); time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]})
        time.sleep(0.05)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]})
        time.sleep(0.35)


def diff(a, b, bx):
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB").crop(bx)
    ib = Image.open(b).convert("RGB").crop(bx)
    d = ImageChops.difference(ia, ib).convert("L")
    hist = d.histogram()
    return sum(hist[12:]) / float(ia.size[0] * ia.size[1])


def wait_boot(timeout=45):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if "图形桌面已就绪".encode("utf-8") in open(LOG, "rb").read():
                return True
        except OSError: pass
        time.sleep(0.15)
    return False


def main():
    print("===== 窗口最小化 + 任务栏恢复 行为验收  TAG=minimize =====")
    print("    内核: %s  md5=%s" %
          (KERNEL, subprocess.check_output(["md5sum", KERNEL], text=True).split()[0]))
    subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", "/tmp/loongos_min_vars.fd"], check=True)
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
        if not wait_boot():
            print("  T0 未等到桌面就绪，FAIL"); return 1
        q = Qmp(QMP)
        time.sleep(0.6)
        B = q.shot("base")                   # 默认 5 窗口（含 A_TERM）
        q.click(*MIN_XY)                     # 点 A_TERM 最小化按钮
        M = q.shot("minimized")              # A_TERM 被收起
        d_min = diff(B, M, WIN_BOX)
        t1 = d_min > 0.05
        print("    T1 点最小化→窗口区域差异=%.3f  %s" %
              (d_min, "窗口收起 ✓" if t1 else "窗口未变化 FAIL"))
        q.click(*TB_XY)                      # 点任务栏首个按钮（恢复 A_TERM）
        R = q.shot("restored")               # A_TERM 重现
        d_res = diff(M, R, WIN_BOX)
        t2 = d_res > 0.05
        print("    T2 点任务栏→窗口区域差异=%.3f  %s" %
              (d_res, "窗口恢复 ✓" if t2 else "窗口未恢复 FAIL"))
        ok = t1 and t2
        print("===== 结果: T1=%s T2=%s -> %s =====" %
              ("PASS" if t1 else "FAIL", "PASS" if t2 else "FAIL",
               "ALL PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        if p.poll() is None:
            p.send_signal(9)


if __name__ == "__main__":
    sys.exit(main())
