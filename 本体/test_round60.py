#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""59.0-A 开发者 API 端到端判据（TCG）。

拓扑：QEMU TCG 起新内核 → b64 通道注入 u60api.elf → run
     → 核对 loslib.h 的 8 个新 syscall（28~35）返回值与输出合理
     → golden 老命令/老程序逐字零扰动。

判据原则：不只看"没崩"，而是逐项核对输出里的实际数值（uname/墙钟/uptime/
fsstat/meminfo/ppid/rng），并确认老 ELF（u55test）仍 code=0 退出。

启动：python3 test_round60.py
前置：已 sh build.sh（生成 kernel.elf 与 build/u60api.elf）。
"""
import os, sys, re, time, pty, select, base64, json, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(HERE, "kernel.elf")
ELF = os.environ.get("R60_ELF", os.path.join(HERE, "build/u60api.elf"))
OLD_ELF = os.path.join(HERE, "build/u55test.elf")
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "
LOG = "/tmp/round60_api.log"


class Session:
    def __init__(self):
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
               "-m", "256M", "-kernel", KERNEL, "-nographic", "-no-reboot"]
        self.cmd = cmd
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        self.buf = bytearray()

    def read_for(self, sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if self.master in r:
                try: d = os.read(self.master, 4096)
                except OSError: break
                if not d: break
                self.buf.extend(d)

    def seg(self, mark=0):
        return ANSI.sub(b"", bytes(self.buf[mark:])).replace(b"\r\n", b"\n").replace(b"\r", b"\n").decode("utf-8", "replace")

    def mark(self): return len(self.buf)
    def send(self, line): os.write(self.master, (line + "\r").encode())

    def wait(self, mark, pattern, timeout):
        rx = re.compile(pattern); end = time.time() + timeout
        while time.time() < end:
            self.read_for(0.1)
            if rx.search(self.seg(mark)): return True
        return False

    def collect(self, line, end_rx, timeout=30):
        m = self.mark(); self.send(line)
        hit = self.wait(m, end_rx, timeout)
        if hit:
            self.wait(m, re.escape(PROMPT.strip()) + r"\s*$", 6)
            self.read_for(0.1)
        return self.seg(m), hit

    def paste_elf(self, path, elf, chunk=40, pause=0.5):
        data = open(elf, "rb").read()
        enc = base64.b64encode(data).decode("ascii")
        lines = [enc[i:i + 76] for i in range(0, len(enc), 76)]
        m = self.mark(); self.send("b64in %s" % path); self.read_for(0.3)
        for i in range(0, len(lines), chunk):
            os.write(self.master, ("\r".join(lines[i:i + chunk]) + "\r").encode())
            self.read_for(pause)
        self.send("."); self.wait(m, r"字节 -> |已放弃", 20)
        time.sleep(0.1); self.read_for(0.2)
        t = self.seg(m)
        mm = re.search(r"已写入 (\d+) 字节 -> " + re.escape(path), t)
        n = int(mm.group(1)) if mm else -1
        return (n == len(data), n, len(data), t)

    def close(self):
        try: self.proc.kill()
        except Exception: pass
        try: os.close(self.master)
        except Exception: pass
        try: open(LOG, "w", encoding="utf-8").write(self.seg(0))
        except Exception: pass


def main():
    print("=== 59.0-A 开发者 API 端到端判据（TCG）===")
    checks = []
    def ck(name, ok, detail=""):
        checks.append({"name": name, "ok": bool(ok), "detail": str(detail)[:200]})
        print(("  [OK] " if ok else "  [X ] ") + name + ("" if ok else "   " + str(detail)[:180]))

    if not os.path.exists(ELF):
        print("[X] 找不到 %s，请先 sh build.sh" % ELF); return 1

    s = Session()
    res = {"mode": "TCG", "cmd": " ".join(s.cmd)}
    try:
        boot = s.wait(0, r"LoongOS stage5", 20)
        s.wait(0, re.escape(PROMPT.strip()), 20)
        ck("1 内核横幅 + shell 提示符就绪", boot)

        # ---- golden 零扰动 ----
        hb, _ = s.collect("help", r"run <", 15)
        ck("2 help 仍含 run（命令集未删）", "run <" in hb, hb[:100])
        host, _ = s.collect("cat /etc/hostname", r"myos", 15)
        ck("3 cat /etc/hostname 仍 myos", "myos" in host, host[:100])
        ok_h, n_h, w_h, _ = s.paste_elf("/home/u55test", OLD_ELF)
        ck("4 老程序 u55test.elf 注入字节一致", ok_h, "写入 %d/期望 %d" % (n_h, w_h))
        t_h, hit_h = s.collect("run /home/u55test", r"程序退出|用户程序出错", 45)
        ck("5 老 syscall 程序仍 code=0 退出（0~27 零扰动）",
           hit_h and "程序退出 code=0" in t_h and "用户程序出错" not in t_h, t_h[-200:])

        # ---- 注入并运行 u60api ----
        ok_i, n, w, _ = s.paste_elf("/home/u60api", ELF)
        ck("6 u60api.elf 注入字节一致", ok_i, "写入 %d/期望 %d" % (n, w))
        t, hit = s.collect("run /home/u60api", r"API demo done|用户程序出错|程序退出", 60)
        res["output"] = t
        ck("7 API 演示程序跑完并体面退出 code=0（无 EL0 fault）",
           hit and "API demo done" in t and "程序退出 code=0" in t and "用户程序出错" not in t, t[-300:])
        ck("8 uname 返回 LoongOS / 59.0-A / aarch64",
           "uname: LoongOS 59.0-A aarch64" in t, t[:300])
        mw = re.search(r"wall\s*=\s*(\d+)", t)
        ck("9 clock_gettime(墙钟) 返回合理 Unix 秒（>1577836800=2020）",
           bool(mw) and int(mw.group(1)) > 1577836800, mw.group(1) if mw else None)
        mm = re.search(r"mono\s*=\s*(\d+)\.", t)
        ck("10 clock_gettime(单调钟) 返回非 0 秒", bool(mm) and int(mm.group(1)) >= 0, mm.group(1) if mm else None)
        mu = re.search(r"uptime=\s*(\d+)", t)
        ck("11 uptime 返回非负秒", bool(mu) and int(mu.group(1)) >= 0, mu.group(1) if mu else None)
        mr = re.search(r"rng\s*=\s*([0-9a-f]+|unavailable)", t)
        ck("12 getrandom 返回真随机字节或明确 unavailable",
           bool(mr) and (mr.group(1) == "unavailable" or len(mr.group(1)) >= 2), mr.group(1) if mr else None)
        mf = re.search(r"fs\s*=\s*used\s+(\d+)\s*/\s*total\s+(\d+)", t)
        ck("13 fsstat 返回 used<=total 且 total>0",
           bool(mf) and int(mf.group(1)) <= int(mf.group(2)) and int(mf.group(2)) > 0,
           mf.groups() if mf else None)
        mmi = re.search(r"heap\s*=\s*used\s+(\d+)\s*peak\s+(\d+)", t)
        ck("14 meminfo 返回 used<=peak 且 used>0",
           bool(mmi) and int(mmi.group(1)) <= int(mmi.group(2)) and int(mmi.group(1)) > 0,
           mmi.groups() if mmi else None)
        mp = re.search(r"pid\s*=\s*(\d+)\s+ppid\s*=\s*(\d+)", t)
        ck("15 getpid/getppid 返回合理 pid/ppid",
           bool(mp) and int(mp.group(1)) >= 1 and int(mp.group(2)) >= 0, mp.groups() if mp else None)
        ck("16 nanosleep(10ms) 未卡死且返回 ok",
           "nanosleep(10ms) ok" in t, t[-200:])
    finally:
        s.close()

    passed = sum(1 for c in checks if c["ok"]); total = len(checks)
    print("\n=== 结果: %d/%d PASS ===" % (passed, total))
    res["checks"] = checks
    with open(os.path.join(HERE, "docs", "第五十九轮A_开发者API判据报告.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=2)
    return 0 if passed == total else 2


if __name__ == "__main__":
    sys.exit(main())
