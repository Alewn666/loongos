#!/usr/bin/env python3
"""拖拽诊断探针：定向给 QEMU HID Tablet 发事件，每批同时带 abs 坐标 + 按钮状态。

目的：区分「测试脚本注入方式问题」与「内核拖拽代码问题」。
QMP input-send-event 每次调用是独立批次，且默认广播给所有输入 handler；
只发 btn 不带 abs 时 tablet 与 mouse 都会报。这里改用 device 定向 + 完整状态。
"""
import json, os, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
LOG = "/tmp/loongos_drag_serial.log"
QMP = "/tmp/loongos_drag_qmp.sock"
SHOTS = "/tmp/loongos_drag_shots"
W, H = 800, 600
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass


def log():
    try: return open(LOG, "rb").read().decode("utf-8", "replace")
    except OSError: return ""


class Q:
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
        while b"\n" not in self.buf: self.buf += self.s.recv(65536)
        l, self.buf = self.buf.split(b"\n", 1)
        return json.loads(l.decode("utf-8", "replace"))

    def cmd(self, ex, args=None):
        m = {"execute": ex}
        if args: m["arguments"] = args
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                if "error" in r: print("  [QMP error]", ex, r["error"])
                return r

    def ev(self, x, y, down, device=None):
        """一批事件里同时带绝对坐标 + 左键状态（可定向到 tablet）"""
        vx = int(x * 32767 / (W - 1)); vy = int(y * 32767 / (H - 1))
        evs = [
            {"type": "abs", "data": {"axis": "x", "value": vx}},
            {"type": "abs", "data": {"axis": "y", "value": vy}},
            {"type": "btn", "data": {"down": bool(down), "button": "left"}},
        ]
        a = {"events": evs}
        if device: a["device"] = device
        return self.cmd("input-send-event", a)

    def shot(self, n):
        ppm = os.path.join(SHOTS, n + ".ppm")
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.05)
        from PIL import Image
        Image.open(ppm).convert("RGB").save(os.path.join(SHOTS, n + ".png"))
        return os.path.join(SHOTS, n + ".png")


def diff(a, b):
    from PIL import Image, ImageChops
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    h = ImageChops.difference(ia, ib).convert("L").histogram()
    return sum(h[12:]) / float(ia.size[0] * ia.size[1])


def main():
    # None = 广播给所有输入 handler（tablet 收 ABS+BTN，mouse 只收 BTN、忽略 ABS）
    # 也可传 QOM id（例如给 -device usb-tablet,id=tab0 后传 "tab0"）
    dev = next((a for a in sys.argv[1:] if not a.startswith("--")), None)
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2",
           "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
           "-kernel", os.path.join(BASE, "kernel.elf"),
           "-display", "none", "-serial", "file:" + LOG,
           "-qmp", "unix:%s,server=on,wait=off" % QMP,
           "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1"]
    mouse_dev = ["-device", "usb-mouse,bus=xhci.0,port=2"] if "--no-mouse" not in sys.argv else []
    kbd_dev = ["-device", "usb-kbd,bus=xhci.0,port=3"]
    cmd += mouse_dev + kbd_dev
    print("[drag] device=%r" % dev)
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        end = time.time() + 40
        while "[desktop] 图形桌面已就绪" not in log() and time.time() < end:
            time.sleep(0.15)
        print("[drag] 桌面就绪:", "[desktop] 图形桌面已就绪" in log())
        q = Q(QMP)
        # 先移动到标题栏，停一拍让内核看到"未按下"的帧
        q.ev(300, 56, False, dev); time.sleep(0.4)
        q.shot("before")
        # 按下：一批同时给坐标 + down
        q.ev(300, 56, True, dev); time.sleep(0.45)
        q.shot("pressed")
        # 拖动：每批都保持 down，坐标递增，间隔拉长（10Hz timer + wfi）
        for i in range(1, 7):
            q.ev(300 + i * 26, 56 + i * 22, True, dev)
            time.sleep(0.3)
        q.shot("dragged")
        # 松开
        q.ev(456, 188, False, dev); time.sleep(0.4)
        q.shot("released")
        print("[drag] before->pressed 差异 = %.4f" % diff(
            os.path.join(SHOTS, "before.png"), os.path.join(SHOTS, "pressed.png")))
        print("[drag] before->dragged 差异 = %.4f" % diff(
            os.path.join(SHOTS, "before.png"), os.path.join(SHOTS, "dragged.png")))
        print("[drag] before->released 差异 = %.4f" % diff(
            os.path.join(SHOTS, "before.png"), os.path.join(SHOTS, "released.png")))
        print("[drag] 截图:", SHOTS)
    finally:
        p.terminate()
        try: p.wait(timeout=5)
        except Exception: p.kill()


if __name__ == "__main__":
    main()
