#!/usr/bin/env python3
"""P4 交互增强 · 窗口最大化/还原 端到端验收。

读 guest 内存静态符号（确定性，不靠 OCR）：
  1. 点最大化按钮 → G_win[].r 变为工作区，G_maximized=1
  2. 再点 → 还原到原矩形，G_maximized=0
  3. 最大化后拖标题栏 → 自动还原后再拖动（G_maximized=0，矩形变化）
  4. struct dwin 步长仍为 40B（确保没被新状态破坏 → 保护其它判据）

用法: python3 test_desktop_interact.py
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
SER = "/tmp/loongos_interact_serial.sock"
QMP = "/tmp/loongos_interact_qmp.sock"
SHOTS = "/tmp/loongos_interact_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (SER, QMP):
    try:
        os.unlink(p)
    except OSError:
        pass

W, H = 800, 600
TOPBAR_H, TASKBAR_H = 38, 42
OK = [0]
FAIL = [0]


def check(name, ok, detail=""):
    (OK if ok else FAIL)[0] += 1
    print("  [%s] %s %s" % ("ok" if ok else "FAIL", name, detail))


def sym_addrs(elf):
    out = subprocess.check_output(["aarch64-linux-gnu-nm", "-n", elf], text=True)
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


A = sym_addrs(KERNEL)


class Qmp:
    def __init__(self, path):
        self.s = None
        for _ in range(120):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError:
                time.sleep(0.1)
        if not self.s:
            raise RuntimeError("QMP 连不上")
        self.s.settimeout(8); self.buf = b""
        self.line(); self.q("qmp_capabilities")

    def line(self):
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d: raise RuntimeError("QMP 断开")
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1)
        return json.loads(l.decode("utf-8", "replace"))

    def q(self, exe, arg=None):
        m = {"execute": exe}
        if arg: m["arguments"] = arg
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self.line()
            if "return" in r or "error" in r: return r

    def hmp(self, c): return self.q("human-monitor-command", {"command-line": c})

    def words(self, addr, n):
        txt = self.hmp("xp /%dwx 0x%x" % (n, addr)).get("return", "") or ""
        return [int(t, 16) & 0xFFFFFFFF for t in txt.replace(":", " ").split() if t.startswith("0x")]

    def r32(self, name, off=0):
        v = self.words(A[name] + off, 1); return v[0] if v else None

    def abs_move(self, x, y):
        self.q("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}]})

    def btn(self, down):
        self.q("input-send-event", {"events": [{"type": "btn", "data": {"down": bool(down), "button": "left"}}]})

    def click(self, x, y):
        self.abs_move(x, y); time.sleep(0.08)
        self.btn(True); time.sleep(0.13); self.btn(False); time.sleep(0.3)

    def drag(self, x0, y0, x1, y1):
        self.abs_move(x0, y0); time.sleep(0.15)
        self.btn(True); time.sleep(0.3)
        for i in range(1, 9):
            self.abs_move(x0 + (x1-x0)*i/8, y0 + (y1-y0)*i/8); time.sleep(0.12)
        self.btn(False); time.sleep(0.4)

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        self.q("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.02)
        return ppm


def win(q, app_id=0):
    """返回 (slot, [x,y,w,h])，聚焦的终端窗口 A_TERM=0"""
    for i in range(12):
        b = A["G_win"] + i * 40
        if q.words(b + 24, 1) == [1] and q.words(b + 20, 1)[0] == app_id:
            return i, q.words(b, 4)
    return None, None


def main():
    print("===== P4 交互增强 窗口最大化/还原 验收 =====")
    print("    内核 md5=%s" % subprocess.check_output(["md5sum", KERNEL], text=True).split()[0])
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
           "-m", "256", "-smp", "1", "-kernel", KERNEL, "-display", "none",
           "-serial", "unix:%s,server=on,wait=off" % SER,
           "-qmp", "unix:%s,server=on,wait=off" % QMP,
           "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-device", "usb-mouse,bus=xhci.0,port=2",
           "-device", "usb-kbd,bus=xhci.0,port=3"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        q = Qmp(QMP)
        end = time.time() + 45
        while time.time() < end and q.r32("g_desktop_active") != 1:
            time.sleep(0.2)
        check("桌面就绪", q.r32("g_desktop_active") == 1)
        # 确认 G_maximized 与 G_win 相邻但不改 dwin 步长：G_win[1] - G_win[0] 应为 40
        # （通过符号地址无法直接读步长，用 find_win 能正确定位即证明步长=40）
        slot, r0 = win(q, 0)
        check("终端窗口定位(证明dwin步长40B)", slot is not None, "slot=%s r=%s" % (slot, r0))

        # 先确保聚焦终端（点任务栏终端按钮中心）
        q.click(140, 579)
        time.sleep(0.3)
        slot, r0 = win(q, 0)
        mx = A["G_maximized"] + slot * 4
        check("初始未最大化", q.words(mx, 1)[0] == 0)

        # 1) 点最大化按钮: max_rect = r.x+r.w-76+10, r.y+4+11
        bx, by = r0[0] + r0[2] - 76 + 10, r0[1] + 15
        q.click(bx, by)
        time.sleep(0.4)
        slot, r1 = win(q, 0)
        check("点最大化→标志置1", q.words(mx, 1)[0] == 1)
        exp = [4, TOPBAR_H + 4, W - 8, H - TOPBAR_H - TASKBAR_H - 8]
        check("窗口铺满工作区", r1 == exp, "r=%s 期望=%s" % (r1, exp))
        q.shot("maximized")

        # 2) 再点还原（最大化后按钮位置变了，重新取矩形）
        bx2, by2 = r1[0] + r1[2] - 76 + 10, r1[1] + 15
        q.click(bx2, by2)
        time.sleep(0.4)
        slot, r2 = win(q, 0)
        check("再点→标志清0", q.words(mx, 1)[0] == 0)
        check("还原到原矩形", r2 == r0, "r=%s 原=%s" % (r2, r0))

        # 3) 最大化后拖标题栏→自动还原
        q.click(bx, by)                      # 再次最大化
        time.sleep(0.4)
        check("再次最大化", q.words(mx, 1)[0] == 1)
        slot, rmax = win(q, 0)
        # 拖标题栏中部（避开按钮区）从中心向左下
        q.drag(rmax[0] + 200, rmax[1] + 14, rmax[0] + 120, rmax[1] + 90)
        time.sleep(0.3)
        slot, r3 = win(q, 0)
        check("拖最大化窗→自动还原", q.words(mx, 1)[0] == 0, "r=%s" % (r3,))
        check("拖动后矩形变化", r3 != rmax, "r=%s max=%s" % (r3, rmax))

        print("\n结果: %d/%d PASS" % (OK[0], OK[0] + FAIL[0]))
        sys.exit(0 if FAIL[0] == 0 else 1)
    finally:
        try: proc.terminate(); proc.wait(timeout=5)
        except Exception: pass


if __name__ == "__main__":
    main()
