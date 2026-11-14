#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""桌面重绘性能基线采集（配合内核 [gfxstat] 串口统计）。

固定操作序列（优化前后必须完全一致，才可比）：
  16 次指针移动（触发小区脏矩形重绘）→ 点开始/启动计算器（全屏重绘）→
  触屏「开始→终端」聚焦 → 6 步拖动窗口 → Ctrl+Shift+F1 退出 →
  读串口 [gfxstat] 行。

关注两个比值（与采样帧数无关）：
  avg_cycles_per_present    每帧重绘平均 CPU 周期，越低越好
  redraws_per_present_x100  每次 present 平均 redraw 次数，越低越好

用法: python3 diag_gfxstat.py <标签>
"""
import json, os, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release

BASE = os.path.dirname(os.path.abspath(__file__))
TAG = "gfxstat"
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass
W, H = 800, 600
LABEL = sys.argv[1] if len(sys.argv) > 1 else "run"


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

    def cmd(self, e, a=None):
        m = {"execute": e}
        if a: m["arguments"] = a
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r: return r

    def hmp(self, line):
        return self.cmd("human-monitor-command", {"command-line": line})


def _abs(x, y):
    return [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
    ]


def move(q, x, y):
    q.cmd("input-send-event", {"events": _abs(x, y)})


def btn(q, down):
    q.cmd("input-send-event", {"events":
        [{"type": "btn", "data": {"down": bool(down), "button": "left"}}]})


def tap(q, x, y):
    move(q, x, y); time.sleep(0.08)
    btn(q, True); time.sleep(0.05); btn(q, False); time.sleep(0.30)


def wait_boot(timeout=45):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if "图形桌面已就绪".encode("utf-8") in open(LOG, "rb").read():
                return True
        except OSError: pass
        time.sleep(0.15)
    return False


def wait_log(pat, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pat in open(LOG, "rb").read().decode("utf-8", "replace"):
                return True
        except OSError: pass
        time.sleep(0.15)
    return False


def main():
    if not preflight(need_mb=400, label="diag_gfxstat"):
        return 2
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
        "-m", "256", "-smp", "1",
        "-kernel", os.path.join(BASE, "kernel.elf"),
        "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_boot():
            print("[%s] 未等到桌面就绪" % LABEL); return 1
        q = Qmp(QMP)
        time.sleep(0.5)

        # ① 16 次指针移动：每次只该产生一个「光标小区」脏矩形
        for i in range(16):
            move(q, 300 + (i % 8) * 20, 200 + (i % 4) * 15)
            time.sleep(0.12)

        # ② 点开始 → 启动计算器（全屏 damage 路径）
        tap(q, 49, 579)
        tap(q, 126, 353)

        # ③ 触屏「开始→终端」把终端聚焦到顶层
        tap(q, 49, 579)
        tap(q, 126, 249)

        # ④ 6 步拖动窗口
        move(q, 200, 55); time.sleep(0.10)
        btn(q, True); time.sleep(0.20)
        for (x, y) in [(230, 85), (260, 115), (290, 145),
                       (320, 175), (350, 205), (380, 235)]:
            move(q, x, y); time.sleep(0.12)
        btn(q, False); time.sleep(0.30)

        # ⑤ 退出桌面，触发 [gfxstat] 串口打印
        q.hmp("sendkey ctrl-shift-f1")
        if not wait_log("[gfxstat]", 15):
            print("[%s] 未读到 [gfxstat] 行" % LABEL); return 1
        txt = open(LOG, "rb").read().decode("utf-8", "replace")
        lines = [l for l in txt.splitlines() if l.startswith("[gfxstat]")]
        print("[%s] %s" % (LABEL, lines[-1]))
        return 0
    finally:
        if p.poll() is None:
            p.send_signal(9)
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: pass
        release()


if __name__ == "__main__":
    sys.exit(main())
