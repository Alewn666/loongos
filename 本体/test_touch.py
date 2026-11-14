#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 手机触屏场景走查（2026-11-14）。

背景：用户真机是「手机上跑虚拟机」，输入只有手指触屏——没有物理鼠标、
没有物理键盘。本脚本把「手机触屏」翻译成 QEMU 能注入的等价事件，走查
桌面能否纯靠手指操作，并抓图给用户直观看「手指点出来的画面」。

触屏 tap 与桌面鼠标点击的三点关键差异（本测试专门复刻，勿简化成分离式 click）：
  1. 手指「按下即带坐标」：坐标(abs) 与 按下(btn down) 在**同一个事件批**里
     到达，不像鼠标那样先移动光标、隔一会儿再按下。若内核先处理按下再
     更新坐标，第一下就会点偏（点到上一次/屏幕中央）。
  2. 按下与抬起间隔极短（短促 tap），一批 HID 报告里可能 down/up 都发生 ——
     靠按钮边沿队列(P3.2)才不丢；纯状态快照会吞掉中间边沿。
  3. 触屏没有 hover：手指离开屏幕后没有坐标事件，光标停在最后点击处。

判定（全程单批事件 tap，不掺桌面热键）：
  T0 桌面就绪（串口 "图形桌面已就绪"）
  T1 触屏单批 tap 点「开始」→ 菜单展开（MENU_BOX 显著变化）
  T2 触屏单批 tap 点菜单「计算器」→ 菜单收起 + 新窗口绘制
  T3 连续触屏 tap（开始→文件管理器）→ 再开一窗（多次短 tap 边沿不丢）
  T4 触屏「开始→终端」把终端抬到顶层并聚焦 → 按住其标题栏连续滑动 →
     抬起 → 窗口跟手移动
  T5 手机无物理键盘：软键盘事件(HID)敲 whoami → 触屏聚焦的终端输出 root

坐标依源码实算（800x600, TASKBAR_H=42, MENU_ROW=26, NAPP=10+关机+重启=12项）：
  菜单 h=8+12*26+8=328, m={10,230,232,328}; 项 i 中心 (126, 249+i*26)
    A_TERM=0 → (126,249)   A_CALC=4 → (126,353)   A_FILES=8 → (126,457)
  开始按钮 {10,566,78,26} → (49,579)
  终端窗口 {40,44,460,340}, 标题栏 y∈[44,72] → (200,55)
⚠️ 连续开 App 会不断改变窗口堆叠，T1~T3 之后 (200,130) 已落在新开的
  「文件管理器」内容区而非任何窗口标题栏。因此 T4/T5 不猜堆叠：先用触屏
  「开始→终端」把既有终端(single 实例)抬到最顶层并聚焦，之后 (200,55)
  确定只被终端标题栏覆盖，拖动与打字都基于这个确定状态。
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release

BASE = os.path.dirname(os.path.abspath(__file__))
TAG = "touch"
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
SHOTS = "/tmp/loongos_%s_shots" % TAG
OVERVIEW = os.path.join(BASE, "触屏走查总览.png")
os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass

W, H = 800, 600
START = (49, 579)
CALC = (126, 353)
FILES = (126, 457)
TERM_TITLE = (200, 55)       # 终端标题栏（聚焦到顶层后，仅终端覆盖此点）
TERM_MENU = (126, 249)       # 开始菜单第 1 项「终端」
DRAG_PATH = [(240, 95), (280, 135), (320, 175)]   # 从 (200,55) 往右下滑动
MENU_BOX = (12, 232, 240, 556)
FULL_BOX = (0, 0, W, H - 42)


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


