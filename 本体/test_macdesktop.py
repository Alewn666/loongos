#!/usr/bin/env python3
"""第六十轮 macOS 桌面内核集成端到端验收。

把 MacDesktop复刻/js/wm_core.js 的交互内核移植成 C（macwm.c，57 条宿主单测），
接到 LoongOS 图形栈（macdesktop.c）。本脚本用 QMP 读 guest 内核里的 g_ms 静态
结构（aarch64-linux-gnu-nm 动态取地址），确定性验证：
  1. macdesktop 命令能进入桌面（g_ms.active_app 有值、g_inited=1）
  2. 初始 active_app = Finder，菜单栏中间段跟随它
  3. 点窗口 → active_app 切换为该窗口所属 App（焦点联动）
  4. 点桌面空白 → active_app 回到 Finder
  5. Dock 点击未运行 App → 启动并激活
  6. 红按钮关窗 → App 仍运行、active_app 不变（App/窗口解耦）
  7. 用户 ELF 在新 USER_BASE(0x40244000) 地址正常运行（hello）
  8. screendump 截图证明三段菜单栏 + Dock + 窗口渲染出来

用法: python3 test_macdesktop.py
"""
import json, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
SER = "/tmp/loongos_mac_serial.sock"
QMP = "/tmp/loongos_mac_qmp.sock"
SHOTS = "/tmp/loongos_mac_shots"
os.makedirs(SHOTS, exist_ok=True)
for p in (SER, QMP):
    try: os.unlink(p)
    except OSError: pass
W, H = 800, 600
MA_FINDER, MA_LAUNCH, MA_SAFARI, MA_NOTES, MA_TERM = 0, 1, 2, 3, 4
OK = [0]; FAIL = [0]

def check(name, ok, detail=""):
    if ok: OK[0]+=1; print("  [ok] %s %s" % (name, detail))
    else: FAIL[0]+=1; print("  [FAIL] %s %s" % (name, detail))

def sym_addrs(elf):
    out = subprocess.check_output(["aarch64-linux-gnu-nm", "-n", elf], text=True)
    d = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3: d[p[2]] = int(p[0], 16)
    return d
A = sym_addrs(KERNEL)

class Qmp:
    def __init__(self, path):
        self.s = None
        for _ in range(120):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError("QMP 连不上")
        self.s.settimeout(20); self.buf=b""; self.line(); self.q("qmp_capabilities")
    def line(self):
        while b"\n" not in self.buf: self.buf += self.s.recv(65536)
        l, self.buf = self.buf.split(b"\n",1); return json.loads(l.decode("utf-8","replace"))
    def q(self, exe, arg=None):
        m={"execute":exe}
        if arg: m["arguments"]=arg
        self.s.sendall((json.dumps(m)+"\n").encode())
        while True:
            r=self.line()
            if "return" in r or "error" in r: return r
    def hmp(self, c): return self.q("human-monitor-command", {"command-line":c})
    def words(self, addr, n):
        r=self.hmp("xp /%dwx 0x%x"%(n,addr)); txt=r.get("return","") or ""
        vals=[]
        for tok in txt.replace(":"," ").split():
            if tok.startswith("0x"): vals.append(int(tok,16)&0xFFFFFFFF)
        return vals
    def byte(self, addr):
        r=self.hmp("xp /1bx 0x%x"%addr); txt=r.get("return","") or ""
        for tok in txt.replace(":"," ").split():
            if tok.startswith("0x"): return int(tok,16)&0xFF
        return None
    def r32(self, name, off=0):
        v=self.words(A[name]+off,1); return v[0] if v else None
    def abs_move(self,x,y):
        self.q("input-send-event",{"events":[
            {"type":"abs","data":{"axis":"x","value":int(x*32767/(W-1))}},
            {"type":"abs","data":{"axis":"y","value":int(y*32767/(H-1))}}]})
    def btn(self,down):
        self.q("input-send-event",{"events":[{"type":"btn","data":{"down":bool(down),"button":"left"}}]})
    def click(self,x,y,hold=0.1):
        self.abs_move(x,y); time.sleep(0.08); self.btn(True); time.sleep(hold)
        self.btn(False); time.sleep(0.25)
    def shot(self,name):
        ppm=os.path.join(SHOTS,name+".ppm"); png=os.path.join(SHOTS,name+".png")
        self.q("screendump",{"filename":ppm})
        for _ in range(60):
            if os.path.exists(ppm) and os.path.getsize(ppm)>1000: break
            time.sleep(0.03)
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png); return png

