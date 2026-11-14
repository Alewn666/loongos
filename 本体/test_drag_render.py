#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""桌面拖动渲染回归 / 诊断（2026-11-14）。

用户反馈：桌面「有时候卡死」，且「移动窗口的时候有些元素会消失」。
本脚本把一次窗口拖动拆成 8 个中间帧逐帧抓图，用像素证据定位：

判定 A（正确性，PASS/FAIL）：
  顶栏 (0,0,800,38) 与任务栏左侧 (0,558,590,42) 在拖动过程中**本应完全
  静止**——窗口只在中部移动、光标也只在中部。任一中间帧这两个区域相对
  pre_drag 出现显著变化，就说明固定 UI 被漏画/覆盖（用户说的「元素消失」）。
  时钟区 (x>=604) 已排除（每秒刷新）；起始按钮 hover 不在拖动路径上。

判定 B（交互未卡死）：拖动结束后再点「开始」，菜单必须仍能展开。

判定 C（性能基线，仅报告数值，不做阈值断言）：
  记录每帧 screendump 往返耗时与拖动总耗时，用于修复前后 A/B 对比。

基准图 = 「按下标题栏、窗口已聚焦但尚未移动」的 pre_drag，避开聚焦导致的
任务栏按钮换色这一**预期**变化。

坐标（800x600）：初始 4 窗口按 G_open_seq 错位，最顶层「操作说明」
{118,116,380,300}；终端 {40,44,460,340} 标题栏 y∈[44,72]，
(200,55) 只被终端覆盖（A_INFO 从 y=68 起），点它必然抓住终端标题栏。
"""
import json, os, socket, subprocess, sys, time
from PIL import Image, ImageChops
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release

BASE = os.path.dirname(os.path.abspath(__file__))
TAG = "dragrender"
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
SHOTS = "/tmp/loongos_%s_shots" % TAG
OVERVIEW = os.path.join(BASE, "拖动逐帧诊断.png")
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass

W, H = 800, 600
TERM_TITLE = (200, 55)
DRAG_PATH = [(230, 85), (260, 115), (290, 145), (320, 175),
             (350, 205), (380, 235), (410, 265), (440, 295)]
TOPBAR_BOX = (0, 0, 800, 38)
TASKBAR_BOX = (0, 558, 590, 600)   # PIL crop 格式 (l,u,r,l)，排除 x>=604 的时钟区
MENU_BOX = (12, 232, 240, 556)
START = (49, 579)
UI_TOL = 0.02          # 固定 UI 区域允许的差异（理论应为 0）


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

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        png = os.path.join(SHOTS, name + ".png")
        try: os.unlink(ppm)
        except OSError: pass
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.02)
        Image.open(ppm).convert("RGB").save(png)
        return png


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
    btn(q, True); time.sleep(0.05); btn(q, False); time.sleep(0.35)


def diff(a, b, bx):
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


def build_overview(frames, labels, scores):
    """拖动逐帧缩略网格，标题栏标出该帧固定 UI 差异分数。"""
    from PIL import ImageDraw, ImageFont
    tw, th = 320, 240
    cols = 3
    rows = (len(frames) + cols - 1) // cols
    pad, bar = 8, 20
    cw, ch = tw + pad * 2, th + bar + pad * 2
    canvas = Image.new("RGB", (cols * cw, rows * ch), (24, 28, 36))
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    except OSError:
        font = ImageFont.load_default()
    for i, (png, lab, sc) in enumerate(zip(frames, labels, scores)):
        r, c = divmod(i, cols)
        ox, oy = c * cw, r * ch
        bad = sc is not None and (sc[0] > UI_TOL or sc[1] > UI_TOL)
        color = (170, 50, 50) if bad else (40, 90, 150)
        draw.rectangle([ox + pad, oy + pad, ox + pad + tw, oy + pad + bar], fill=color)
        txt = lab if sc is None else "%s  top=%.3f bar=%.3f" % (lab, sc[0], sc[1])
        draw.text((ox + pad + 4, oy + pad + 3), txt, fill=(255, 255, 255), font=font)
        canvas.paste(Image.open(png).convert("RGB").resize((tw, th)),
                     (ox + pad, oy + pad + bar))
    canvas.save(OVERVIEW)
    return OVERVIEW


def main():
    if not preflight(need_mb=400, label="test_drag_render"):
        return 2
    print("===== 桌面拖动渲染诊断  TAG=dragrender =====")
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
    fails, frames, labels, scores, timings = [], [], [], [], []
    try:
        if not wait_boot():
            print("  T0 未等到桌面就绪，FAIL"); return 1
        q = Qmp(QMP)
        time.sleep(0.6)

        # 抓住终端标题栏、聚焦，但先不移动 → 作为基准
        move(q, *TERM_TITLE); time.sleep(0.15)
        btn(q, True); time.sleep(0.35)
        pre = q.shot("00_pre_drag")
        frames.append(pre); labels.append("00 pre-drag (base)"); scores.append(None)

        t_start = time.monotonic()
        worst_top = worst_bar = 0.0
        for i, (x, y) in enumerate(DRAG_PATH):
            t0 = time.monotonic()
            move(q, x, y)
            time.sleep(0.12)                 # 给内核一帧重绘时间
            f = q.shot("01_drag_%02d" % i)
            dt = time.monotonic() - t0
            timings.append(dt)
            dt_top = diff(pre, f, TOPBAR_BOX)
            dt_bar = diff(pre, f, TASKBAR_BOX)
            worst_top = max(worst_top, dt_top)
            worst_bar = max(worst_bar, dt_bar)
            frames.append(f); labels.append("%02d drag" % (i + 1))
            scores.append((dt_top, dt_bar))
            flag = "BAD" if (dt_top > UI_TOL or dt_bar > UI_TOL) else "ok"
            print("  帧%02d  top=%.4f bar=%.4f  %s  往返=%.3fs" %
                  (i + 1, dt_top, dt_bar, flag, dt))
            if flag == "BAD":
                fails.append("拖动第 %d 帧固定 UI 被破坏 (top=%.3f bar=%.3f)" %
                             (i + 1, dt_top, dt_bar))
        total = time.monotonic() - t_start
        btn(q, False); time.sleep(0.45)
        final = q.shot("02_final")
        frames.append(final); labels.append("final"); scores.append(None)

        print("  A 固定 UI 最差差异: 顶栏=%.4f 任务栏=%.4f (阈值 %.2f)" %
              (worst_top, worst_bar, UI_TOL))
        print("  C 性能: 拖动 %d 步共 %.3fs, 每步往返 avg=%.3fs max=%.3fs" %
              (len(timings), total, sum(timings) / len(timings), max(timings)))

        # B 拖动后桌面仍可交互：点开始菜单应能展开
        tap(q, *START)
        after = q.shot("03_menu_after")
        frames.append(after); labels.append("tap start"); scores.append(None)
        d_menu = diff(final, after, MENU_BOX)
        b_ok = d_menu > 0.05
        print("  B 拖动后点「开始」→ 菜单区差异=%.3f  %s" %
              (d_menu, "未卡死 ✓" if b_ok else "无响应 FAIL"))
        if not b_ok: fails.append("拖动后桌面无响应（卡死）")

        build_overview(frames, labels, scores)
        print("    逐帧总览: %s" % OVERVIEW)
        ok = not fails
        print("===== 结果: %s =====" %
              ("ALL PASS" if ok else "FAIL -> " + " | ".join(fails)))
        return 0 if ok else 1
    finally:
        if p.poll() is None:
            p.send_signal(9)
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: pass
        release()


if __name__ == "__main__":
    sys.exit(main())