def tap(q, x, y, settle=0.35):
    """触屏单批 tap：坐标 + 按下 放在同一个 input-send-event（手指按下即带坐标）。

    按下后仅隔 0.02s 就抬起，复刻手机短促 tap；不先单独移动光标。"""
    q.cmd("input-send-event", {"events":
        _abs(x, y) + [{"type": "btn", "data": {"down": True, "button": "left"}}]})
    time.sleep(0.02)
    q.cmd("input-send-event", {"events":
        [{"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(settle)


def drag(q, x0, y0, path, settle=0.45):
    """触屏拖动：按下(带坐标) → 连续 abs 滑过 path（保持按住）→ 抬起。"""
    q.cmd("input-send-event", {"events":
        _abs(x0, y0) + [{"type": "btn", "data": {"down": True, "button": "left"}}]})
    time.sleep(0.05)
    for (x, y) in path:
        q.cmd("input-send-event", {"events": _abs(x, y)})
        time.sleep(0.07)
    time.sleep(0.1)
    q.cmd("input-send-event", {"events":
        [{"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(settle)


def diff(a, b, bx):
    from PIL import ImageChops
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


def log_text():
    try: return open(LOG, "rb").read().decode("utf-8", "replace")
    except OSError: return ""


def wait_log(pat, timeout=10, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if pat in log_text()[since:]: return True
        time.sleep(0.15)
    return False


def build_overview(shots):
    """把 6 张 800x600 缩成总览网格（英文编号，避免依赖中文字体）。"""
    from PIL import ImageDraw, ImageFont
    tw, th = 400, 300
    labels = [
        "1 Desktop (boot)",
        "2 Tap 'Start' -> menu",
        "3 Tap 'Calculator'",
        "4 Tap 'Files' again",
        "5 Drag a window",
        "6 Soft-kbd: whoami -> root",
    ]
    cols, rows = 2, 3
    pad, bar = 10, 22
    cw, ch = tw + pad * 2, th + bar + pad * 2
    canvas = Image.new("RGB", (cols * cw, rows * ch), (24, 28, 36))
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
    except OSError:
        font = ImageFont.load_default()
    for i, (png, label) in enumerate(zip(shots, labels)):
        r, c = divmod(i, cols)
        ox, oy = c * cw, r * ch
        draw.rectangle([ox + pad, oy + pad, ox + pad + tw, oy + pad + bar], fill=(40, 90, 150))
        draw.text((ox + pad + 6, oy + pad + 3), label, fill=(255, 255, 255), font=font)
        im = Image.open(png).convert("RGB").resize((tw, th))
        canvas.paste(im, (ox + pad, oy + pad + bar))
    canvas.save(OVERVIEW)
    return OVERVIEW


def main():
    if not preflight(need_mb=400, label="test_touch"):
        return 2
    print("===== 手机触屏场景走查  TAG=touch =====")
    print("    内核: %s  md5=%s" %
          (os.path.join(BASE, "kernel.elf"),
           subprocess.check_output(["md5sum", os.path.join(BASE, "kernel.elf")], text=True).split()[0]))
    print("    触屏事件形态: abs+按下 同批到达 / 短促 tap / 无 hover")
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
    fails, shots = [], []
    try:
        if not wait_boot():
            print("  T0 未等到桌面就绪，FAIL"); return 1
        print("  T0 桌面就绪 ✓")
        q = Qmp(QMP)
        time.sleep(0.6)

        B = q.shot("00_base"); shots.append(B)
        tap(q, *START)                                  # ① 单批 tap 点「开始」
        M = q.shot("01_menu"); shots.append(M)
        d1 = diff(B, M, MENU_BOX)
        t1 = d1 > 0.05
        print("  T1 触屏单批 tap 点开始 → 菜单区差异=%.3f  %s" %
              (d1, "菜单展开 ✓" if t1 else "无反应 FAIL"))
        if not t1: fails.append("T1 触屏 tap 未展开开始菜单")

        tap(q, *CALC)                                   # ② 单批 tap 点「计算器」
        A1 = q.shot("02_calc"); shots.append(A1)
        d2m = diff(M, A1, MENU_BOX); d2s = diff(B, A1, FULL_BOX)
        t2 = d2m > 0.05 and d2s > 0.01
        print("  T2 触屏 tap 点计算器 → 菜单收起=%.3f 全屏变化=%.3f  %s" %
              (d2m, d2s, "应用启动 ✓" if t2 else "FAIL"))
        if not t2: fails.append("T2 触屏 tap 未启动计算器")

        tap(q, *START); time.sleep(0.3)                 # ③ 连续第二次 tap
        tap(q, *FILES)
        A2 = q.shot("03_files"); shots.append(A2)
        d3 = diff(A1, A2, FULL_BOX)
        t3 = d3 > 0.01
        print("  T3 连续触屏 tap 再开文件管理器 → 全屏差异=%.3f  %s" %
              (d3, "第二窗口已绘制 ✓" if t3 else "FAIL"))
        if not t3: fails.append("T3 连续 tap 未开第二个窗口")

        # 连续开窗后堆叠已变，先用触屏「开始→终端」把既有终端抬到顶层并聚焦，
        # 让拖动起点落在确定的窗口标题栏上（否则会戳到别的窗口内容区）。
        tap(q, *START); time.sleep(0.3)
        tap(q, *TERM_MENU); time.sleep(0.3)
        DB = q.shot("04a_before_drag")
        drag(q, *TERM_TITLE, DRAG_PATH)                 # ④ 触屏按住滑动
        DA = q.shot("04_dragged"); shots.append(DA)
        d4 = diff(DB, DA, FULL_BOX)
        t4 = d4 > 0.03
        print("  T4 触屏拖动窗口 → 全屏差异=%.3f  %s" %
              (d4, "窗口跟手移动 ✓" if t4 else "未移动 FAIL"))
        if not t4: fails.append("T4 触屏拖动未移动窗口")

        # ⑤ 手机无物理键盘：终端已在 T4 被聚焦；不再点 (200,55)——窗口已被
        #    拖走，那个点不再是终端。直接注入软键盘(HID)事件。
        #    since 用「字符数」而非字节数：串口日志含中文，UTF-8 下会错位。
        mark = len(log_text())
        for ch in "whoami":
            q.hmp("sendkey " + ch); time.sleep(0.12)
        q.hmp("sendkey ret"); time.sleep(0.6)
        S = q.shot("05_term_typed"); shots.append(S)
        t5 = wait_log("root", 8, since=mark)
        print("  T5 触屏聚焦终端 + 软键盘敲 whoami → %s" %
              ("输出 root ✓" if t5 else "无输出 FAIL"))
        if not t5: fails.append("T5 软键盘事件未能在触屏聚焦终端中打字")

        ov = build_overview(shots)
        print("    总览图: %s" % ov)
        ok = t1 and t2 and t3 and t4 and t5
        print("===== 结果: %s =====" % ("ALL PASS (5/5)" if ok else "FAIL " + "; ".join(fails)))
        return 0 if ok else 1
    finally:
        if p.poll() is None:
            p.send_signal(9)
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: pass
        release()


if __name__ == "__main__":
    sys.exit(main())
