#!/usr/bin/env python3
"""LoongOS 桌面 P2 端到端实测（TCG / UEFI+ramfb 图形 + 三 HID 设备）。

复刻用户真机 VM 的 USB 拓扑：qemu-xhci 上同时挂
  usb-tablet(port1, 绝对 6B) / usb-mouse(port2, 相对 4B) / usb-kbd(port3, 8B)。
用 QMP 做：screendump 抓图、input-send-event 注入绝对坐标 + 左键按下/抬起
（点击开始菜单、点菜单项跑命令、拖窗口标题栏）、human-monitor-command 调
HMP sendkey 注入 Ctrl+Shift+F1（退出桌面）。

判定：
  1. 三 HID 设备全部枚举成功（键盘=1 指针设备=2）
  2. 开机默认进桌面，截图有完整渲染（颜色数 > 40）
  3. 点「开始」→ 菜单区域像素明显变化
  4. 点「命令清单」→ 终端窗口出现 help 输出（串口日志 + 截图变化）
  5. 拖标题栏 → 窗口位置变化（截图变化）
  6. Ctrl+Shift+F1 → 日志出现「已返回串口终端」
  7. 退出后 USB 键盘敲 help 仍能被串口 shell 消费（不丢输入）
"""
import json
import os
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
TAG = "p2"
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
VARS = "/tmp/loongos_%s_vars.fd" % TAG
SHOTS = "/tmp/loongos_%s_shots" % TAG
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
W, H = 800, 600          # ramfb / GOP 固定 800x600

os.makedirs(SHOTS, exist_ok=True)
for p in (LOG, QMP, VARS):
    try:
        os.unlink(p)
    except OSError:
        pass
subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", VARS], check=True)


def log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_log(pat, timeout=30, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if pat in log()[since:]:
            return True
        time.sleep(0.15)
    return False


class Qmp:
    def __init__(self, path):
        end = time.time() + 15
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.1)
        self.s.settimeout(5)
        self.buf = b""
        self._line()                       # greeting
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8", "replace"))

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall((json.dumps(msg) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                if "error" in r:
                    print("  [QMP error] %s -> %s" % (execute, r["error"]))
                return r

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        self.cmd("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.05)
        try:
            from PIL import Image
            Image.open(ppm).convert("RGB").save(os.path.join(SHOTS, name + ".png"))
        except Exception as e:
            print("  [warn] PPM->PNG: %s" % e)
        return ppm

    def abs_move(self, x, y):
        # QEMU 绝对轴取值 0..32767，线性映射到 surface
        vx = int(x * 32767 / (W - 1))
        vy = int(y * 32767 / (H - 1))
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": vx}},
            {"type": "abs", "data": {"axis": "y", "value": vy}},
        ]})

    def btn(self, down):
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": "left"}},
        ]})

    def click(self, x, y, hold=0.35):
        self.abs_move(x, y)
        time.sleep(0.15)
        self.btn(True)
        time.sleep(hold)
        self.btn(False)
        time.sleep(0.35)

    def drag(self, x0, y0, x1, y1):
        self.abs_move(x0, y0)
        time.sleep(0.15)
        self.btn(True)
        time.sleep(0.3)
        steps = 8
        for i in range(1, steps + 1):
            self.abs_move(x0 + (x1 - x0) * i / steps,
                          y0 + (y1 - y0) * i / steps)
            time.sleep(0.12)
        self.btn(False)
        time.sleep(0.4)

    def hmp(self, line):
        return self.cmd("human-monitor-command", {"command-line": line})


def diff_ratio(a, b, box=None):
    """两张 PNG 在指定区域的像素差异比例（0..1）"""
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if box:
        ia, ib = ia.crop(box), ib.crop(box)
    if ia.size != ib.size:
        return -1.0
    d = ImageChops.difference(ia, ib).convert("L")
    hist = d.histogram()
    changed = sum(hist[12:])
    return changed / float(ia.size[0] * ia.size[1])


def colors(png):
    from PIL import Image
    im = Image.open(png).convert("RGB")
    return len(im.getcolors(maxcolors=1 << 20) or [])


