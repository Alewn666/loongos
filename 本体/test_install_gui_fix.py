#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安装 App GUI 修复专项判据（用户报告「点到这里就装不了 / 卡死」）。

本轮三处修复的端到端验证：
  1. 二次确认超时 10s→30s：第一次点击 armed=1 后等 12 秒仍保持 armed
     （旧值 100 tick 此时早已撤销回退，用户会以为「点不动」）。
  2. 真正安装前先把终端窗口切到前台：第二次点击后目标盘写入 MBR，
     且安装输出经图形终端（G_len 增长），不再全程冻结无反馈。
  3. 端到端：两次点击真的装完盘（目标盘 MBR=55AA），证明「点这里能装」。

拓扑：QEMU virt/TCG + UEFI + ramfb + usb-tablet（绝对坐标点击）；
从 ISO 启动（安装盘 = install_media_present=1，开始菜单才有安装 App），
挂一块空白 virtio-blk 当目标盘。判据不靠 OCR：QMP 读内核静态符号 + 读盘 MBR。
用法：python3 test_install_gui_fix.py
"""
import json, os, shutil, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
from qemu_guard import preflight, release

SER = "/tmp/inst_gui.ser"; QMP = "/tmp/inst_gui.qmp"
VARSDIR = BASE + "/build"
D0 = os.path.join(BASE, "build", "inst_gui_d0.img")
ISO = os.path.join(BASE, "iso", "loongos.iso")
CODE = "/usr/share/AAVMF/AAVMF_CODE.fd"
W, H = 800, 600
SIZE_MB = 64
TITLE_H = 28

# struct dwin: gfx_rect{x,y,w,h} + z,app_id,visible,minimized,data,seq  = 40B
DWIN_SZ = 40
OFF_X, OFF_Y, OFF_W, OFF_H, OFF_VIS, OFF_AID = 0, 4, 8, 12, 24, 20
# install_draw: INST_ROW0=40 ROW_H=22 MAXROW=4 → by=40+4*22+14=142；确认按钮(cr.x+14, cr.y+by+36, 210x30)
BTN_X, BTN_Y, BTN_W, BTN_H = 14, 142 + 36, 210, 30

OK = [0]; FAIL = [0]
def ck(name, cond, extra=""):
    (OK if cond else FAIL)[0] += 1
    print(("  \u2705 " if cond else "  \u274c ") + name + (("  " + extra) if extra else ""), flush=True)

if not preflight(need_mb=256, label="test_install_gui_fix.py"):
    sys.exit(2)

def sym_addrs(elf):
    out = subprocess.check_output(["aarch64-linux-gnu-nm", "-n", elf], text=True)
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}

A = sym_addrs(os.path.join(BASE, "kernel.elf"))

class Qmp:
    def __init__(self, path):
        self.s = None
        for _ in range(150):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); self.s.connect(path); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError("QMP 连不上")
        self.s.settimeout(8); self.buf = b""; self.line(); self.q("qmp_capabilities")
    def line(self):
        while b"\n" not in self.buf: self.buf += self.s.recv(65536)
        l, self.buf = self.buf.split(b"\n", 1); return json.loads(l.decode("utf-8", "replace"))
    def q(self, exe, arg=None):
        m = {"execute": exe}
        if arg: m["arguments"] = arg
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self.line()
            if "return" in r or "error" in r: return r
    def hmp(self, c): return self.q("human-monitor-command", {"command-line": c})
    def words(self, addr, n):
        txt = self.hmp("xp /%dwx 0x%x" % (n, addr)).get("return", "") or ""
        return [int(t, 16) & 0xFFFFFFFF for t in txt.replace(":", " ").split() if t.startswith("0x")]
    def r32(self, name, off=0):
        v = self.words(A[name] + off, 1); return v[0] if v else None
    def abs_move(self, x, y):
        self.q("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}]})
    def btn(self, down):
        self.q("input-send-event", {"events": [{"type": "btn", "data": {"down": bool(down), "button": "left"}}]})
    def click(self, x, y, hold=0.15, settle=0.35):
        self.abs_move(x, y); time.sleep(0.1); self.btn(True); time.sleep(hold); self.btn(False); time.sleep(settle)
    def win_rect(self, app_id):
        for wi in range(12):
            b = A["G_win"] + wi * DWIN_SZ
            v = self.words(b, 10)
            if len(v) < 7: continue
            if v[OFF_VIS // 4] and v[OFF_AID // 4] == app_id:
                return v[OFF_X // 4], v[OFF_Y // 4], v[OFF_W // 4], v[OFF_H // 4]
        return None

def mbr_ok(path):
    with open(path, "rb") as f: sec = f.read(512)
    return len(sec) == 512 and sec[510] == 0x55 and sec[511] == 0xAA

def main():
    with open(D0, "wb") as f: f.truncate(SIZE_MB * 1024 * 1024)
    vars = os.path.join(VARSDIR, "inst_gui_vars.fd")
    shutil.copy("/usr/share/AAVMF/AAVMF_VARS.fd", vars)
    for pth in (SER, QMP):
        try: os.unlink(pth)
        except OSError: pass
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=3", "-cpu", "cortex-a57",
           "-m", "256", "-smp", "1",
           "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
           "-drive", "if=pflash,format=raw,file=" + vars,
           "-serial", "unix:%s,server,nowait" % SER,
           "-qmp", "unix:%s,server=on,wait=off" % QMP,
           "-device", "ramfb",
           "-device", "qemu-xhci,id=xhci",
           "-device", "usb-tablet,bus=xhci.0,port=1",
           "-drive", "if=none,id=d0,format=raw,file=" + D0,
           "-device", "virtio-blk-pci,drive=d0",
           "-cdrom", ISO, "-display", "none", "-no-reboot"]
    err = open(os.path.join(BASE, "build", "inst_gui_qemu.err"), "wb")
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    try:
        q = Qmp(QMP)
        end = time.time() + 90
        while time.time() < end:
            if q.r32("g_desktop_active") == 1: break
            time.sleep(0.3)
        ck("安装盘图形桌面已就绪", q.r32("g_desktop_active") == 1)
        if q.r32("g_desktop_active") != 1: raise RuntimeError("桌面没起来")

        # 定位「安装系统」app_id：G_apps 数组里非 NULL 的最大下标即 A_INSTALL
        aid_max = -1
        ga = q.words(A["G_apps"], 12)
        for i, ptr in enumerate(ga):
            if ptr: aid_max = i
        ck("安装 App 已注册（安装盘）", aid_max >= 0, "app_id=%s" % aid_max)
        if aid_max < 0: raise RuntimeError("安装 App 未注册")

        # 开始菜单 → 安装系统
        q.click(49, 579)
        time.sleep(0.4)
        n = q.r32("G_menu_n") or 0
        idx = -1
        for i in range(n):
            b = A["G_menu"] + i * 24
            typ = q.words(b + 8, 1)[0]; m_aid = q.words(b + 12, 1)[0]
            if typ == 0 and m_aid == aid_max: idx = i; break
        ck("开始菜单含安装系统项", idx >= 0, "menu_n=%d idx=%d" % (n, idx))
        if idx < 0: raise RuntimeError("菜单无安装项")
        h = 8 + n * 26 + 8; y0 = H - 42 - h
        q.click(126, y0 + 19 + idx * 26)
        time.sleep(0.6)

        r = q.win_rect(aid_max)
        ck("安装 App 窗口已打开", r is not None, "r=%s" % (r,))
        if not r: raise RuntimeError("安装窗口没打开")
        wx, wy = r[0], r[1]
        # 内容区左上 = (wx+1, wy+TITLE_H)；确认按钮中心（内容区局部 14+105, by+36+15）
        bx = wx + 1 + BTN_X + BTN_W // 2
        by = wy + TITLE_H + BTN_Y + BTN_H // 2

        # 第一次点击：武装
        q.click(bx, by)
        time.sleep(0.3)
        armed1 = q.r32("G_install_armed")
        ck("第一次点击后进入二次确认(armed=1)", armed1 == 1, "armed=%s" % armed1)

        # 等 12 秒：旧版 10s 窗口此时会超时撤销，新版 30s 应保持 armed
        time.sleep(12)
        armed2 = q.r32("G_install_armed")
        ck("12 秒后仍处于 armed（超时已放宽到 30s）", armed2 == 1, "armed=%s" % armed2)

        # 第二次点击：真正安装（同步写盘，QMP 命令会等它跑完才返回，故设长 settime）
        gt_before = q.r32("G_len") or 0
        q.click(bx, by, hold=0.2, settle=2.0)
        # 轮询等安装完成：目标盘出现 MBR 55AA
        end = time.time() + 120
        while time.time() < end and not mbr_ok(D0): time.sleep(1.0)
        ck("第二次点击真的装完盘（MBR=55AA）", mbr_ok(D0))
        gt_after = q.r32("G_len") if "G_len" in A else None
        ck("安装输出进入图形终端历史（实时可见）",
           (gt_after or 0) > gt_before, "G_len %d→%s" % (gt_before, gt_after))
        ck("gfxterm_live 已链接（实时刷新支撑）", "gfxterm_live" in A)
    finally:
        try: p.terminate(); p.wait(timeout=5)
        except Exception: p.kill()
        print("\n==== 结果: %d 通过 / %d 失败 ====" % (OK[0], FAIL[0]), flush=True)
        release()
    return FAIL[0]

if __name__ == "__main__":
    sys.exit(1 if main() else 0)
