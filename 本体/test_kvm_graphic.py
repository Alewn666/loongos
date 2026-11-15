#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十二轮 · 真机 / KVM 图形桌面复验判据脚本。

背景
----
第五十一轮用 `test_kvm_dual.py` 在飞牛 NAS aarch64 真机上跑通了
`-enable-kvm -cpu host` 的 **TCG 14/14 + KVM 14/14**，但那是 `-nographic`
纯串口双路、没有帧缓冲。README「诚实的边界」与两份 TODO 都明确留了
「路线 B：图形桌面 / 键鼠 / 装盘交互人工验收」未做，且不冒充已完成。

本脚本把「只在 TCG 实测过」的图形链路搬到真机 KVM：
  - 复用 `test_desktop_p2.py` 的 QMP 注入（screendump + input-send-event
    + HMP sendkey），用**真实像素**判定而不是串口假绿（第三十二轮教训）；
  - 复用 `test_kvm_dual.py` 的串口 Session（后台 pump 线程 + 5ms 轮询，
    避免阻塞 `read(1)` 被 [task1] 心跳拖死）；
  - 零第三方依赖：真机没有 PIL，PPM(P6) 解析与像素差异全部纯 Python 实现；
  - TCG / KVM 双路同脚本同判据，产出结构化对照报告。

启动方式
--------
  GFX_BOOT=iso   AAVMF pflash + `-cdrom` ISO（贴近用户真实装机路径）
  GFX_BOOT=elf   `-kernel kernel.elf` 裸 ELF 直启（与本地 TCG 图形基线同构）
  默认 iso；两种方式的 ISO 内 kernel.elf 与 `本体/kernel.elf` 三方 md5 一致。

判据（15 项 × 双路）
--------------------
  1  内核横幅 / 引导成功              10 退出后键盘仍被串口 shell 消费
  2  开机默认进图形桌面              11 install 命令可用且不破坏磁盘
  3  三 HID 枚举（键盘1+指针2）      12 net 命令可用
  4  ramfb 真实画面 800x600          13 bench 在图形态跑完（gfx 项有值）
  5  桌面非黑屏（颜色数>40）         14 res 健康计数全 0
  6  点「开始」弹出菜单              15 SHA2 字段与硬件哈希自洽
  7  点菜单项打开应用窗口
  8  拖标题栏移动窗口
  9  Ctrl+Shift+F1 返回串口终端

用法
----
  python3 test_kvm_graphic.py                    # 自动：有 /dev/kvm 跑 TCG+KVM，否则只 TCG
  python3 test_kvm_graphic.py --probe           # 只做 1~5 冒烟（快速探启动方式可行性）
  GFX_BOOT=iso GFX_ISO=/abs/loongos.iso GFX_PATH=/abs/本体 python3 test_kvm_graphic.py
  KVM_FORCE=1 python3 test_kvm_graphic.py      # 无 /dev/kvm 也强制尝试 KVM（明确报错）

