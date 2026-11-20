#!/usr/bin/env python3
"""第四十九轮 · 桌面首批应用可用性与数据安全端到端验收。

背景：第四十八轮把 b64 通道做到能传 2MB 中文文件进来，但桌面文本编辑器
只有 1500B 缓冲、加载时不提示、点「保存」会 fs_setfile 把大文件截断重写。
本轮修成「超限 / 含非 ASCII 一律只读」，并补 UTF-8 码点绘制、文件管理器
新建/删除/大小显示、窗口缩放。本脚本给这些改动一条真实回归护栏。

判据刻意不靠 OCR / 猜像素：QMP 直接读 guest 内存里的内核静态符号
（aarch64-linux-gnu-nm 动态取地址），因此每一项都是确定性的。

覆盖：
  1. >ED_CAP 的大文件在编辑器里进入只读（ed_readonly=1 / ed_ro_reason=1 /
     ed_ro_size=真实字节数），点「只读」按钮被拒（ed_msg=2），文件内容大小不变
  2. 含中文的小文件进入只读（ed_readonly=1 / ed_ro_reason=2），保存同样被拒
  3. 只读模式下键盘输入不改动缓冲（ed_len 不变）
  4. 文件管理器「新建文件」→ fm_notice 报告成功；删除二次确认
     （第一次 armed、第二次真删），fm_sel 复位
  5. 拖右下角手柄缩放：G_win[].r.w/h 变大且受 clamp_win 约束
  6. 最后退出桌面用串口 `stat` 复核大文件仍为原始大小（最有力的不截断证明）

用法: python3 test_desktop_apps.py        # 默认当前 kernel.elf
      KERNEL=/tmp/pre/本体/kernel.elf python3 test_desktop_apps.py
"""
import base64
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
SER = "/tmp/loongos_apps_serial.sock"
QMP = "/tmp/loongos_apps_qmp.sock"
SHOTS = "/tmp/loongos_apps_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (SER, QMP):
    try:
        os.unlink(p)
    except OSError:
        pass

W, H = 800, 600
A_FILES, A_EDIT = 8, 9
ED_CAP = 4096
BIG = 4300                      # > ED_CAP-1 = 4095，触发「文件太大」只读
CN_PATH = "/cn.txt"
BIG_PATH = "/big.txt"

OK = [0]
FAIL = [0]


def wait_desktop(q, timeout=45):
    """不依赖启动日志：QEMU 的 -serial unix: 在客户端连上前会丢掉开机输出。
    直接读内核里的 g_desktop_active，==1 即证明桌面主循环已跑起来。"""
    addr = A.get("g_desktop_active")
    end = time.time() + timeout
    while time.time() < end:
        if addr is not None:
            v = q.words(addr, 1)
            if v and v[0] == 1:
                return True
        time.sleep(0.2)
    return False


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
        for _ in range(100):
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
            self.buf += self.s.recv(65536)
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

    def rstr(self, name, n=64):
        b = b""
        for v in self.words(A[name], (n + 3) // 4):
            b += v.to_bytes(4, "little")
        return b.split(b"\0")[0].decode("utf-8", "replace")

    def abs_move(self, x, y):
        self.q("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
        ]})

    def btn(self, down):
        self.q("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": "left"}}]})

    def click(self, x, y, hold=0.12):
        self.abs_move(x, y)
        time.sleep(0.08)
        self.btn(True)
        time.sleep(hold)
        self.btn(False)
        time.sleep(0.28)

    def drag(self, x0, y0, x1, y1):
        self.abs_move(x0, y0)
        time.sleep(0.15)
        self.btn(True)
        time.sleep(0.3)
        for i in range(1, 9):
            self.abs_move(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8)
            time.sleep(0.12)
        self.btn(False)
        time.sleep(0.4)

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


class Ser:
    """双向 Unix socket 串口：既能读日志，也能往客人 shell 敲命令。"""

    def __init__(self, path):
        self.s = None
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        if not self.s:
            raise RuntimeError("串口 socket 连不上")
        self.s.settimeout(0.02)
        self.buf = bytearray()

    def pump(self, sec):
        end = time.time() + sec
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if d:
                    self.buf.extend(d)
            except socket.timeout:
                pass
            except OSError:
                break

    def send(self, s, wait=0.4):
        self.s.sendall(s.encode())
        self.pump(wait)

    def since(self, mark):
        return bytes(self.buf)[mark:].decode("utf-8", "replace")

    def wait(self, needle, timeout=20, since=0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.2)
            if needle in bytes(self.buf)[since:]:
                return True
        return False

    def stat_size(self, path, timeout=6):
        mark = len(self.buf)
        self.send("stat %s\r" % path, 0.8)
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.2)
            t = self.since(mark)
            m = re.search(r"大小:\s*(\d+)\s*字节", t)
            if m:
                return int(m.group(1))
        return -1


