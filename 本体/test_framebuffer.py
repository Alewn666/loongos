#!/usr/bin/env python3
"""真实画面判据：验证 ramfb display surface 是否真的建立并绘制了桌面。

为什么需要这把尺子（2026-11-11 R 轮血泪）：
  此前所有图形实验都用「串口出现『图形桌面已就绪』/『fwcfg ramfb 已配置』」
  当成功判据，但这两句都是内核无条件打印的 —— QEMU 的 fw_cfg DMA 可能压根
  没建立 ramfb surface（缓冲落在 0x40080000 之前的低 RAM / loader-DTB 区时
  就是这样），此时 screendump 恒为 640x480 纯黑，而串口一切正常、CPU 在跑、
  wfi/tick/调频计数全对，全套「假绿」。真实回归里 P2/P2.1/P2.2/P3 全因
  「画面差异=0.000」而红，却没有一个测试直接说「屏幕是黑的」。

本脚本直接对 QEMU screendump 做像素级判定，是图形链路最底层、最不会骗人的
  判据：
    ① 尺寸必须是 FB_W x FB_H = 800x600（ramfb surface 建成才是这个尺寸；
       没建成时 QEMU 回退到默认 640x480）；
    ② 颜色数必须 > 阈值（真桌面是渐变壁纸+窗口+文字，至少几十种颜色；
       纯黑/纯色空屏只有 1~2 种）；
    ③ 非背景像素占比 > 阈值（确认真的画了东西，不是刚好一张纯色 large surface）。
  任一不满足即 FAIL，且明确打印「黑屏/未建 surface」，杜绝再次静默假绿。

用法：
  python3 test_framebuffer.py                      # 默认当前 kernel.elf
  KERNEL=/path/to/kernel.elf python3 test_framebuffer.py
返回 0=真出画面，1=黑屏/未建成，2=桌面未就绪/环境问题。
"""
import json
import os
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(BASE, "kernel.elf"))
TAG = os.environ.get("TAG", "fb")
W, H = 800, 600
LOG = "/tmp/loongos_%s_fb_serial.log" % TAG
QMP = "/tmp/loongos_%s_fb_qmp.sock" % TAG
VARS = "/tmp/loongos_%s_fb_vars.fd" % TAG
PPM = "/tmp/loongos_%s_fb.ppm" % TAG


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
        self.s.settimeout(6)
        self.buf = b""
        self._line()
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
                return r

    def shot(self):
        try:
            os.unlink(PPM)
        except OSError:
            pass
        self.cmd("screendump", {"filename": PPM})
        for _ in range(40):
            if os.path.exists(PPM) and os.path.getsize(PPM) > 1000:
                break
            time.sleep(0.02)
        return PPM


def wait_log(pat, timeout=35):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pat.encode() in open(LOG, "rb").read():
                return True
        except OSError:
            pass
        time.sleep(0.15)
    return False


def main():
    for p in (LOG, QMP, VARS, PPM):
        try:
            os.unlink(p)
        except OSError:
            pass
    if os.path.exists("/usr/share/AAVMF/AAVMF_VARS.fd"):
        subprocess.run(["cp", "/usr/share/AAVMF/AAVMF_VARS.fd", VARS], check=True)
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb", "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_log("图形桌面已就绪"):
            print("FAIL 桌面未就绪（串口尾部：%r）" % open(LOG, "rb").read()[-200:])
            return 2
        q = Qmp(QMP)
        time.sleep(0.8)                 # 等首帧全屏绘制完成
        ppm = q.shot()
        from PIL import Image
        im = Image.open(ppm).convert("RGB")
        w, h = im.size
        pixels = list(im.get_flattened_data() if hasattr(im, "get_flattened_data")
                      else im.getdata())
        colors = len(set(pixels))
        # 最常见颜色视为背景，统计「非背景」像素占比
        from collections import Counter
        cnt = Counter(pixels)
        bg, bg_n = cnt.most_common(1)[0]
        nonbg = 1.0 - bg_n / float(w * h)
        print("[framebuffer] 尺寸=%dx%d  颜色数=%d  最常见色=%s(%.1f%%)  非背景占比=%.3f" %
              (w, h, colors, bg, 100.0 * bg_n / (w * h), nonbg))
        ok_size = (w == W and h == H)
        ok_color = (colors >= 16)
        ok_draw = (nonbg >= 0.01)
        print("  判据: 尺寸800x600=%s  颜色数>=16=%s  非背景>=1%%=%s" %
              ("✓" if ok_size else "✗", "✓" if ok_color else "✗",
               "✓" if ok_draw else "✗"))
        if ok_size and ok_color and ok_draw:
            print("PASS 真实画面已建立：ramfb surface 800x600 且已绘制桌面")
            return 0
        print("FAIL 黑屏/未建 ramfb surface（screendump 是空屏）—— 检查 fw_cfg DMA 缓冲地址")
        return 1
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