产出
----
  /tmp/loongos_kvm_gfx_<TCG|KVM>.log            每路完整串口日志
  /tmp/loongos_kvm_gfx_<TCG|KVM>_shots/*.ppm    每步真实画面截图
  /tmp/loong_kvm_graphic_report.json            结构化对照数据

诚实边界：本脚本起的是**独立临时 QEMU 进程**，不启停、不改动用户 libvirt VM；
图形交互由 QMP 注入模拟 HID，不等于人工在 VNC 里拖拽的体感验收。
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("GFX_PATH", HERE)
ISO = os.environ.get("GFX_ISO") or os.path.join(ROOT, "iso", "loongos.iso")
KERNEL = os.environ.get("GFX_KERNEL") or os.path.join(ROOT, "kernel.elf")
CODE = os.environ.get("GFX_CODE", "/usr/share/AAVMF/AAVMF_CODE.fd")
VARS_SRC = os.environ.get("GFX_VARS", "/usr/share/AAVMF/AAVMF_VARS.fd")
VARS_TPL = "/tmp/loongos_kvm_gfx_vars_%s.fd"
BOOT = os.environ.get("GFX_BOOT", "iso").lower()
PROBE = "--probe" in sys.argv
W, H = 800, 600                       # ramfb / GOP 固定 800x600

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b\[=[0-9][a-z]")
PROMPT = re.compile(r"(?:\r?\n|\r)> ")


# ---------------------------------------------------------------- PPM(P6)
def ppm_read(path):
    """纯 Python 解析 QEMU screendump 的 P6 PPM，返回 (w, h, rgb_bytes)。

    真机 fnOS 没有 PIL，不能依赖任何第三方库；P6 头部只含 magic/宽/高/maxval
    与可选 '#' 注释，后面紧跟 w*h*3 字节像素。
    """
    with open(path, "rb") as f:
        data = f.read()
    pos, tok = 0, []
    while len(tok) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        st = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        tok.append(data[st:pos])
    pos += 1                            # 跳过 maxval 后的单个空白
    magic, w, h, mx = tok[0], int(tok[1]), int(tok[2]), int(tok[3])
    if magic != b"P6" or mx != 255:
        raise ValueError("不是 255 色 P6 PPM: %r" % (tok,))
    return w, h, data[pos:pos + w * h * 3]


def ppm_colors(path, limit=100000):
    w, h, p = ppm_read(path)
    s = set()
    for i in range(0, len(p), 3):
        s.add(p[i:i + 3])
        if len(s) > limit:
            break
    return w, h, len(s)


def ppm_diff(a, b, box=None, thresh=12):
    """两图在 box 区域的最大通道差 > thresh 的像素比例（0..1）。"""
    wa, ha, pa = ppm_read(a)
    wb, hb, pb = ppm_read(b)
    if (wa, ha) != (wb, hb):
        return -1.0
    x0, y0, x1, y1 = box or (0, 0, wa, ha)
    changed = total = 0
    for y in range(y0, y1):
        ia = (y * wa + x0) * 3
        for x in range(x0, x1):
            i = ia + (x - x0) * 3
            if (abs(pa[i] - pb[i]) > thresh or
                    abs(pa[i + 1] - pb[i + 1]) > thresh or
                    abs(pa[i + 2] - pb[i + 2]) > thresh):
                changed += 1
            total += 1
    return changed / float(total) if total else 0.0


# ---------------------------------------------------------------- QMP
class Qmp:
    def __init__(self, path, timeout=20):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.1)
        self.s.settimeout(10)
        self.buf = b""
        self._line()                     # greeting
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("QMP 连接关闭")
            self.buf += chunk
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

    def shot(self, path):
        self.cmd("screendump", {"filename": path})
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) > 1000:
                return path
            time.sleep(0.05)
        return path

    def abs_move(self, x, y):
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

    def drag(self, x0, y0, x1, y1, steps=8):
        self.abs_move(x0, y0)
        time.sleep(0.15)
        self.btn(True)
        time.sleep(0.30)
        for i in range(1, steps + 1):
            self.abs_move(x0 + (x1 - x0) * i / steps,
                          y0 + (y1 - y0) * i / steps)
            time.sleep(0.12)
        self.btn(False)
        time.sleep(0.40)

    def hmp(self, line):
        return self.cmd("human-monitor-command", {"command-line": line})


# ---------------------------------------------------------------- 串口 Session
class Session:
    """一路 QEMU：-nographic 串口走 stdio（双向），QMP 走独立 unix socket。

    第五十一轮 IO 重构：后台 daemon 线程持续 read(4096) 抽干串口，
    主线程只轮询缓冲 + 5ms sleep，彻底避开阻塞 read 被心跳拖死。
    """
    def __init__(self, mode):
        self.mode = mode
        self.kvm = (mode == "KVM")
        gic = "3" if self.kvm else "2"
        self.qmp_path = "/tmp/loongos_kvm_gfx_%s.sock" % mode.lower()
        self.shots = "/tmp/loongos_kvm_gfx_%s_shots" % mode.lower()
        self.log_path = "/tmp/loongos_kvm_gfx_%s.log" % mode.lower()
        os.makedirs(self.shots, exist_ok=True)
        for p in (self.qmp_path,):
            try:
                os.unlink(p)
            except OSError:
                pass

        cmd = ["qemu-system-aarch64",
               "-machine", "virt,gic-version=" + gic,
               "-cpu", "host" if self.kvm else "cortex-a57"]
        if self.kvm:
            cmd += ["-enable-kvm"]
        cmd += ["-m", "256M", "-smp", "1"]
        if BOOT == "iso":
            vars_fd = VARS_TPL % mode.lower()
            shutil.copyfile(VARS_SRC, vars_fd)
            cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
                    "-drive", "if=pflash,format=raw,file=" + vars_fd,
                    "-cdrom", ISO]
        else:
            cmd += ["-kernel", KERNEL]
        cmd += ["-nographic", "-no-reboot",
                "-qmp", "unix:%s,server=on,wait=off" % self.qmp_path,
                "-device", "ramfb",
                "-device", "qemu-xhci,id=xhci",
                "-device", "usb-tablet,bus=xhci.0,port=1",
                "-device", "usb-mouse,bus=xhci.0,port=2",
                "-device", "usb-kbd,bus=xhci.0,port=3"]
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
        self.buf = bytearray()
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        fd = self.proc.stdout
        while True:
            try:
                chunk = fd.read(4096)
            except Exception:
                break
            if not chunk:
                break
            with self._lock:
                self.buf.extend(chunk)

    def seg(self, mark=0):
        with self._lock:
            raw = bytes(self.buf[mark:])
        return ANSI.sub(b"", raw).decode("utf-8", "replace")

    def text(self):
        return self.seg(0)

    def wait(self, pattern, timeout=45):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.text()):
                return True
            time.sleep(0.005)
        return False

    def wait_seg(self, mark, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.seg(mark)):
                return True
            time.sleep(0.005)
        return False

    def mark(self):
        with self._lock:
            return len(self.buf)

    def raw_send(self, line):
        self.proc.stdin.write((line + "\r").encode())
        self.proc.stdin.flush()

    def send(self, line, settle=1.2, wait_prompt=True):
        m = self.mark()
        self.raw_send(line)
        if wait_prompt:
            end = time.time() + settle
            while time.time() < end:
                if PROMPT.search(self.seg(m)):
                    break
                time.sleep(0.005)
        else:
            time.sleep(settle)
        return self.seg(m)

    def collect(self, line, end_rx, timeout=30):
        m = self.mark()
        self.raw_send(line)
        rx = re.compile(end_rx)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.seg(m)):
                break
            time.sleep(0.005)
        return self.seg(m)

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            with open(self.log_path, "w", encoding="utf-8") as f:
                f.write(self.text())
        except Exception:
            pass


# ---------------------------------------------------------------- 解析
def parse_res(t):
    d = {}
    m = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+FIQ/SErr=(\d+)\s+熔断=(\d+)", t)
    if m:
        for k, v in zip(["sync_exc", "recovered", "nisv", "fatal", "trip"], m.groups()):
            d[k] = int(v)
    m = re.search(r"SHA256=(硬件|纯C)", t)
    if m:
        d["sha256_mode"] = m.group(1)
    return d


def parse_bench(t):
    d = {}
    m = re.search(r"SHA2 扩展=(\d+)\s+硬件哈希=(\d+)", t)
    if m:
        d["sha2_field"], d["hw_hash"] = int(m.group(1)), int(m.group(2))
    for key, label in [("kdf_us", "KDF 5万轮"), ("memset_us", "memset 1MB"),
                       ("memcpy_us", "memcpy 1MB"), ("gfx_fill_us", "gfx 全屏填充 x10"),
                       ("gfx_1k_us", "gfx 画 ~1000 字符")]:
        m = re.search(re.escape(label) + r": (\d+) us", t)
        if m:
            d[key] = int(m.group(1))
    return d


# ---------------------------------------------------------------- 单路
def run_mode(mode):
    print("\n" + "=" * 68)
    print("  %s 路  %s  |  GFX_BOOT=%s" %
          (mode, "(-enable-kvm -cpu host, GICv3, 真 CPU)" if mode == "KVM"
           else "(QEMU TCG, cortex-a57, GICv2)", BOOT))
    print("=" * 68)
    s = Session(mode)
    checks = []
    res = {"mode": mode, "boot": BOOT, "cmd": " ".join(s.cmd)}

    def ck(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(("  ✅ " if ok else "  ❌ ") + name + (("  | " + detail) if detail else ""))

    try:
        # 1) 引导到内核横幅
        boot = s.wait(r"灵珑系统.*LoongOS", timeout=60)
        ck("内核横幅 / %s 引导成功" % BOOT.upper(), boot)
        # 2) 开机默认进图形桌面（而非串口 shell）
        desk = s.wait(r"\[desktop\] 图形桌面已就绪", timeout=45)
        ck("开机默认进入图形桌面", desk)
        if not (boot and desk):
            res["fatal"] = "未进入图形桌面（看日志尾部）"
            return res, checks
        # 3) 三 HID 枚举
        enum = s.wait(r"HID 枚举完成", timeout=15)
        line = next((l for l in s.text().splitlines() if "HID 枚举完成" in l), "")
        ck("三 HID 设备枚举（键盘=1 指针设备=2）",
           enum and "键盘=1" in line and "指针设备=2" in line, line.strip())

        q = Qmp(s.qmp_path)
        # 4/5) 真实画面判定：尺寸 800x600 + 非黑屏（防「串口假绿」）
        p1 = q.shot(os.path.join(s.shots, "01_desktop.ppm"))
        try:
            w, h, nc = ppm_colors(p1)
        except Exception as e:
            w = h = nc = 0
            ck("ramfb 真实画面可解析", False, str(e))
        ck("ramfb display surface = 800x600", (w, h) == (W, H), "实测 %dx%d" % (w, h))
        ck("桌面完整渲染（颜色数>40，非黑屏）", nc > 40, "colors=%d" % nc)
        res["colors"] = nc
        res["surface"] = "%dx%d" % (w, h)
        if PROBE:
            return res, checks

        # 6) 点「开始」→ 菜单区域像素变化
        q.click(49, 579)
        p2 = q.shot(os.path.join(s.shots, "02_menu_open.ppm"))
        d_menu = ppm_diff(p1, p2, box=(0, 250, 245, 560))
        ck("鼠标点「开始」弹出菜单", d_menu > 0.05, "菜单区差异=%.3f" % d_menu)
        res["d_menu"] = round(d_menu, 4)

        # 7) 点菜单项（第 5 行「命令清单」附近）→ 应用窗口/画面变化
        q.click(110, 379)
        p3 = q.shot(os.path.join(s.shots, "03_app_open.ppm"))
        d_app = ppm_diff(p2, p3)
        ck("点击菜单项打开应用窗口", d_app > 0.02, "全屏差异=%.3f" % d_app)
        res["d_app"] = round(d_app, 4)

        # 8) 拖标题栏移动窗口
        q.click(140, 579)
        p4 = q.shot(os.path.join(s.shots, "04_before_drag.ppm"))
        q.drag(300, 56, 460, 190)
        p5 = q.shot(os.path.join(s.shots, "05_after_drag.ppm"))
        d_drag = ppm_diff(p4, p5)
        ck("拖动标题栏移动窗口", d_drag > 0.02, "全屏差异=%.3f" % d_drag)
        res["d_drag"] = round(d_drag, 4)

        # 9) Ctrl+Shift+F1 返回串口终端
        m = s.mark()
        q.hmp("sendkey ctrl-shift-f1")
        exited = s.wait_seg(m, r"\[desktop\] 已返回串口终端", timeout=15)
        ck("Ctrl+Shift+F1 返回串口终端", exited)

        # 10) 退出后键盘仍能被串口 shell 消费（sendkey 逐字敲 help）
        if exited:
            m2 = s.mark()
            for k in ("h", "e", "l", "p", "ret"):
                q.hmp("sendkey " + k)
                time.sleep(0.20)
            shell_ok = s.wait_seg(m2, r"命令清单|b64out", timeout=12)
            ck("退出后键盘仍可操作串口 shell", shell_ok)
        else:
            ck("退出后键盘仍可操作串口 shell", False, "前置退出未成功")

        # 11) install 命令可用且非破坏性：只敲一次、绝不发 yes
        #     结束标记驱动（第五十一轮教训）：install 会先打印 scsi/nvme 探测
        #     清单再给确认提示，固定窗口 send() 会被异步输出竞态截断。
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("install",
                        r"确认请敲|install 0 yes|\[install\] (?:FAIL|START|DONE)",
                        timeout=20)
        # 未二次确认必须停在确认/失败，绝不能进入 START/DONE 写盘流程
        install_safe = (("确认请敲" in out or "install 0 yes" in out
                         or "[install] FAIL" in out)
                        and "[install] START" not in out
                        and "[install] DONE" not in out)
        ck("install 命令可用且不破坏磁盘（未发 yes）", install_safe,
           out.strip().splitlines()[-1][:70] if out.strip() else "无输出")
        res["install_tail"] = out.strip()[-200:]

        # 12) net 命令可用
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("net", r"\[net \]|联网成功|IP\s", timeout=15)
        net_ok = any(k in out for k in ("[net ]", "链路", "MAC", "联网成功"))
        ck("net 命令可用", net_ok,
           out.strip().splitlines()[-1][:70] if out.strip() else "无输出")

        # 13) bench 在图形态跑完：gfx 渲染项有数值（第 51 轮 -nographic 表格里是「—」）
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("bench", r"提示:\s*us", timeout=30)
        b = parse_bench(out)
        res["bench"] = b
        gfx_ok = b.get("gfx_fill_us") is not None and b.get("gfx_1k_us") is not None
        ck("bench 图形态跑完，gfx 渲染项有实测值", gfx_ok and "KDF 5万轮" in out,
           "gfx_fill=%s us gfx_1k=%s us KDF=%s us" %
           (b.get("gfx_fill_us"), b.get("gfx_1k_us"), b.get("kdf_us")))

        # 14) res 健康计数全 0
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("res", r"SHA256=(?:硬件|纯C)", timeout=25)
        r = parse_res(out)
        res["res"] = r
        healthy = all(r.get(k, -1) == 0 for k in
                      ["sync_exc", "recovered", "nisv", "fatal", "trip"])
        ck("全程健康计数全 0（同步/自愈/NISV/致命/熔断）", healthy,
           str(r))
        res["sha256_mode"] = r.get("sha256_mode")

        # 15) SHA2 字段与硬件哈希自洽
        consistent = (b.get("hw_hash") == (1 if b.get("sha2_field", 0) >= 1 else 0))
        ck("SHA2 字段与硬件哈希自洽", consistent,
           "SHA2=%s 硬件哈希=%s res=%s" %
           (b.get("sha2_field"), b.get("hw_hash"), r.get("sha256_mode")))
        return res, checks
    except Exception as e:
        ck("执行未抛异常", False, "%s: %s" % (type(e).__name__, e))
        res["fatal"] = "%s: %s" % (type(e).__name__, e)
        return res, checks
    finally:
        s.close()
        print("  （串口日志：%s  截图：%s）" % (s.log_path, s.shots))


# ---------------------------------------------------------------- 主流程
def have_kvm():
    if os.uname().machine != "aarch64":
        return False, "宿主不是 aarch64"
    if not os.path.exists("/dev/kvm"):
        return False, "无 /dev/kvm"
    if not (os.access("/dev/kvm", os.R_OK) and os.access("/dev/kvm", os.W_OK)):
        return False, "/dev/kvm 无读写权限（需 root / kvm 组）"
    return True, "aarch64 + /dev/kvm 可读可写"


def main():
    need = [CODE, VARS_SRC] if BOOT == "iso" else []
    need += [ISO] if BOOT == "iso" else [KERNEL]
    for p in need:
        if not os.path.exists(p):
            print("缺文件：%s" % p)
            return 2
    if shutil.which("qemu-system-aarch64") is None:
        print("缺 qemu-system-aarch64")
        return 2

    ok_kvm, why = have_kvm()
    force = os.environ.get("KVM_FORCE") == "1"
    modes = ["TCG"]
    if ok_kvm or force:
        modes.append("KVM")
        if not ok_kvm:
            print("[gfx] 警告：%s；KVM_FORCE=1 仍尝试" % why)
    else:
        print("[gfx] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)

    report = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
              "boot": BOOT, "probe": PROBE,
              "kvm_capability": {"ok": ok_kvm, "why": why},
              "modes": {}, "summary": {}}
    all_checks = {}
    for mode in modes:
        r, checks = run_mode(mode)
        report["modes"][mode] = r
        all_checks[mode] = checks

    print("\n" + "=" * 68)
    print("  TCG vs KVM 图形复验对照表（GFX_BOOT=%s）" % BOOT)
    print("=" * 68)
    hdr = "%-30s%18s%18s" % ("指标", "TCG", "KVM")
    print(hdr)
    print("-" * 66)

    def g(mode, *keys):
        d = report["modes"].get(mode, {})
        for k in keys:
            if isinstance(d, dict) and k in d:
                d = d[k]
            else:
                return "—"
        return str(d)

    rows = [
        ("ramfb surface", ("surface",)),
        ("桌面颜色数", ("colors",)),
        ("开始菜单像素差异", ("d_menu",)),
        ("应用窗口像素差异", ("d_app",)),
        ("拖标题栏像素差异", ("d_drag",)),
        ("gfx 全屏 x10 (us)", ("bench", "gfx_fill_us")),
        ("gfx ~1000 字符 (us)", ("bench", "gfx_1k_us")),
        ("KDF 5万轮 (us)", ("bench", "kdf_us")),
        ("SHA2 扩展字段", ("bench", "sha2_field")),
        ("硬件哈希 (1=硬件)", ("bench", "hw_hash")),
        ("res SHA256 模式", ("sha256_mode",)),
        ("NISV / 同步异常", ("res", "nisv")),
        ("gfx 渲染项有值", None),
    ]
    for label, keys in rows:
        if keys is None:
            continue
        print("%-30s%18s%18s" % (label, g("TCG", *keys), g("KVM", *keys)))
    print("-" * 66)
    print("注① KVM 列「—」= 该路未执行，不是 0，也不是通过。")
    print("注② 本脚本有帧缓冲，bench 的 gfx 分支会真正执行；第五十一轮 -nographic")
    print("     双路报表中该项为「—」，本轮首次给出真机图形渲染耗时。")

    for mode, checks in all_checks.items():
        npass = sum(1 for _, ok, _ in checks if ok)
        report["summary"][mode] = {"pass": npass, "fail": len(checks) - npass,
                                   "failed": [n for n, ok, _ in checks if not ok]}
        print("  %s: %d 通过 / %d 失败%s" %
              (mode, npass, len(checks) - npass,
               "  失败项=%s" % report["summary"][mode]["failed"]
               if report["summary"][mode]["failed"] else ""))
    with open("/tmp/loong_kvm_graphic_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n结构化报告：/tmp/loong_kvm_graphic_report.json")
    if "KVM" not in all_checks:
        print("[gfx] 提醒：KVM 路未执行，本次不构成「TCG+KVM 图形双路复验完成」。")
    return 1 if any(v["fail"] for v in report["summary"].values()) else 0


if __name__ == "__main__":
    sys.exit(main())