def b64_paste(ser, path, data, step=0.2):
    b = base64.b64encode(data).decode()
    lines = [b[i:i + 76] for i in range(0, len(b), 76)]
    mark = len(ser.buf)
    ser.send("b64in %s\r" % path, 0.5)
    for ln in lines:
        ser.send(ln + "\r", step)
    ser.send(".\r", 1.2)
    t = ser.since(mark)
    m = re.search(r"已写入\s*(\d+)\s*字节", t)
    return int(m.group(1)) if m else -1, t


def find_win(q, app_id):
    for i in range(12):
        b = A["G_win"] + i * 40           # struct dwin = 40B
        if q.words(b + 24, 1) == [1]:     # visible
            aid = q.words(b + 20, 1)[0]
            if aid == app_id:
                r = q.words(b, 4)          # x, y, w, h
                return i, r
    return None, None


def close_edit(q):
    ei, er = find_win(q, A_EDIT)
    if ei is not None:
        q.click(er[0] + er[2] - 14, er[1] + 15)   # close_rect 中心
        return True
    return False


def open_file(q, crx, cry, target, maxrow=16):
    """逐行点击列表，用 ed_path 判断是否打开了目标文件。
    点中目录会进子目录 → 下一步先点 '..' 回根；点中文件会开编辑器 → 先关掉。"""
    for j in range(maxrow):
        close_edit(q)                              # 先关旧编辑器，ed_path 才不会残留误判
        if q.rstr("fm_path") != "/":
            q.click(crx + 100, cry + 62)           # '..' 第一行
        q.click(crx + 100, cry + 62 + 20 * j)
        ei, _ = find_win(q, A_EDIT)                # 点中文件才会新开编辑器窗口
        if ei is not None and q.rstr("ed_path") == target:
            return True, j
    return False, -1


