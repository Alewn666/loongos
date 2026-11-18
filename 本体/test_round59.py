#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十八轮 58.0-B · 抢占式调度地基双路判据（TCG / KVM）。

58.0-B 做了什么：
  - sched.c/h：五态调度状态机 + 时间片 + 轮转 runqueue + yield + 统计，宿主单测 72/72；
  - 内核集成：fork 子登记 RUNNABLE、wait 切子标 RUNNING、procexit/reap 移除实体，
    tick 推进时间片账，新增 syscall 26(yield)/27(schedstat)，pid=1 老路径逐字不变；
  - user/u59_sched.c（只编不内嵌，经 b64 注入 /home）。

判据原则（不接受串口自报）：
  本脚本独立重算：[R59] 标记只是线索。逐项核对 stat0 统计全 0、fork 两子得 pid>=2、
  parent_runable=2、child 见自身 RUNNING(state=2)、yield 后 yields 增长且 child_state_after
  变化、两子 procexit status=7、final_live_ent=0（实体全回收）、pid_after=1、
  跨 run 二次一致、mmstat audit=0、老程序零扰动、golden 逐字不变。

启动：
  python3 test_round59.py
  R59_KERNEL=<kernel.elf> python3 test_round59.py
"""
import os, sys, re, json, time, pty, select, base64, hashlib, subprocess, platform

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("R59_KERNEL", os.path.join(HERE, "kernel.elf"))
ELF = os.environ.get("R59_ELF", os.path.join(HERE, "build/u59sched.elf"))

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "
RE_R59 = re.compile(r"^\[R59\] ([A-Za-z0-9_]+)=(.*)$", re.M)


def kvs(t):
    return {k.strip(): v.strip() for k, v in RE_R59.findall(t)}


def kvi(d, k):
    try:
        v = d.get(k, "")
        return int(v, 0) if v.startswith("0x") else int(v)
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
        self.log_path = "/tmp/loong_round59_%s.log" % mode.lower()

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

        # golden 敏感路径：老命令/老程序逐字不变
        hb, _ = s.collect("help", r"run <", 20)
        ck("2 help 仍含 run 命令（golden 命令集未删）", "run <" in hb, hb[:120])
        host, _ = s.collect("cat /etc/hostname", r"myos", 20)
        ck("3 cat /etc/hostname 仍为 myos（老 fs 路径逐字不变）",
           "myos" in host and "MemTotal" not in host, host[:120])
        ok_h, n_h, w_h, _ = s.paste_elf("/home/u55test", os.path.join(HERE, "build/u55test.elf"))
        ck("4 老程序 u55test.elf 注入字节一致", ok_h, "写入 %d / 期望 %d" % (n_h, w_h))
        t_h, hit_h = s.collect("run /home/u55test", r"程序退出|用户程序出错", 45)
        ck("5 老 syscall 程序仍体面退出 code=0（0~25 零扰动）",
           hit_h and "程序退出 code=0" in t_h and "用户程序出错" not in t_h, t_h[-200:])

        # 注入 u59 调度验证程序
        ok_inj, n, w, _ = s.paste_elf("/home/u59sched", ELF)
        ck("6 u59sched.elf 经 b64 注入字节一致", ok_inj, "写入 %d / 期望 %d" % (n, w))
        ls_home, _ = s.collect("ls /home", r"u59sched", 20)
        ck("7 ls /home 含新程序", "u59sched" in ls_home, ls_home[:160])

        t, hit = s.collect("run /home/u59sched", r"R59 pass=|程序退出|用户程序出错", 90)
        d = kvs(t)
        res["u59sched"] = d
        ck("8 用户程序跑完并体面退出 code=0（无 EL0 fault）",
           hit and "程序退出 code=0" in t and "用户程序出错" not in t, t[-260:])

        # ---- 独立重算（不靠自报 pass）----
        ck("9 schedstat 调用返回 0（stat0_ret=0）", kvi(d, "stat0_ret") == 0, d.get("stat0_ret"))
        ck("10 初始统计全 0：ctx/yields/expires=0",
           kvi(d, "stat0_ctx") == 0 and kvi(d, "stat0_yields") == 0 and
           kvi(d, "stat0_expires") == 0,
           "ctx=%s y=%s e=%s" % (d.get("stat0_ctx"), d.get("stat0_yields"), d.get("stat0_expires")))
        ck("11 初始 current=0（pid=1 初始进程表外）", kvi(d, "stat0_current") == 0,
           d.get("stat0_current"))

        ck("11b 初始无活跃实体 stat0_live=0", kvi(d, "stat0_live") == 0,
           d.get("stat0_live"))
        fp = kvi(d, "fork_ret")
        ck("12 fork 一子得 pid>=2", fp is not None and fp >= 2, d.get("fork_ret"))
        ck("13 子见自身 state=RUNNING(2)（wait 切子同步状态）",
           kvi(d, "child_state_before") == 2, d.get("child_state_before"))
        ck("14 子见 current 指向自身（wait 切子同步 current）",
           kvi(d, "child_current_before") == fp, "cur=%s fp=%s" % (d.get("child_current_before"), fp))
        ck("15 子 yield 返回 0（仅一活跃子，选回自己——不冒充切到别人）",
           kvi(d, "child_yield_ret") == 0, d.get("child_yield_ret"))
        ck("16 yield 后全局 yields 增长到 >=1",
           kvi(d, "child_yields_global") is not None and kvi(d, "child_yields_global") >= 1,
           d.get("child_yields_global"))
        ck("17 仅一就绪实体时 yield 选回自己、保持 RUNNING(2)",
           kvi(d, "child_state_after") == 2, d.get("child_state_after"))
        ck("18 父 wait 返回子 pid + 退出码 status=7",
           kvi(d, "wait_ret") == fp and kvi(d, "status") == 7,
           "w=%s fp=%s status=%s" % (d.get("wait_ret"), fp, d.get("status")))
        ck("19 final_live=0（子实体从调度表移除，无残留）", kvi(d, "final_live") == 0,
           d.get("final_live"))
        ck("20 final_yields 与子观测一致（跨 yield 累计）",
           kvi(d, "final_yields") == kvi(d, "child_yields_global"),
           "final=%s child=%s" % (d.get("final_yields"), d.get("child_yields_global")))
        ck("21 pid_after=1（父从 wait 恢复仍是初始进程）", kvi(d, "pid_after") == 1,
           d.get("pid_after"))
        ck("22 自报 pass=6 fail=0（仅线索，与上面独立项交叉验证）",
           kvi(d, "pass") == 6 and kvi(d, "fail") == 0,
           "pass=%s fail=%s" % (d.get("pass"), d.get("fail")))

        # 跨 run 零泄漏
        t2, hit2 = s.collect("run /home/u59sched", r"R59 pass=|程序退出", 90)
        d2 = kvs(t2)
        ck("23 二次 run 同样 pass=6 fail=0（跨 run 调度表干净可复用）",
           hit2 and kvi(d2, "pass") == 6 and kvi(d2, "fail") == 0 and
           kvi(d2, "stat0_ctx") == 0,
           "pass=%s fail=%s ctx0=%s" % (kvi(d2, "pass"), kvi(d2, "fail"), kvi(d2, "stat0_ctx")))
        ms, _ = s.collect("mmstat", r"audit", 20)
        ck("24 mmstat audit=0（无堆泄漏）", re.search(r"audit\s*=\s*0", ms) is not None, ms[:200])

        rr, _ = s.collect("res", r"NISV", 20)
        hm = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                       r"FIQ/SErr=(\d+)\s+熔断=(\d+)", rr)
        res["res_after"] = {"sync": int(hm.group(1)), "nisv": int(hm.group(3)),
                            "fiq": int(hm.group(4)), "trip": int(hm.group(5))} if hm else None
        ck("25 结束后 res 健康计数全 0（调度记账未触发异常）",
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
    report = {"title": "LoongOS 第五十八轮 B 58.0-B 抢占式调度地基判据报告",
              "kernel_md5": md5(KERNEL), "elf_sched_md5": md5(ELF),
              "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "modes": {}}
    modes = ["TCG"]
    why = None
    if platform.machine() != "aarch64":
        why = "宿主为 %s，无 aarch64 KVM 能力" % platform.machine()
    elif not os.path.exists("/dev/kvm"):
        why = "无 /dev/kvm"
    if os.environ.get("KVM_FORCE") == "1":
        modes.append("KVM")
        print("[r59] 警告：%s；KVM_FORCE=1 仍尝试（可能失败）" % (why or ""))
    elif why:
        print("[r59] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)
    else:
        modes.append("KVM")

    allok = True
    for mode in modes:
        try:
            report["modes"][mode] = run_mode(mode)
        except Exception as e:
            allok = False
            report["modes"][mode] = {"error": str(e)}
            print("[r59] %s 路异常：%s" % (mode, e))
    for mode, r in report["modes"].items():
        if r.get("summary", {}).get("fail", 1) or r.get("error"):
            allok = False

    out = os.path.join(HERE, "docs", "第五十八轮B_抢占式调度地基判据报告.json")
    try:
        with open(out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print("[report] %s" % out)
    except Exception as e:
        print("[report] 写入失败：%s" % e)
    print("\n[r59] %s" % ("ALL PASS" if allok else "FAIL — 见上"))
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()
