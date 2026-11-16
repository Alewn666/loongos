#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十七轮 · 进程模型双路判据脚本（TCG / 真机 KVM）。

第五十七轮做了什么（演进路线 §六 第 57 轮 / §3.2）：
  - proc.c/h：进程表状态机（分配/僵尸/回收）+ 用户空间全拷贝，宿主单测 30/30；
  - 每进程独立用户地址空间：fork 复制 L3 表 + 独立物理页，切换改 l2_ram[1]+TLBI；
  - 合作式 fork / wait / procexit / getpid（syscall 22~25），帧整体拷贝切换；
  - 跨 run 零泄漏：run_userprog 装载前与 el1_landing 都回收全部子进程页。

判据原则（文档 §八「不接受串口打印说成功」）：
  u57_proc.elf 打印的 [R57] pass= 只是自报。本脚本独立重算关键量：
  pid_before=1（老语义）、fork_ret>=2、wait_ret==fork_ret、status=7、
  shared_before=shared_after=0x1111（子进程改 0x2222 未污染父页）、
  pid_after=1（父身份不变）、无 EL0 fault、res 健康计数全 0、golden 逐字不变。

启动：
  python3 test_round57.py
  R57_KERNEL=<kernel.elf> python3 test_round57.py
"""
import os, sys, re, json, time, pty, select, base64, hashlib, subprocess, platform

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("R57_KERNEL", os.path.join(HERE, "kernel.elf"))
ELF = os.environ.get("R57_ELF", os.path.join(HERE, "build/u57proc.elf"))

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "
RE_KV = re.compile(r"^\[R57\] ([A-Za-z0-9_]+)=(.*)$", re.M)


def kvs(t):
    return {k.strip(): v.strip() for k, v in RE_KV.findall(t)}


def kvi(d, k):
    try:
        return int(d.get(k, ""), 0) if d.get(k, "").startswith("0x") else int(d.get(k, ""))
    except (ValueError, TypeError, AttributeError):
        return None


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


class Session:
    def __init__(self, mode):
        self.mode = mode
        self.kvm = (mode == "KVM")
        gic = "3" if self.kvm else "2"
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=" + gic,
               "-cpu", "host" if self.kvm else "cortex-a57"]
        if self.kvm:
            cmd += ["-enable-kvm"]
        cmd += ["-m", "256M", "-kernel", KERNEL, "-nographic", "-no-reboot"]
        self.cmd = cmd
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(cmd, stdin=slave, stdout=slave,
                                     stderr=slave, close_fds=True)
        os.close(slave)
        self.buf = bytearray()
        self.log_path = "/tmp/loong_round57_%s.log" % mode.lower()

    def read_for(self, sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if self.master in r:
                try:
                    d = os.read(self.master, 4096)
                except OSError:
                    break
                if not d:
                    break
                self.buf.extend(d)

    def seg(self, mark=0):
        raw = ANSI.sub(b"", bytes(self.buf[mark:]))
        return raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n").decode("utf-8", "replace")

    def mark(self):
        return len(self.buf)

    def send(self, line, cr="\r"):
        os.write(self.master, (line + cr).encode())

    def wait(self, mark, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            self.read_for(0.1)
            if rx.search(self.seg(mark)):
                return True
        return False

    def collect(self, line, end_rx, timeout=30):
        m = self.mark()
        self.send(line)
        hit = self.wait(m, end_rx, timeout)
        if hit:
            self.wait(m, re.escape(PROMPT.strip()) + r"\s*$", 6)
            time.sleep(0.05)
            self.read_for(0.1)
        return self.seg(m), hit

    def paste_elf(self, path, elf, chunk=40, pause=0.5):
        data = open(elf, "rb").read()
        enc = base64.b64encode(data).decode("ascii")
        lines = [enc[i:i + 76] for i in range(0, len(enc), 76)]
        m = self.mark()
        self.send("b64in %s" % path)
        self.read_for(0.3)
        for i in range(0, len(lines), chunk):
            os.write(self.master, ("\r".join(lines[i:i + chunk]) + "\r").encode())
            self.read_for(pause)
        self.send(".")
        self.wait(m, r"字节 -> |已放弃", 20)
        time.sleep(0.1)
        self.read_for(0.2)
        t = self.seg(m)
        mm = re.search(r"已写入 (\d+) 字节 -> " + re.escape(path), t)
        n = int(mm.group(1)) if mm else -1
        return (n == len(data), n, len(data), t)

    def close(self):
        try: self.proc.kill()
        except Exception: pass
        try: os.close(self.master)
        except Exception: pass
        try:
            with open(self.log_path, "w", encoding="utf-8") as f:
                f.write(self.seg(0))
        except Exception: pass


def run_mode(mode):
    print("\n" + "=" * 70)
    print("  %s 路  %s" % (mode, "(-enable-kvm -cpu host, GICv3)"
                          if mode == "KVM" else "(QEMU TCG, cortex-a57, GICv2)"))
    print("=" * 70)
    s = Session(mode)
    checks = []
    res = {"mode": mode, "cmd": " ".join(s.cmd)}

    def ck(name, ok, detail=""):
        checks.append({"name": name, "ok": bool(ok), "detail": str(detail)})
        print(("  [OK] " if ok else "  [X ] ") + name +
              ("" if ok else "   " + str(detail)[:180]))

    try:
        boot = s.wait(0, r"LoongOS stage5", 20)
        s.wait(0, re.escape(PROMPT.strip()), 20)
        ck("1 内核横幅 + shell 提示符就绪", boot, "未见横幅/提示符")

        # golden 敏感路径：老程序 getpid 语义、老命令不变（独立核对）
        hb, _ = s.collect("help", r"run <", 20)
        ck("2 help 仍含 run 命令（golden 命令集未删）", "run <" in hb, hb[:120])
        host, _ = s.collect("cat /etc/hostname", r"myos", 20)
        ck("3 cat /etc/hostname 仍为 myos（老 fs 路径逐字不变）",
           "myos" in host and "MemTotal" not in host, host[:120])

        # 老用户程序 getpid 仍返回 1（初始进程，进程模型门控不扰动老程序）
        ok_inj_h, n_h, w_h, _ = s.paste_elf("/home/u55test", os.path.join(HERE, "build/u55test.elf"))
        ck("4 u55test.elf（老程序）注入字节一致", ok_inj_h, "写入 %d / 期望 %d" % (n_h, w_h))
        t_h, hit_h = s.collect("run /home/u55test", r"程序退出|用户程序出错", 45)
        ck("5 老 syscall 程序仍体面退出 code=0（0~21 零扰动）",
           hit_h and "程序退出 code=0" in t_h and "用户程序出错" not in t_h, t_h[-200:])

        # 注入 u57 进程模型验证程序
        ok_inj, n, w, _ = s.paste_elf("/home/u57proc", ELF)
        ck("6 u57proc.elf 经 b64 注入字节数一致", ok_inj, "写入 %d / 期望 %d" % (n, w))
        ls_home, _ = s.collect("ls /home", r"u57proc", 20)
        ck("7 ls /home 含新程序", "u57proc" in ls_home, ls_home[:160])

        t, hit = s.collect("run /home/u57proc", r"R57. done|程序退出|用户程序出错", 60)
        d = kvs(t)
        res["u57proc"] = d
        ck("8 用户程序跑完并体面退出 code=0（无 EL0 fault）",
           hit and "程序退出 code=0" in t and "用户程序出错" not in t, t[-240:])
        ck("9 自报 pass=5 且 fail=0",
           kvi(d, "pass") == 5 and kvi(d, "fail") == 0,
           "pass=%s fail=%s" % (d.get("pass"), d.get("fail")))

        # 独立重算关键量（不靠自报）
        ck("10 pid_before=1（初始进程，老 getpid 语义）", kvi(d, "pid_before") == 1,
           d.get("pid_before"))
        fp = kvi(d, "fork_ret")
        ck("11 fork_ret>=2（子进程 pid 从 2 起）", fp is not None and fp >= 2, d.get("fork_ret"))
        ck("12 wait_ret==fork_ret（wait 返回子 pid）", kvi(d, "wait_ret") == fp,
           "wait=%s fork=%s" % (d.get("wait_ret"), d.get("fork_ret")))
        ck("13 status=7（子进程 procexit 退出码正确回传）", kvi(d, "status") == 7, d.get("status"))
        ck("14 shared_before=0x1111 且 child 见到自己写 0x2222",
           kvi(d, "shared_before") == 0x1111 and kvi(d, "child_shared_w") == 0x2222 and
           kvi(d, "child_shared_r") == 0x2222,
           "before=%s cw=%s cr=%s" % (d.get("shared_before"), d.get("child_shared_w"),
                                      d.get("child_shared_r")))
        ck("14b 子进程视角 fork() 返回 0", kvi(d, "child_forkret") == 0,
           d.get("child_forkret"))
        ck("15 地址隔离：shared_after=0x1111（子改自己页未污染父）",
           kvi(d, "shared_after") == 0x1111, d.get("shared_after"))
        ck("16 child_pid==fork_ret（子进程视角 getpid=子 pid）",
           kvi(d, "child_pid") == fp, "child=%s fork=%s" % (d.get("child_pid"), d.get("fork_ret")))
        ck("17 pid_after=1（父从 wait 恢复后仍是初始进程）", kvi(d, "pid_after") == 1,
           d.get("pid_after"))

        # 跨 run 零泄漏：再跑一次同一程序，mmstat 峰值不应累积（子进程页已回收）
        t2, hit2 = s.collect("run /home/u57proc", r"R57. done|程序退出", 60)
        ck("18 二次 run 同样 pass=5 fail=0（跨 run 进程表干净可复用）",
           hit2 and kvi(kvs(t2), "pass") == 5 and kvi(kvs(t2), "fail") == 0,
           t2[-160:])
        ms, _ = s.collect("mmstat", r"audit", 20)
        ck("19 mmstat audit=0（子进程页全部回收，堆无泄漏）",
           re.search(r"audit\s*=\s*0", ms) is not None, ms[:200])

        rr, _ = s.collect("res", r"NISV", 20)
        hm = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                       r"FIQ/SErr=(\d+)\s+熔断=(\d+)", rr)
        res["res_after"] = {"sync": int(hm.group(1)), "nisv": int(hm.group(3)),
                            "fiq": int(hm.group(4)), "trip": int(hm.group(5))} if hm else None
        ck("20 结束后 res 健康计数全 0（进程切换未触发异常）",
           bool(hm) and all(int(hm.group(i)) == 0 for i in (1, 2, 3, 4, 5)),
           hm.group(0) if hm else rr[-120:])

        res["checks"] = checks
        res["summary"] = {"total": len(checks), "pass": sum(1 for c in checks if c["ok"]),
                          "fail": sum(1 for c in checks if not c["ok"]),
                          "failed": [c["name"] for c in checks if not c["ok"]]}
    finally:
        s.close()
    print("  ---- %s 路：%d/%d ----" %
          (mode, res["summary"]["pass"], res["summary"]["total"]))
    return res


def main():
    report = {"title": "LoongOS 第五十七轮 进程模型判据报告",
              "kernel_md5": md5(KERNEL), "elf_proc_md5": md5(ELF),
              "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "modes": {}}
    modes = ["TCG"]
    why = None
    if platform.machine() != "aarch64":
        why = "宿主为 %s，无 aarch64 KVM 能力" % platform.machine()
    elif not os.path.exists("/dev/kvm"):
        why = "无 /dev/kvm"
    if os.environ.get("KVM_FORCE") == "1":
        modes.append("KVM")
        print("[r57] 警告：%s；KVM_FORCE=1 仍尝试（可能失败）" % (why or ""))
    elif why:
        print("[r57] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)
    else:
        modes.append("KVM")

    for mode in modes:
        try:
            report["modes"][mode] = run_mode(mode)
        except Exception as e:
            report["modes"][mode] = {"error": repr(e),
                                     "summary": {"pass": 0, "fail": -1, "failed": [repr(e)]}}
            print("  [X ] %s 路异常：%r" % (mode, e))

    print("\n" + "=" * 70)
    print("  TCG vs KVM 第 57 轮对照表（同一 kernel md5 %s…）" % report["kernel_md5"][:12])
    print("=" * 70)
    rows = [("pid_before", "u57proc", "pid_before"),
            ("fork_ret", "u57proc", "fork_ret"),
            ("wait_ret", "u57proc", "wait_ret"),
            ("status", "u57proc", "status"),
            ("shared_after", "u57proc", "shared_after"),
            ("自报 pass", "u57proc", "pass"),
            ("结束后 NISV", "res_after", "nisv")]
    def cell(mode, sec, key):
        r = report["modes"].get(mode)
        if not r or r.get("summary", {}).get("fail") == -1:
            return "—"
        v = r.get(sec, {})
        if not isinstance(v, dict):
            return "—"
        v = v.get(key)
        return "—" if v is None else str(v)[:14]
    print("%-26s%16s%16s" % ("指标", "TCG", "KVM"))
    for name, sec, key in rows:
        print("%-26s%16s%16s" % (name, cell("TCG", sec, key), cell("KVM", sec, key)))

    out = os.path.join(HERE, "docs", "第五十七轮_进程模型判据报告.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n[report] %s" % out)
    allok = all(r.get("summary", {}).get("fail") == 0 for r in report["modes"].values())
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