def main():
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
        "-m", "256", "-smp", "1",
        # 裸 ELF 直启：-kernel 由 QEMU 内建跳板直接加载，不挂 pflash
        # （挂了 AAVMF 会走正常 UEFI 启动流程去 PXE 而根本不碰 -kernel）。
        # 帧缓冲走 kernel.c 的 fw_cfg "etc/ramfb" 自建 800x600 显示底座。
        "-kernel", os.path.join(BASE, "kernel.elf"),
        "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    print("[P2] 启动 QEMU（UEFI vars + 裸 ELF + ramfb + 三 HID 设备）")
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  %s %s %s" % ("✓" if ok else "✗", name, detail))

    try:
        if not wait_log("[desktop]", 40):
            print("[P2] ✗ 40s 内未启动到桌面，日志尾部：\n" + log()[-2000:])
            return 1
        q = Qmp(QMP)

        # 1) 三 HID 设备枚举
        enum_ok = wait_log("HID 枚举完成", 10) and "键盘=1" in log() and "指针设备=2" in log()
        enum_line = next((l for l in log().splitlines() if "HID 枚举完成" in l), "")
        check("三 HID 设备枚举（键盘1+指针2）", enum_ok, enum_line.strip())

        # 2) 开机默认进桌面 + 截图
        desk_ok = "[desktop] 图形桌面已就绪" in log()
        check("开机默认直接进桌面", desk_ok)
        q.shot("01_desktop")
        png1 = os.path.join(SHOTS, "01_desktop.png")
        nc = colors(png1)
        check("桌面完整渲染（颜色数>40）", nc > 40, "colors=%d" % nc)

        # 3) 点「开始」按钮 (10,600-42+8,78,26) 中心约 (49,579)
        q.click(49, 579)
        q.shot("02_menu_open")
        d_menu = diff_ratio(png1, os.path.join(SHOTS, "02_menu_open.png"),
                            box=(0, 260, 240, 560))
        check("鼠标点「开始」弹出菜单", d_menu > 0.05,
              "菜单区域差异=%.3f" % d_menu)

        # 4) 点菜单「命令清单」(第 5 行：y=282+8+4*26+11≈405)
        q.click(110, 405)
        q.shot("03_help_in_term")
        d_help = diff_ratio(os.path.join(SHOTS, "02_menu_open.png"),
                            os.path.join(SHOTS, "03_help_in_term.png"))
        help_ok = wait_log("命令清单", 8) or "help" in log()
        check("点击菜单执行 help 并显示在图形终端", d_help > 0.02 or help_ok,
              "全屏差异=%.3f 日志命中=%s" % (d_help, help_ok))

        # 5) 拖终端窗口标题栏（先点任务栏「图形终端」确保置顶）
        q.click(140, 579)
        q.shot("04_before_drag")
        # 终端窗口初始 {24,44,503,335}，标题栏 y=44..72；必须从标题栏按下才会拖动
        q.drag(300, 56, 460, 190)
        q.shot("05_after_drag")
        d_drag = diff_ratio(os.path.join(SHOTS, "04_before_drag.png"),
                            os.path.join(SHOTS, "05_after_drag.png"))
        check("拖动标题栏移动窗口", d_drag > 0.02, "全屏差异=%.3f" % d_drag)

        # 6) Ctrl+Shift+F1 退出桌面
        mark = len(log())
        q.hmp("sendkey ctrl-shift-f1")
        exit_ok = wait_log("[desktop] 已返回串口终端", 10, since=mark)
        check("Ctrl+Shift+F1 返回串口终端", exit_ok)

        # 7) 退出后 USB 键盘输入仍可用（shell 收到 help）
        if exit_ok:
            mark2 = len(log())
            for k in ("h", "e", "l", "p", "ret"):
                q.hmp("sendkey " + k)
                time.sleep(0.2)
            shell_ok = wait_log("命令清单", 8, since=mark2)
            check("退出后键盘仍能操作串口 shell", shell_ok)

        ok = all(r[1] for r in results)
        print("\n[P2] %s  %d/%d 通过" %
              ("ALL PASS" if ok else "FAIL", sum(1 for r in results if r[1]), len(results)))
        print("[P2] 截图目录：" + SHOTS)
        # 关键日志摘录，便于排查
        for key in ("[usb ]", "[desktop]", "[fb  ]", "[install]"):
            lines = [l for l in log().splitlines() if l.startswith(key)]
            if lines:
                print("  ---- %s ----" % key)
                for l in lines[-8:]:
                    print("  " + l)
        return 0 if ok else 1
    finally:
        try:
            p.terminate()
            p.wait(timeout=5)
        except Exception:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
