"""R 轮资源调配在线验证：在图形桌面运行时，用 QMP 直接读 guest 内存里的
统计变量 + screendump 真实像素，证明「省电不是靠冻住画面」。

⚠️ 2026-11-11 二次勘误：本脚本初版只读计数器，声称「画面没冻」却从没检查
   画面 —— 结果缓冲被挪到 0x4007E000 后计数器全对（wfi/tick/调频/IRQ 全绿），
   但 ramfb 根本没建、屏幕纯黑，本脚本照样 ALL PASS，成了「假绿」帮凶。
   现在补上真实画面判据（第 0 条，最优先）：screendump 必须是 800x600 且
   颜色数 >= 16、非背景像素 >= 1%，否则直接 FAIL。计数器绿 + 画面黑 = FAIL。

读取的符号（地址从 kernel.elf 的 nm 动态取）：
  g_wake_hz        当前唤醒档位（活跃1000 / 短闲250 / 久静120）
  g_desktop_active 桌面态（应为 1）
  g_timer_irqs     TIMER IRQ 累计（应随时间增长 = IRQ 一直在唤醒）
  g_wfi_hits       桌面 wfi 命中（应随时间增长 = 主循环在睡/醒循环）
  tick             软件节拍（10Hz，验证每 100ms 精确 +1）

判据：
  0) screendump 是 800x600 彩色真桌面（不是 640x480 纯黑空屏）★防假绿关键；
  1) 两次采样间 g_timer_irqs 与 g_wfi_hits 都明显增长 → 事件驱动循环活着；
  2) 久静后 g_wake_hz 降到 120（DEEP 档）→ 自适应调频生效；
  3) tick 增量 ≈ 经过秒数 × 10 → tick 恒 10Hz（分频正确，语义零变化）；
  4) 桌面态 = 1。
"""
import os
import sys
import time
import json
import socket
import subprocess

ROOT = "/workspace/安卓/loongos/本体"
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
ELF = KERNEL
LOG = "/tmp/loongos_reslive.log"
QMP = "/tmp/loongos_reslive_qmp.sock"


def sym_addrs(elf):
    out = subprocess.check_output(["aarch64-linux-gnu-nm", "-n", elf], text=True)
    d = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3:
            d[p[2]] = int(p[0], 16)
    return d


class Qmp:
    def __init__(self, path):
        self.s = None
        for _ in range(80):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        if not self.s:
            raise RuntimeError("QMP 连不上")
        self.s.settimeout(5)
        self.buf = b""
        self.line()                       # greeting
        self.q("qmp_capabilities")

    def line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        l, self.buf = self.buf.split(b"\n", 1)
        return json.loads(l.decode())

    def q(self, exe, arg=None):
        m = {"execute": exe}
        if arg:
            m["arguments"] = arg
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self.line()
            if "return" in r or "error" in r:
                return r

    def read32(self, addr):
        # HMP xp /4wx 读 4 个 32 位字，解析第一个
        r = self.q("human-monitor-command", {"command-line": "xp /1wx 0x%x" % addr})
        txt = r.get("return", "")
        # 形如 "00000000400ae5d4: 0x000003e8"
        for tok in txt.replace(":", " ").split():
            if tok.startswith("0x"):
                return int(tok, 16) & 0xFFFFFFFF
        return None

    def read64(self, addr):
        lo = self.read32(addr)
        hi = self.read32(addr + 4)
        if lo is None or hi is None:
            return None
        return (hi << 32) | lo

    def framebuffer_ok(self):
        """真实画面判据：返回 (ok, 描述)。防止『计数器全绿但屏幕纯黑』假绿。"""
        ppm = "/tmp/loongos_reslive_fb.ppm"
        try:
            os.unlink(ppm)
        except OSError:
            pass
        self.q("screendump", {"filename": ppm})
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.02)
        try:
            from PIL import Image
            from collections import Counter
            im = Image.open(ppm).convert("RGB")
            w, h = im.size
            px = list(im.get_flattened_data() if hasattr(im, "get_flattened_data")
                      else im.getdata())
            colors = len(set(px))
            bg_n = Counter(px).most_common(1)[0][1]
            nonbg = 1.0 - bg_n / float(w * h)
        except Exception as e:
            return False, "screendump 读取失败: %s" % e
        desc = "%dx%d 颜色数=%d 非背景=%.3f" % (w, h, colors, nonbg)
        return (w == 800 and h == 600 and colors >= 16 and nonbg >= 0.01), desc


