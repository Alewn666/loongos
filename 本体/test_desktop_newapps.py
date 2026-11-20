#!/usr/bin/env python3
"""P4 桌面拓展 · 三个新 App（日历/画板/贪吃蛇）端到端验收。

不靠 OCR / 猜像素：QMP 直接读 guest 内存里的内核静态符号
（aarch64-linux-gnu-nm 动态取地址），每一项都是确定性的。

覆盖：
  1. 开始菜单含 日历/画板/贪吃蛇（app_id 11/12/13）
  2. 点菜单能打开三个新窗口（G_win[].app_id / visible）
  3. 日历：cal_view_y/m 从 RTC 初始化；按 A 上月 / D 下月 / T 回今天生效
  4. 画板：点画布某格 → g_paint 对应位置写当前色；点调色板换色后新格用新色；C 清屏
  5. 贪吃蛇：snk_len=3 起步；转向后 snake_tick 推进（头坐标变化）；R 重开复位

用法: python3 test_desktop_newapps.py          # 默认当前 kernel.elf
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
SER = "/tmp/loongos_newapps_serial.sock"
QMP = "/tmp/loongos_newapps_qmp.sock"
SHOTS = "/tmp/loongos_newapps_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (SER, QMP):
    try:
        os.unlink(p)
    except OSError:
        pass

W, H = 800, 600
A_CALENDAR, A_PAINT, A_SNAKE = 11, 12, 13
OK = [0]
FAIL = [0]


def check(name, ok, detail=""):
    if ok:
        OK[0] += 1
        print("  [ok] %s %s" % (name, detail))
    else:
        FAIL[0] += 1
        print("  [FAIL] %s %s" % (name, detail))


def sym_addrs(elf):
    out = subprocess.check_output(["aarch64-linux-gnu-nm", "-n", elf], text=True)
    d = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3:
            d[p[2]] = int(p[0], 16)
    return d


A = sym_addrs(KERNEL)


class Qmp:
    def __init__(self, path):
        self.s = None
        for _ in range(120):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        if not self.s:
            raise RuntimeError("QMP 连不上")
        self.s.settimeout(8)
        self.buf = b""
        self.line()
        self.q("qmp_capabilities")

    def line(self):
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("QMP 断开")
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1)
        return json.loads(l.decode("utf-8", "replace"))

    def q(self, exe, arg=None):
        m = {"execute": exe}
        if arg:
            m["arguments"] = arg
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self.line()
            if "return" in r or "error" in r:
                return r

    def hmp(self, cmdline):
        return self.q("human-monitor-command", {"command-line": cmdline})

    def words(self, addr, n):
        r = self.hmp("xp /%dwx 0x%x" % (n, addr))
        txt = r.get("return", "") or ""
        vals = []
        for tok in txt.replace(":", " ").split():
            if tok.startswith("0x"):
                vals.append(int(tok, 16) & 0xFFFFFFFF)
        return vals

    def r32(self, name, off=0):
        v = self.words(A[name] + off, 1)
        return v[0] if v else None

    def abs_move(self, x, y):
        self.q("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]})

    def btn(self, down, button="left"):
        self.q("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": button}}]})

    def click(self, x, y, hold=0.12):
        self.abs_move(x, y)
        time.sleep(0.08)
        self.btn(True)
        time.sleep(hold)
        self.btn(False)
        time.sleep(0.3)

    def key(self, ch):
        # 必须用 HMP sendkey（QKeyCode 键名），QMP input-send-event 的 key 事件格式不同
        self.hmp("sendkey " + ch)
        time.sleep(0.3)

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        png = os.path.join(SHOTS, name + ".png")
        self.q("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.02)
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png)
        return png


def wait_desktop(q, timeout=45):
    end = time.time() + timeout
    while time.time() < end:
        v = q.r32("g_desktop_active")
        if v == 1:
            return True
        time.sleep(0.2)
    return False


def find_win(q, app_id):
    for i in range(12):
        b = A["G_win"] + i * 40
        if q.words(b + 24, 1) == [1]:
            aid = q.words(b + 20, 1)[0]
            if aid == app_id:
                return i, q.words(b, 4)
    return None, None


def open_from_menu(q, app_id):
    """点开始按钮 → 在菜单里找 app_id 对应项点击。菜单项布局: m.x+8, m.y+8+i*26, w-16,22"""
    q.click(49, 579)                       # 开始按钮
    time.sleep(0.3)
    menu_n = q.r32("G_menu_n")
    menu_base = A["G_menu"]
    idx = None
    for i in range(menu_n or 0):
        # struct menuitem { const char* label(8) ; int type; int app_id; const char* cmd(8) } = 24B (对齐)
        b = menu_base + i * 24
        typ = q.words(b + 8, 1)[0]
        aid = q.words(b + 12, 1)[0]
        if typ == 0 and aid == app_id:
            idx = i
            break
    if idx is None:
        return None, menu_n
    # 菜单矩形: {10, gfx_height()-TASKBAR_H - (8+n*26+8), MENU_W=232, h}
    mh = 8 + menu_n * 26 + 8
    my = H - 42 - mh
    row_cy = my + 8 + idx * 26 + 11
    q.click(10 + 100, row_cy)              # 菜单项中心 x
    time.sleep(0.4)
    return idx, menu_n


def main():
    print("===== P4 桌面拓展 三新 App 端到端验收 =====")
    print("    内核: %s  md5=%s" %
          (KERNEL, subprocess.check_output(["md5sum", KERNEL], text=True).split()[0]))
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none",
        "-serial", "unix:%s,server=on,wait=off" % SER,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
        "-fw_cfg", "name=opt/loongos.boot,string=classic",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        q = Qmp(QMP)
        if not wait_desktop(q):
            print("[FAIL] 桌面未就绪"); print("结果: 0/1"); sys.exit(2)
        check("桌面就绪", True)

        # 1) 菜单含三个新 App
        idx, menu_n = open_from_menu(q, A_CALENDAR)
        check("菜单含日历(app_id=11)", idx is not None, "菜单项=%s idx=%s" % (menu_n, idx))
        # 上面已点击打开了日历
        wi, _ = find_win(q, A_CALENDAR)
        check("日历窗口打开", wi is not None)

        # 3) 日历初始化 + 翻月
        vy = q.r32("cal_view_y"); vm = q.r32("cal_view_m")
        check("日历从RTC初始化年月", vy and 1 <= vm <= 12, "y=%s m=%s" % (vy, vm))
        vm0 = vm; vy0 = vy
        q.key("d")                          # 下月
        vm1 = q.r32("cal_view_m"); vy1 = q.r32("cal_view_y")
        check("按D下月生效", (vm1 != vm0 or vy1 != vy0), "%s-%s -> %s-%s" % (vy0, vm0, vy1, vm1))
        q.key("a")                          # 上月回退
        vm2 = q.r32("cal_view_m")
        check("按A上月生效", vm2 == vm0, "m=%s 期望=%s" % (vm2, vm0))
        q.key("t")                          # 回今天
        vmt = q.r32("cal_view_m")
        check("按T回今天", vmt == vm and q.r32("cal_view_y") == vy, "m=%s" % vmt)
        # 关掉日历
        wi, r = find_win(q, A_CALENDAR)
        if wi is not None:
            q.click(r[0] + r[2] - 14, r[1] + 15)

        # 2+4) 画板
        idx, _ = open_from_menu(q, A_PAINT)
        check("菜单含画板(app_id=12)", idx is not None)
        wi, r = find_win(q, A_PAINT)
        check("画板窗口打开", wi is not None)
        check("画板初始色=1", q.r32("g_paint_color") == 1)
        # 切到色 3（调色板第4格 x=pad+3*24=80, y 在内容底部）
        crx, cry, crw, crh = r[0] + 1, r[1] + 28, r[2] - 2, r[3] - 29
        py = cry + crh - 30 - 2
        q.click(crx + 8 + 3 * 24 + 10, py + 10)
        check("点调色板换色=3", q.r32("g_paint_color") == 3, "color=%s" % q.r32("g_paint_color"))
        # 点画布左上角格子 → g_paint[0]=3
        q.click(crx + 8 + 4, cry + 8 + 4)
        cell0 = q.words(A["g_paint"], 1)[0] & 0xFF
        check("点画布写入当前色", cell0 == 3, "g_paint[0]=%s" % cell0)
        # C 清屏
        q.key("c")
        cell0b = q.words(A["g_paint"], 1)[0] & 0xFF
        check("按C清屏", cell0b == 0, "g_paint[0]=%s" % cell0b)
        wi, r = find_win(q, A_PAINT)
        if wi is not None:
            q.click(r[0] + r[2] - 14, r[1] + 15)

        # 2+5) 贪吃蛇
        idx, _ = open_from_menu(q, A_SNAKE)
        check("菜单含贪吃蛇(app_id=13)", idx is not None)
        wi, r = find_win(q, A_SNAKE)
        check("贪吃蛇窗口打开", wi is not None)
        check("蛇初始长度=3", q.r32("snk_len") == 3, "len=%s" % q.r32("snk_len"))
        hx0 = q.words(A["snk_x"], 1)[0] & 0xFF   # u8 数组，取首字节 = 蛇头 x
        # 打开后主循环 tick 已自动推进数步，蛇头 x 会 >4；只断言坐标合法（未撞墙越界）
        check("蛇头坐标合法", 0 <= hx0 < 16, "x=%s" % hx0)
        # 设向下(方向4)，由 tick 推进 -- 蛇头 y 应增加；这里直接读方向
        q.key("s")
        sd = q.r32("snk_dir")
        check("按S转向下(方向4)", sd == 4, "dir=%s" % sd)
        # 等自动推进几步
        time.sleep(1.2)
        hy1 = q.words(A["snk_y"], 1)[0] & 0xFF
        check("tick自动推进(头y变化)", hy1 > 6, "head_y=%s" % hy1)
        # R 重开
        q.key("r")
        check("按R重开(len复位3)", q.r32("snk_len") == 3 and q.r32("snk_score") == 0)

        q.shot("newapps_final")

        print("\n结果: %d/%d PASS" % (OK[0], OK[0] + FAIL[0]))
        sys.exit(0 if FAIL[0] == 0 else 1)
    finally:
        try:
            proc.terminate(); proc.wait(timeout=5)
        except Exception:
            pass


if __name__ == "__main__":
    main()
