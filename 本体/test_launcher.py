#!/usr/bin/env python3
"""R+1 轮：开始按钮 = App 启动器 行为验收（QMP 鼠标注入）。

用户诉求（2026-11-11）：之前系统的「开始」按钮根本没有用——点开只有 5 个
硬编码窗口的聚焦/命令，没有真正的 App 概念。本轮把开始按钮改造成真正的
App 启动器：点开列出全部 App（终端/安装系统/系统信息/图形演示/操作说明/
计算器/系统监视器/设置/关于/文件管理器/文本编辑器）+ 关机/重启，点应用即
开窗。

本脚本用 QMP 注入真实鼠标点击，验证「启动器」三件事（确定性、不依赖 OCR）：
  T1  点「开始」按钮 → 开始菜单区域出现显著变化（菜单展开 = 启动器生效）
  T2  点菜单里的「计算器」项 → 菜单区域再次显著变化（菜单收起 = 应用已启动）
  T3  对比「启动前」与「启动后」全屏（排除任务栏），画面发生实质变化
      （多开了一个窗口 = 应用真的画出来了，不是空壳）

坐标依源码实算（800x600, TASKBAR_H=42）：
  开始按钮 start_rect={10,558+8,78,26} → 中心 (49,579)
  菜单 menu_rect={10, 600-42-h, 232, h}, h=8+13*26+8=354 → y∈[204,558]
  菜单项 i 中心 y = 204+8+11+i*26 = 223+i*26, x 中心 = 10+8+(232-16)/2=126
    A_CALC=5  → (126, 223+130=353)
    A_FILES=9 → (126, 223+234=457)
"""
import json
import os
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(BASE, "kernel.elf"))
LOG = "/tmp/loongos_launcher_serial.log"
QMP = "/tmp/loongos_launcher_qmp.sock"
SHOTS = "/tmp/loongos_launcher_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass

W, H = 800, 600
MENU_BOX = (12, 210, 240, 556)          # 菜单展开覆盖的中部区域
FULL_BOX = (0, 0, W, H - 42)            # 全屏（排除任务栏 42px）
START_XY = (49, 579)
CALC_XY = (126, 353)
FILES_XY = (126, 457)


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

    def abs_move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]})

    def click(self, x, y):
        self.abs_move(x, y); time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]})
        time.sleep(0.05)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]})
        time.sleep(0.35)   # 等画面稳定（避开半帧）


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
    print("===== 开始按钮=App启动器 行为验收  TAG=launcher =====")
    print("    内核: %s  md5=%s" %
          (KERNEL, subprocess.check_output(["md5sum", KERNEL], text=True).split()[0]))
    subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", "/tmp/loongos_launcher_vars.fd"], check=True)
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
        B = q.shot("base")                 # 启动前：菜单关闭
        q.click(*START_XY)                 # 点「开始」
        M = q.shot("menu")                 # 菜单展开
        d_open = diff(B, M, MENU_BOX)
        t1 = d_open > 0.05
        print("    T1 点开始→菜单区域差异=%.3f  %s" %
              (d_open, "启动器菜单展开 ✓" if t1 else "菜单无反应 FAIL"))
        q.click(*CALC_XY)                  # 点「计算器」
        A1 = q.shot("calc")                # 菜单收起、计算器已开窗
        d_close = diff(M, A1, MENU_BOX)
        t2 = d_close > 0.05
        print("    T2 点计算器→菜单收起差异=%.3f  %s" %
              (d_close, "应用启动、菜单收起 ✓" if t2 else "菜单未收起 FAIL"))
        d_screen1 = diff(B, A1, FULL_BOX)
        t3a = d_screen1 > 0.01
        print("    T3a 启动计算器后全屏差异=%.3f  %s" %
              (d_screen1, "新窗口已绘制 ✓" if t3a else "画面无变化 FAIL"))
        # 再开一个不同 App（文件管理器），证明启动器对多个 App 都生效
        q.click(*START_XY); time.sleep(0.3)
        q.click(*FILES_XY)
        A2 = q.shot("files")
        d_screen2 = diff(A1, A2, FULL_BOX)
        t3b = d_screen2 > 0.01
        print("    T3b 再启动文件管理器全屏差异=%.3f  %s" %
              (d_screen2, "第二个窗口已绘制 ✓" if t3b else "画面无变化 FAIL"))
        ok = t1 and t2 and t3a and t3b
        print("===== 结果: T1=%s T2=%s T3a=%s T3b=%s -> %s =====" %
              ("PASS" if t1 else "FAIL", "PASS" if t2 else "FAIL",
               "PASS" if t3a else "FAIL", "PASS" if t3b else "FAIL",
               "ALL PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        if p.poll() is None:
            p.send_signal(9)


if __name__ == "__main__":
    sys.exit(main())