def main():
    print("===== 第四十九轮 桌面首批应用 端到端验收 =====")
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
        ser = Ser(SER)
        q = Qmp(QMP)
        check("桌面就绪", wait_desktop(q, 45), "g_desktop_active=1")

        # ---------- Phase A：退回串口 shell 造两个测试文件 ----------
        mark = len(ser.buf)
        q.hmp("sendkey ctrl-shift-f1")
        check("Ctrl+Shift+F1 返回串口终端",
              ser.wait("已返回串口终端".encode(), 12, mark))

        big = (b"LoongOS-49-bigfile-ASCII-" * 200)[:BIG]
        n1, t1 = b64_paste(ser, BIG_PATH, big)
        check("b64in 造 >4KB 大文件", n1 == BIG, "写入 %d 字节（期望 %d）" % (n1, BIG))
        if n1 != BIG:
            print("    ---- b64in 输出片段 ----")
            print(t1[-400:])

        cn = ("灵珑系统第四十九轮中文显示测试 ABC 123\n" * 3).encode("utf-8")
        n2, t2 = b64_paste(ser, CN_PATH, cn)
        check("b64in 造中文小文件", n2 == len(cn),
              "写入 %d 字节（期望 %d）" % (n2, len(cn)))

        sz0 = ser.stat_size(BIG_PATH)
        check("串口 stat 记录大文件原始大小", sz0 == BIG, "%d 字节" % sz0)

        # ---------- Phase B：重进桌面，走真实点击 ----------
        mark = len(ser.buf)
        ser.send("desktop\r", 2.0)
        check("desktop 重进图形桌面",
              ser.wait("已装系统：开始菜单不显示安装程序".encode(), 12, mark))

        q.click(49, 579)                                   # 开始按钮
        menu_n = q.r32("G_menu_n")
        h = 8 + menu_n * 26 + 8
        y0 = H - 42 - h
        idx = None
        for i in range(menu_n):
            b = A["G_menu"] + i * 24                       # struct menuitem = 24B
            typ = q.words(b + 8, 1)[0]
            aid = q.words(b + 12, 1)[0]
            if typ == 0 and aid == A_FILES:
                idx = i
                break
        check("开始菜单含文件管理器", idx is not None,
              "菜单项数=%d, 命中 idx=%s" % (menu_n, idx))
        if idx is None:
            raise RuntimeError("找不到文件管理器菜单项")
        q.click(126, y0 + 19 + idx * 26)

        wi, r = find_win(q, A_FILES)
        check("文件管理器窗口已打开", wi is not None,
              "r=%s" % (r,))
        if wi is None:
            raise RuntimeError("文件管理器未打开")
        crx, cry = r[0] + 1, r[1] + 28                    # 内容区左上

        # 1) 大文件只读
        got, row = open_file(q, crx, cry, BIG_PATH)
        check("在文件管理器里点开大文件", got, "列表行 %d" % row)
        ro = q.r32("ed_readonly")
        reason = q.r32("ed_ro_reason")
        rosize = q.r32("ed_ro_size")
        check("大文件进入只读模式", ro == 1 and reason == 1,
              "readonly=%s reason=%s" % (ro, reason))
        check("只读原因记录真实文件大小", rosize == BIG,
              "ed_ro_size=%s（期望 %d）" % (rosize, BIG))
        q.shot("01_big_readonly")

        # 点「只读」按钮（内容区右上，ed_draw 里 cr.w-78, y=2, 70x22）
        ei, er = find_win(q, A_EDIT)
        if ei is not None:
            q.click(er[0] + 1 + (er[2] - 2) - 78 + 35, er[1] + 28 + 2 + 11)
            check("点只读按钮被拒绝保存", q.r32("ed_msg") == 2,
                  "ed_msg=%s（2=只读未保存）" % q.r32("ed_msg"))

        # 只读模式下敲键不改动缓冲
        before = q.r32("ed_len")
        q.hmp("sendkey a")
        q.hmp("sendkey ret")
        time.sleep(0.4)
        check("只读模式键盘不改缓冲", q.r32("ed_len") == before,
              "ed_len %s -> %s" % (before, q.r32("ed_len")))
        close_edit(q)

        # 2) 中文小文件只读
        got2, row2 = open_file(q, crx, cry, CN_PATH)
        check("在文件管理器里点开中文文件", got2, "列表行 %d" % row2)
        ro2 = q.r32("ed_readonly")
        reason2 = q.r32("ed_ro_reason")
        check("中文文件进入只读（不插坏 UTF-8）", ro2 == 1 and reason2 == 2,
              "readonly=%s reason=%s" % (ro2, reason2))
        q.shot("02_cn_readonly")
        close_edit(q)

        # 3) 文件管理器新建文件
        q.click(crx + 44, cry + 37)                        # 新建文件按钮中心
        notice = q.rstr("fm_notice")
        check("新建文件按钮生效", "已建文件" in notice, "fm_notice=%r" % notice)

        # 4) 删除二次确认：先点中一个文件（会开编辑器），关掉后用其选中态
        open_file(q, crx, cry, CN_PATH)
        close_edit(q)
        sel = q.r32("fm_sel")
        check("点选文件后保留选中态", sel is not None and sel >= 0, "fm_sel=%s" % sel)
        q.click(crx + 44 + 2 * 78, cry + 37)              # 删除按钮中心
        armed1 = q.r32("fm_del_armed")
        check("删除第一次仅武装（二次确认）", armed1 == 1,
              "fm_del_armed=%s" % armed1)
        q.click(crx + 44 + 2 * 78, cry + 37)
        armed2 = q.r32("fm_del_armed")
        notice2 = q.rstr("fm_notice")
        check("删除第二次真删且解除武装", armed2 == 0 and "已删" in notice2,
              "armed=%s notice=%r" % (armed2, notice2))

        # 5) 缩放手柄
        wi3, r3 = find_win(q, A_FILES)
        if wi3 is not None:
            q.drag(r3[0] + r3[2] - 7, r3[1] + r3[3] - 7,
                   r3[0] + r3[2] - 7 + 70, r3[1] + r3[3] - 7 + 50)
            wi4, r4 = find_win(q, A_FILES)
            grew = (r4 is not None and r4[2] > r3[2] + 20 and r4[3] > r3[3] + 10)
            bounded = (r4 is not None and r4[2] <= W - 16 and r4[3] <= H - 38 - 42 - 4)
            check("拖右下角手柄放大窗口", grew, "%s -> %s" % (r3, r4))
            check("缩放受 clamp_win 约束", bounded, "r=%s" % (r4,))
            q.shot("03_resized")

        # ---------- Phase C：退回串口，复核大文件未被截断 ----------
        mark = len(ser.buf)
        q.hmp("sendkey ctrl-shift-f1")
        exited = ser.wait("已返回串口终端".encode(), 12, mark)
        sz1 = ser.stat_size(BIG_PATH) if exited else -1
        check("大文件点过保存后大小不变（核心：不截断）", sz1 == BIG,
              "stat %d 字节（原始 %d）" % (sz1, BIG))

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("===== 结果: %d 通过 / %d 失败 =====" % (OK[0], FAIL[0]))
    return 1 if FAIL[0] else 0


if __name__ == "__main__":
    sys.exit(main())