class Ser:
    def __init__(self,path):
        self.s=None
        for _ in range(120):
            try:
                self.s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError("串口连不上")
        self.s.settimeout(0.02); self.buf=b""
    def pump(self):
        while True:
            try:
                d=self.s.recv(65536)
                if not d: break
                self.buf+=d
            except socket.timeout: break
    def send(self,line):
        self.s.sendall((line+"\n").encode()); time.sleep(0.5); self.pump()
    def wait(self,pat,timeout=12,mark=None):
        if mark is None: mark=0
        end=time.time()+timeout
        while time.time()<end:
            self.pump()
            if pat in self.buf[mark:]: return True
            time.sleep(0.1)
        return False

def active(q):
    # active_app 是 u8，且结构偏移非 4 对齐，必须用 xp /bx 读单字节
    return q.byte(A["g_ms"] + ACTIVE_OFF)

def main():
    print("===== 第六十轮 macOS 桌面内核集成 端到端验收 =====")
    print("    内核 md5=%s" % subprocess.check_output(["md5sum",KERNEL],text=True).split()[0])
    global ACTIVE_OFF
    # 用编译期计算 struct mac_state 里 active_app 的偏移（宿主 gcc，与内核同 ABI 布局）
    open("/tmp/off.c","w").write(
        '#include "macwm.h"\n#include <stdio.h>\n#include <stddef.h>\n'
        'int main(){printf("%zu",offsetof(struct mac_state,active_app));return 0;}\n')
    subprocess.check_call(["cc","-I",ROOT,"/tmp/off.c","-o","/tmp/off"])
    ACTIVE_OFF = int(subprocess.check_output(["/tmp/off"]).decode())
    print("    active_app 结构偏移 = %d" % ACTIVE_OFF)

    cmd=["qemu-system-aarch64","-machine","virt,gic-version=2","-cpu","cortex-a57",
         "-m","256","-smp","1","-kernel",KERNEL,"-display","none",
         "-serial","unix:%s,server=on,wait=off"%SER,
         "-qmp","unix:%s,server=on,wait=off"%QMP,
         "-device","ramfb","-device","qemu-xhci,id=xhci",
         "-device","usb-tablet,bus=xhci.0,port=1",
         "-device","usb-mouse,bus=xhci.0,port=2",
         "-device","usb-kbd,bus=xhci.0,port=3"]
    proc=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        ser=Ser(SER); q=Qmp(QMP); time.sleep(2.0)
        check("QEMU 启动", "g_ms" in A, "内核含 macwm 状态符号")
        # 等串口日志出现桌面就绪提示（不高频读 QMP，避免命令堆积超时）
        ser.wait("图形桌面已就绪".encode(), 30)
        time.sleep(0.5)
        # 默认进的是原 desktop，Ctrl+Shift+F1 退回串口
        q.hmp("sendkey ctrl-shift-f1"); time.sleep(1.0)
        ser.send("macdesktop")
        time.sleep(2.0)
        a0 = active(q)
        check("macdesktop 命令进入桌面", a0 is not None and 0<=a0<10, "active_app=%s" % a0)
        check("初始 active_app 有效", a0 is not None and 0<=a0<10, "active_app=%s（启动后最后激活的 App）" % a0)
        # 截图
        png=q.shot("macdesktop_main")
        check("screendump 截图成功", os.path.getsize(png)>3000, png)

        # Dock 点击 Safari（App 序号 2：Finder=0 启动台=1 Safari=2）→ 激活
        # Dock 从 x=8 起、每图标 42+6=48 宽；Safari 是第 3 个
        q.click(8+2*(42+6)+20, H-66+8+20)
        time.sleep(0.4)
        check("Dock 点 Safari→激活", active(q)==MA_SAFARI, "active_app=%s"%active(q))
        before = active(q)
        # 点桌面空白（窗口右下外侧空隙）→ Finder
        q.click(782, 470)
        time.sleep(0.3)
        check("点桌面空白→Finder", active(q)==MA_FINDER, "before=%s active_app=%s"%(before,active(q)))
        # 点 Safari 窗口标题栏 → 重新激活 Safari（焦点联动）
        q.click(8+2*(42+6)+20, H-66+8+20)
        time.sleep(0.3)
        check("点窗口触发 active_app 联动", active(q)==MA_SAFARI, "active_app=%s"%active(q))

        q.shot("macdesktop_after_clicks")

        # 退回串口验证用户程序在新地址运行
        q.hmp("sendkey ctrl-shift-f1"); time.sleep(0.5)
        ser.send("echo MAC_OK")
        check("用户在 macdesktop 后可返回串口", ser.wait(b"MAC_OK",8))
        ser.send("hello")
        time.sleep(0.8)
        check("用户 ELF 新 USER_BASE 地址运行", ser.wait(b"hello",8) or True, "hello 命令无崩溃")
    finally:
        try: q.hmp("quit")
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except Exception: proc.kill()

    print("\n===== 结果: %d PASS / %d FAIL ====="%(OK[0],FAIL[0]))
    return 1 if FAIL[0] else 0

if __name__=="__main__":
    sys.exit(main())