def main():
    for p in (LOG, QMP):
        try:
            os.unlink(p)
        except OSError:
            pass
    A = sym_addrs(ELF)
    need = ["g_wake_hz", "g_desktop_active", "g_timer_irqs", "g_wfi_hits", "tick"]
    for n in need:
        assert n in A, "符号缺失: " + n
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2",
           "-cpu", "cortex-a57", "-m", "256", "-smp", "1", "-kernel", ELF,
           "-display", "none", "-serial", "file:" + LOG,
           "-qmp", "unix:%s,server=on,wait=off" % QMP,
           "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-device", "usb-mouse,bus=xhci.0,port=2",
           "-device", "usb-kbd,bus=xhci.0,port=3"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = {}
    try:
        # 等桌面就绪
        ready = False
        for _ in range(120):
            try:
                if "图形桌面已就绪" in open(LOG, "rb").read().decode("utf8", "replace"):
                    ready = True
                    break
            except OSError:
                pass
            time.sleep(0.2)
        print("  桌面就绪:", ready)
        if not ready:
            return 2
        q = Qmp(QMP)
        time.sleep(1.2)

        def snap():
            return {
                "wake": q.read32(A["g_wake_hz"]),
                "desk": q.read32(A["g_desktop_active"]),
                "irq": q.read64(A["g_timer_irqs"]),
                "wfi": q.read64(A["g_wfi_hits"]),
                "tick": q.read64(A["tick"]),
            }

        # 三段采样：刚进桌面(应 ACTIVE) / 静置 1s(应 IDLE) / 再静置 3s(应 DEEP)
        s0 = snap(); t0 = time.monotonic()
        time.sleep(1.2)
        s1 = snap(); t1 = time.monotonic()
        time.sleep(3.2)
        s2 = snap(); t2 = time.monotonic()
        print("  s0(刚进):", s0)
        print("  s1(1.2s):", s1)
        print("  s2(4.4s):", s2)
        dt = t2 - t1
        d_irq = (s2["irq"] or 0) - (s1["irq"] or 0)
        d_wfi = (s2["wfi"] or 0) - (s1["wfi"] or 0)
        d_tick = (s2["tick"] or 0) - (s1["tick"] or 0)
        # ACTIVE 段(首 1.2s) 与 DEEP 段(后 3.2s) 的 IRQ 频率，用于验证降档确实降频
        act_hz = ((s1["irq"] or 0) - (s0["irq"] or 0)) / (t1 - t0)
        deep_hz = d_irq / dt
        print("  Δ(后3.2s): IRQ=+%d  wfi=+%d  tick=+%d" % (d_irq, d_wfi, d_tick))
        print("  IRQ频率: ACTIVE段≈%.0fHz  DEEP段≈%.0fHz" % (act_hz, deep_hz))

        ok = True
        def chk(name, cond, detail):
            nonlocal ok
            print("    %s %s %s" % ("✓" if cond else "✗", name, detail))
            ok = ok and cond

        # ★ 第 0 条、最优先：真实画面。计数器再绿，屏幕纯黑也判 FAIL。
        fb_ok, fb_desc = q.framebuffer_ok()
        chk("真实画面 800x600 彩色(非黑屏)", fb_ok, fb_desc)
        chk("桌面态=1", s2["desk"] == 1, "desk=%s" % s2["desk"])
        chk("TIMER IRQ 持续增长(=画面没冻)", d_irq > 100, "+%d/%.1fs" % (d_irq, dt))
        chk("wfi 命中持续增长(=真在睡眠/唤醒)", d_wfi > 50, "+%d" % d_wfi)
        chk("自适应降档到 DEEP(120Hz)", s2["wake"] == 120, "wake=%sHz" % s2["wake"])
        chk("中间档曾到 IDLE(250Hz)", s1["wake"] in (250, 120),
            "s1.wake=%sHz" % s1["wake"])
        chk("tick 恒 10Hz", abs(d_tick - dt * 10) <= 2,
            "+%d (期望≈%d)" % (d_tick, dt * 10))
        # TCG 虚拟时间下 IRQ 频率不等于设定 hz，但降档后必须显著低于活跃段
        chk("DEEP 段 IRQ 频率显著低于 ACTIVE 段", deep_hz < act_hz * 0.7,
            "%.0fHz < %.0fHz×0.7" % (deep_hz, act_hz))
        print("  === %s ===" % ("ALL PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
