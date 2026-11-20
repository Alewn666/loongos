#!/usr/bin/env python3
"""60.0-P：验证「开机默认进 macOS 桌面」与「苹果菜单 → Classic Desktop 切回原桌面」。
- 开机（无 fw_cfg classic）应进 mac 桌面：g_ms.active_app 有效。
- 点苹果菜单(约 8,10) 展开，点最后一项 Classic Desktop(约 100,248)。
- 切换后原桌面接管：g_desktop_active==1，且串口出现原桌面就绪日志。
"""
import json, os, socket, subprocess, sys, time
ROOT = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("KERNEL", os.path.join(ROOT, "kernel.elf"))
SER="/tmp/loongos_switch_serial.sock"; QMP="/tmp/loongos_switch_qmp.sock"
LOG="/tmp/loongos_switch_serial.log"
for p in (SER,QMP,LOG):
    try: os.unlink(p)
    except OSError: pass
OK=[0]; FAIL=[0]
def check(n,ok,d=""):
    if ok: OK[0]+=1; print("  [ok] %s %s"%(n,d))
    else: FAIL[0]+=1; print("  [FAIL] %s %s"%(n,d))
def syms(elf):
    out=subprocess.check_output(["aarch64-linux-gnu-nm","-n",elf],text=True)
    return {p[2]:int(p[0],16) for p in (l.split() for l in out.splitlines()) if len(p)==3}
A=syms(KERNEL)
class Qmp:
    def __init__(self,path):
        self.s=None
        for _ in range(120):
            try: self.s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); self.s.connect(path); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError("QMP 连不上")
        self.s.settimeout(20); self.buf=b""; self.line(); self.q("qmp_capabilities")
    def line(self):
        while b"\n" not in self.buf: self.buf+=self.s.recv(65536)
        l,self.buf=self.buf.split(b"\n",1); return json.loads(l.decode("utf-8","replace"))
    def q(self,exe,arg=None):
        m={"execute":exe}
        if arg: m["arguments"]=arg
        self.s.sendall((json.dumps(m)+"\n").encode())
        while True:
            r=self.line()
            if "return" in r or "error" in r: return r
    def hmp(self,c): return self.q("human-monitor-command",{"command-line":c})
    def byte(self,a):
        r=self.hmp("xp /1bx 0x%x"%a); t=r.get("return","") or ""
        for tok in t.replace(":"," ").split():
            if tok.startswith("0x"): return int(tok,16)&0xFF
        return None
    def click(self,x,y):
        self.q("input-send-event",{"events":[
            {"type":"abs","data":{"axis":"x","value":x*32767//800}},
            {"type":"abs","data":{"axis":"y","value":y*32767//600}},
            {"type":"btn","data":{"down":True,"button":"left"}}]})
        time.sleep(0.08)
        self.q("input-send-event",{"events":[{"type":"btn","data":{"down":False,"button":"left"}}]})
        time.sleep(0.15)
def wait_log(pat,timeout=30):
    end=time.time()+timeout
    while time.time()<end:
        try:
            s=open(LOG,"rb").read().decode("utf-8","replace")
            if pat in s: return True
        except OSError: pass
        time.sleep(0.15)
    return False
def main():
    print("===== 60.0-P 默认桌面 + Classic 切换验收 =====")
    cmd=["qemu-system-aarch64","-machine","virt,gic-version=2","-cpu","cortex-a57",
         "-m","256","-smp","1","-kernel",KERNEL,"-display","none",
         "-serial","file:"+LOG,"-qmp","unix:%s,server=on,wait=off"%QMP,
         "-device","ramfb","-device","qemu-xhci,id=xhci",
         "-device","usb-tablet,bus=xhci.0,port=1",
         "-device","usb-mouse,bus=xhci.0,port=2",
         "-device","usb-kbd,bus=xhci.0,port=3"]
    p=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        q=Qmp(QMP); time.sleep(1.0)
        ok_mac=wait_log("macOS 风格桌面已就绪",30)
        check("开机默认进 macOS 桌面（无 classic 参数）",ok_mac)
        # active_app 偏移（编译期算）
        open("/tmp/off2.c","w").write('#include "macwm.h"\n#include <stdio.h>\n#include <stddef.h>\nint main(){printf("%zu",offsetof(struct mac_state,active_app));return 0;}\n')
        subprocess.check_call(["cc","-I",ROOT,"/tmp/off2.c","-o","/tmp/off2"])
        ao=int(subprocess.check_output(["/tmp/off2"]).decode())
        a0=q.byte(A["g_ms"]+ao)
        check("mac 状态有效 active_app",a0 is not None and 0<=a0<10,"active_app=%s"%a0)
        # 点苹果菜单展开
        q.click(9,10); time.sleep(0.3)
        # 点 Classic Desktop（苹果菜单第9项，y≈24+10+8*26=242）
        q.click(100,248); time.sleep(1.5)
        # 原桌面接管：g_desktop_active==1
        da=q.byte(A["g_desktop_active"]) if "g_desktop_active" in A else None
        ok_classic = (da==1) or wait_log("图形桌面已就绪",12)
        check("苹果菜单 Classic Desktop → 进原桌面",ok_classic,"g_desktop_active=%s"%da)
    finally:
        try: q.hmp("quit")
        except Exception: pass
        try: p.wait(timeout=5)
        except Exception: p.kill()
    print("\n===== 结果: %d PASS / %d FAIL ====="%(OK[0],FAIL[0]))
    sys.exit(0 if FAIL[0]==0 else 1)
if __name__=="__main__": main()
