#!/usr/bin/env python3
# LoongOS x86_64 · 端到端交互回归
# ----------------------------------------------------------------------------
# 验证三件真机最关心的事（都是旧 ARM 版翻过车的地方）：
#   1) 纯鼠标移动（不点击）光标必须实时跟随 —— 旧版「要点击才知道位置」；
#   2) 键盘打字必须回显到图形终端 —— EV_SYN 帧 + 按键位图 + ASCII 映射；
#   3) Ctrl+Shift+F1 必须退出桌面 —— 修饰键状态机（位图，非临时字节）。
# 手段：QMP input-send-event 注入真实 PS/2 事件，screendump 抓图做像素比对。
import socket, json, subprocess, time, os, sys
from PIL import Image, ImageChops

ISO = os.path.join(os.path.dirname(__file__), "iso", "loongos_x86.iso")
SOCK = "/tmp/loongos_x86_test.sock"
SER  = "/tmp/loongos_x86_test_serial.log"
SHOTS= "/tmp/loongos_x86_shots"
os.makedirs(SHOTS, exist_ok=True)

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try: self.s.connect(path); break
            except OSError: time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")
    def cmd(self, exe, arg=None, id_=None):
        o = {"execute": exe}
        if arg is not None: o["arguments"] = arg
        self.f.write(json.dumps(o) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" in r: continue
            return r
    # ---- 输入注入 ----
    def rel_move(self, dx, dy):
        """PS/2 相对鼠标位移（不点击）"""
        ev = []
        if dx: ev.append({"type":"rel","data":{"axis":"x","value":dx}})
        if dy: ev.append({"type":"rel","data":{"axis":"y","value":dy}})
        if ev: self.cmd("input-send-event", {"events": ev})
    def abs_move(self, x, y):
        self.cmd("input-send-event", {"events":[
            {"type":"abs","data":{"axis":"x","value":x}},
            {"type":"abs","data":{"axis":"y","value":y}}]})
    def btn(self, down):
        self.cmd("input-send-event", {"events":[
            {"type":"btn","data":{"button":"left","down":down}}]})
    def key(self, qcode, down=True):
        self.cmd("input-send-event", {"events":[
            {"type":"key","data":{"key":{"type":"qcode","data":qcode},"down":down}}]})
    def tap(self, qcode):
        self.key(qcode, True); time.sleep(0.03); self.key(qcode, False)
    def shot(self, name):
        p = os.path.join(SHOTS, name + ".ppm")
        self.cmd("screendump", {"filename": p})
        for _ in range(30):
            if os.path.exists(p) and os.path.getsize(p) > 1000: break
            time.sleep(0.1)
        im = Image.open(p).convert("RGB")
        im.save(os.path.join(SHOTS, name + ".png"))
        return im
    def close(self):
        try: self.s.close()
        except Exception: pass

def diff(a, b, box=None):
    """两图差异像素数（可限定区域）"""
    if box: a, b = a.crop(box), b.crop(box)
    d = ImageChops.difference(a, b)
    return sum(1 for px in d.getdata() if px != (0,0,0))

# 光标是白色/描边，出现在 (cx,cy)~(cx+11,cy+16) 的小区域。
# 我们用「整屏差异」判断有没有重绘；用「指定区域是否出现亮色」判断光标到位。
def bright_in(im, box):
    sub = im.crop(box)
    return sum(1 for (r,g,b) in sub.getdata() if r>200 and g>200 and b>200)

def main():
    ok = True
    def report(name, cond, extra=""):
        nonlocal ok
        print(("  [PASS] " if cond else "  [FAIL] ") + name + ("  " + extra if extra else ""))
        if not cond: ok = False

    for f in (SOCK, SER):
        try: os.remove(f)
        except OSError: pass

    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "256M", "-cdrom", ISO,
        "-serial", "file:" + SER, "-qmp", "unix:" + SOCK + ",server=on,wait=off",
        "-no-reboot", "-display", "none", "-vga", "std",
    ], stdout=open("/tmp/loongos_x86_test_qemu.log","w"), stderr=subprocess.STDOUT)
    try:
        print("=== 等待内核启动 ===")
        time.sleep(6)
        c = QMP(SOCK)

        print("\n[1] 初始桌面")
        s0 = c.shot("01_initial")
        report("初始画面非纯色（桌面已绘制）",
               diff(s0, Image.new("RGB", s0.size, s0.getpixel((0,300)))) > 5000)

        # ---------- 2. 纯相对移动（不点击），光标必须跟随 ----------
        print("\n[2] 纯鼠标移动（不点击）实时跟随 —— 核心回归点")
        # 连续多次相对位移，累计把光标从中心移到右下
        for _ in range(6):
            c.rel_move(30, 20)
            time.sleep(0.12)
        time.sleep(0.5)
        s1 = c.shot("02_rel_moved")
        d_move = diff(s0, s1)
        report("纯移动后画面发生变化（光标/轨迹重绘）", d_move > 200,
               "diff=%d px" % d_move)
        # 轨迹线是青色 (0,224,224)，统计整屏青色像素是否出现
        cyan = sum(1 for (r,g,bl) in s1.getdata() if g>150 and bl>150 and r<80)
        report("出现青色鼠标轨迹（证明坐标在推进）", cyan > 30,
               "cyan=%d px" % cyan)

        # ---------- 3. 再移动一段，验证「继续跟随」而非「只动一次」 ----------
        print("\n[3] 继续移动，光标持续跟随")
        for _ in range(6):
            c.rel_move(-20, -15)
            time.sleep(0.12)
        time.sleep(0.5)
        s2 = c.shot("03_rel_moved2")
        report("第二次移动画面再次变化", diff(s1, s2) > 200,
               "diff=%d px" % diff(s1, s2))

        # ---------- 4. 键盘回显 ----------
        print("\n[4] 键盘打字回显到图形终端")
        # 终端窗口内容区 (40,220)~(560,370)，提示符在第 254 行附近
        for qc in ["h","e","l","l","o"]:
            c.tap(qc); time.sleep(0.08)
        time.sleep(0.4)
        s3 = c.shot("04_typed")
        # 回显区：第二行 y≈254，x 从 52 起
        echo_box = (50, 250, 300, 272)
        dark_before = sum(1 for (r,g,b) in s2.crop(echo_box).getdata() if r<100 and g<100 and b<100)
        dark_after  = sum(1 for (r,g,b) in s3.crop(echo_box).getdata() if r<100 and g<100 and b<100)
        report("终端区出现新的深色文字像素（hello 已回显）",
               dark_after > dark_before + 20,
               "before=%d after=%d" % (dark_before, dark_after))

        # ---------- 5. 自动重复（按住不放应产生多个字符） ----------
        print("\n[5] typematic 自动重复（按住 a 不放）")
        n_before = dark_after
        c.key("a", True)
        time.sleep(1.2)                  # 250ms delay + 40ms rate → 应重复多次
        c.key("a", False)
        time.sleep(0.4)
        s4 = c.shot("05_repeat")
        dark_rep = sum(1 for (r,g,b) in s4.crop((50,250,400,272)).getdata() if r<100 and g<100 and b<100)
        report("长按后文字像素继续增加（自动重复生效）", dark_rep > n_before + 20,
               "typed=%d long=%d" % (n_before, dark_rep))

        # ---------- 6. Ctrl+Shift+F1 退出桌面 ----------
        print("\n[6] Ctrl+Shift+F1 退出桌面")
        c.key("ctrl", True); c.key("shift", True)
        c.tap("f1")
        c.key("shift", False); c.key("ctrl", False)
        time.sleep(0.8)
        serial = ""
        try: serial = open(SER, "rb").read().decode("utf-8", "replace")
        except Exception: pass
        report("串口打印退出诊断（组合键状态机生效）",
               "桌面已退出" in serial or "已退出" in serial,
               repr(serial[-160:]))

        c.close()
    finally:
        q.terminate()
        try: q.wait(timeout=5)
        except Exception: q.kill()

    print("\n=== 结果：%s ===" % ("ALL PASS ✅" if ok else "有 FAIL ❌"))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
