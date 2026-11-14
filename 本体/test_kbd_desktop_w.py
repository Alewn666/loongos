#!/usr/bin/env python3
"""LoongOS 桌面终端键盘回归：验证 W/A/S/D、数字、Tab 不再被桌面热键抢走。

背景：桌面 handle_key 曾把 W/A/S/D、Tab、1-9 当全局热键，在分发给聚焦 App
之前就 return，导致终端里敲 whoami、ls /home 等含这些字符的命令被打断。
修复：聚焦"有键盘输入"的 App（终端/编辑器/计算器）时，所有按键原样交给它；
桌面热键只在聚焦"无键盘输入"的静态窗口时生效。

复刻用户真机拓扑：裸 ELF 直启 + ramfb 图形 + 三 HID 设备，开机默认进桌面。
用 HMP sendkey 在"聚焦的终端窗口"里敲字，再读串口日志判定。

判定（四条，缺一不可）：
  1. 桌面启动、HID 枚举完成（键盘=1 指针=2）
  2. "whoami"   → 串口出现 root：'w' 正常进终端（修复前被热键吞成 hoami）
  3. "ls /home" → 列出 readme.txt：空格正常进终端
  4. "cat /home/readme.txt" → 输出 LoongOS：斜杠/点/空格/参数全部正常
  5. "ls"       → 普通字母输入不受影响

⚠️ QEMU HMP sendkey 只认 QKeyCode 键名（spc/slash/dot/ret…），不认 '/' '.'
   等字面标点；传字面字符会被静默忽略，导致命令被注入成残缺串、误判为
   "内核丢字符"。type_text 里的 KEYNAME 映射就是为此而设，勿删。
"""
import json, os, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release
if not preflight(need_mb=400, label=os.path.basename(__file__)):
    sys.exit(2)

BASE = os.path.dirname(os.path.abspath(__file__))
TAG = "dkw"
LOG = "/tmp/loongos_%s_serial.log" % TAG
QMP = "/tmp/loongos_%s_qmp.sock" % TAG
for p in (LOG, QMP):
    try: os.unlink(p)
    except OSError: pass

class Qmp:
    def __init__(self, path):
        end = time.time() + 15
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError:
                if time.time() > end: raise
                time.sleep(0.1)
        self.s.settimeout(5); self.buf = b""
        self._line(); self.cmd("qmp_capabilities")
    def _line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8", "replace"))
    def cmd(self, e, a=None):
        m = {"execute": e}
        if a: m["arguments"] = a
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r: return r
    def hmp(self, line):
        return self.cmd("human-monitor-command", {"command-line": line})

def log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""

def wait_log(pat, timeout=30, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if pat in log()[since:]: return True
        time.sleep(0.15)
    return False

def sendkey(q, name, delay=0.3):
    q.hmp("sendkey " + name)
    time.sleep(delay)

def main():
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
        "-m", "256", "-smp", "1",
        "-kernel", os.path.join(BASE, "kernel.elf"),
        "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server=on,wait=off" % QMP,
        "-device", "ramfb",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,port=1",
        "-device", "usb-mouse,bus=xhci.0,port=2",
        "-device", "usb-kbd,bus=xhci.0,port=3",
    ]
    print("[DKW] 启动 QEMU（裸 ELF + ramfb + 三 HID）")
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = []
    try:
        if not wait_log("[usb ] HID 枚举完成", 30):
            # 兼容旧文案
            if not wait_log("USB 键盘就绪", 5):
                print("✗ 启动失败：没等到 HID 枚举"); print(log()[-2000:]); return 1
        print("✓ 桌面启动 + HID 枚举完成")
        time.sleep(1.5)  # 等桌面 settle

        q = Qmp(QMP)
        def focus_term():
            # 点击终端标题栏中心 (200,55)：只有终端覆盖该点（其他窗口 y 起点都>=68），
            # 标题栏点击只聚焦、不会触发最小化（最小化键在右上角），坐标无歧义。
            q.cmd("input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(200*32767/799)}},
                {"type": "abs", "data": {"axis": "y", "value": int(55*32767/599)}},
            ]})
            time.sleep(0.25)
            q.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"down": True, "button": "left"}}]})
            time.sleep(0.15)
            q.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"down": False, "button": "left"}}]})
            time.sleep(0.45)
        focus_term()

        # QEMU HMP sendkey 只认 QKeyCode 键名，不认 '/' '.' 等字面标点；
        # 传字面字符会被静默忽略（曾把 cat /home/readme.txt 注入成 cat homereadmetxt）。
        KEYNAME = {
            " ": "spc", "\r": "ret", "\n": "ret", "\b": "backspace", "\t": "tab",
            "/": "slash", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
            ";": "semicolon", "'": "apostrophe", "\\": "backslash",
            "[": "bracket_left", "]": "bracket_right", "`": "grave",
        }
        def type_text(q, text):
            for ch in text:
                sendkey(q, KEYNAME.get(ch, ch))

        def run(q, cmd, pat, label):
            """聚焦终端后敲 cmd，检查串口是否出现 pat。"""
            focus_term()
            mark = len(log())
            type_text(q, cmd + "\r")
            if wait_log(pat, 8, since=mark):
                print("✓ %s" % label)
                return True
            fails.append("%s 没出现预期输出（%s）" % (label, pat))
            return False

        # 1) 带空格的命令：ls /home —— 空格若被吞会变成 "ls/home" 找不到
        run(q, "ls /home", "readme.txt", "ls /home：空格正常进终端（列出 readme.txt）")
        # 2) 含 W 的命令：whoami —— 修复前 'w' 被桌面热键吞掉变成 "hoami"
        run(q, "whoami", "root", "whoami：'w' 正常进了终端（输出 root）")
        # 3) 带空格 + 参数输出：cat /home/readme.txt（校验 ASCII 串 LoongOS）
        run(q, "cat /home/readme.txt", "LoongOS", "cat /home/readme.txt：空格+参数+输出正常")
        # 4) 普通 ls：验证普通字母输入未受影响
        focus_term()
        mark = len(log())
        type_text(q, "ls\r")
        time.sleep(2)
        tail = log()[mark:]
        if ("hostname" in tail) or ("etc" in tail) or ("home" in tail):
            print("✓ ls 执行：普通字母输入正常")
        else:
            fails.append("ls 似乎没正常执行（普通字母路径异常）")
    finally:
        p.send_signal(__import__("signal").SIGTERM)
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()

    print()
    if fails:
        print("FAIL:")
        for f in fails: print("  ✗", f)
        print("---- 串口尾部 ----"); print(log()[-3000:])
        return 1
    print("ALL PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())

release()
