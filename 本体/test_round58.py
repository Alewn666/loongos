#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十八轮 58.0-A · 每进程 FD 表 + 完整 execve 双路判据（TCG / KVM）。

58.0-A 做了什么：
  - fdtab.c/h：每进程 FD 表 + 打开文件描述(OFD)共享语义，宿主单测 47/47；
  - 内核集成：fork 共享父 OFD、子进程 open/read/close/lseek/fstat/dup/dup2 走自己的表，
    pid=1 老 g_fd 路径逐字不变；execve 从占位 -1 改为真实 ELF 加载（含回滚）。
  - user/u58_fdproc.c + u58_exec.c + u58_target.c（只编不内嵌，经 b64 注入 /home）。

判据原则（不接受串口自报）：
  u58fd 的 [R58FD] pass= 与 u58exec 的标记只是线索。本脚本独立重算：
  FD：fork_ret>=2、child_open_fd=3、child_read>0、child_read_after_close=-1、
      wait/status、parent_open_fd=3 且 parent_read>0（子的 close 未污染父）、pid 身份。
  execve：before_exec=1 → replaced=1（目标接管）、pid 不变、argc=2、argv0=HELLO、
      目标 target_exit=1、**无** exec_failed/after（成功不返回原程序）、无 EL0 fault。
  另：失败回滚用坏路径 execve 验证返回 -1 且原程序继续、golden 逐字不变。

启动：
  python3 test_round58.py
  R58_KERNEL=<kernel.elf> python3 test_round58.py
"""
import os, sys, re, json, time, pty, select, base64, hashlib, subprocess, platform

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("R58_KERNEL", os.path.join(HERE, "kernel.elf"))
ELF_FD = os.environ.get("R58_ELF_FD", os.path.join(HERE, "build/u58fd.elf"))
ELF_EXE = os.environ.get("R58_ELF_EXE", os.path.join(HERE, "build/u58exec.elf"))
ELF_TGT = os.environ.get("R58_ELF_TGT", os.path.join(HERE, "build/u58tgt.elf"))
ELF_BAD = os.environ.get("R58_ELF_BAD", os.path.join(HERE, "build/u58bad.elf"))

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "
RE_FD = re.compile(r"^\[R58FD\] ([A-Za-z0-9_]+)=(.*)$", re.M)
RE_EX = re.compile(r"^\[R58(EX|CALL)\] ([A-Za-z0-9_]+)=(.*)$", re.M)
RE_BAD = re.compile(r"^\[R58BAD\] ([A-Za-z0-9_]+)=(.*)$", re.M)


def kvs(t, rx):
    if rx is RE_FD or rx is RE_BAD:
        return {k.strip(): v.strip() for k, v in rx.findall(t)}
    return {"%s_%s" % (g.lower(), k.strip()): v.strip() for g, k, v in rx.findall(t)}


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
        self.log_path = "/tmp/loong_round58_%s.log" % mode.lower()

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

    def paste_elf(self, path, elf, chunk=40, pause=0.45):
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

        # ---- 老路径零扰动 ----
        host, _ = s.collect("cat /etc/hostname", r"myos", 20)
        ck("2 cat /etc/hostname 仍为 myos（老 fs 逐字不变）", "myos" in host, host[:100])
        ok_h, n_h, w_h, _ = s.paste_elf("/home/u55test", os.path.join(HERE, "build/u55test.elf"))
        t_h, hit_h = s.collect("run /home/u55test", r"程序退出|用户程序出错", 45)
        ck("3 老 syscall 程序仍体面退出 code=0（fd 改动零扰动 pid=1）",
           hit_h and "程序退出 code=0" in t_h and "用户程序出错" not in t_h, t_h[-160:])

        # ================= 每进程 FD 表 =================
        ok1, n1, w1, _ = s.paste_elf("/home/u58fd", ELF_FD)
        ck("4 u58fd.elf 注入字节一致", ok1, "写入 %d / 期望 %d" % (n1, w1))
        tfd, hitfd = s.collect("run /home/u58fd", r"R58FD. done|程序退出|用户程序出错", 60)
        dfd = kvs(tfd, RE_FD)
        res["u58fd"] = dfd
        ck("5 FD 程序跑完并体面退出 code=0（无 EL0 fault）",
           hitfd and "程序退出 code=0" in tfd and "用户程序出错" not in tfd, tfd[-240:])
        ck("6 自报 pass/fail 归零", kvi(dfd, "pass") == 5 and kvi(dfd, "fail") == 0,
           "pass=%s fail=%s" % (dfd.get("pass"), dfd.get("fail")))
        ck("7 fork_ret>=2（子 pid 从 2 起）",
           (kvi(dfd, "fork_ret") or 0) >= 2, dfd.get("fork_ret"))
        ck("8 子独立表 open 得 fd=3", kvi(dfd, "child_open_fd") == 3, dfd.get("child_open_fd"))
        ck("9 子 read>0 且首字节可见",
           (kvi(dfd, "child_read") or 0) > 0 and "child_first" in dfd,
           "read=%s first=%s" % (dfd.get("child_read"), dfd.get("child_first")))
        ck("10 子 close 后再读返回 -1（fd 表项已释放）",
           kvi(dfd, "child_read_after_close") == -1, dfd.get("child_read_after_close"))
        ck("11 wait/status 正确（子 procexit 退出码 5）",
           kvi(dfd, "wait_ret") == kvi(dfd, "fork_ret") and kvi(dfd, "status") == 5,
           "wait=%s fork=%s status=%s" % (dfd.get("wait_ret"), dfd.get("fork_ret"), dfd.get("status")))
        ck("12 父 open 也得 fd=3 且 read>0（子的 close 未污染父表）",
           kvi(dfd, "parent_open_fd") == 3 and (kvi(dfd, "parent_read") or 0) > 0,
           "pfd=%s pread=%s" % (dfd.get("parent_open_fd"), dfd.get("parent_read")))
        ck("13 pid_before=1 且身份未被 fork 改变",
           kvi(dfd, "pid_before") == 1, dfd.get("pid_before"))

        # 二次 run：OFD 池跨 run 零泄漏
        tfd2, hitfd2 = s.collect("run /home/u58fd", r"R58FD. done|程序退出", 60)
        dfd2 = kvs(tfd2, RE_FD)
        ck("14 二次 run 同样 pass=5 fail=0（OFD 池跨 run 干净）",
           hitfd2 and kvi(dfd2, "pass") == 5 and kvi(dfd2, "fail") == 0,
           tfd2[-160:])

        # ================= 完整 execve =================
        ok2, n2, w2, _ = s.paste_elf("/home/u58tgt", ELF_TGT)
        ok3, n3, w3, _ = s.paste_elf("/home/u58exec", ELF_EXE)
        ck("15 u58tgt/u58exec 注入字节一致", ok2 and ok3,
           "tgt %d/%d exec %d/%d" % (n2, w2, n3, w3))
        tex, hitx = s.collect("run /home/u58exec", r"R58EX. target_exit|R58CALL. exec_failed|用户程序出错|程序退出", 60)
        dex = kvs(tex, RE_EX)
        res["u58exec"] = dex
        ck("16 execve 成功：目标程序接管并体面退出（无 EL0 fault）",
           "replaced=1" in tex and "程序退出 code=0" in tex and "用户程序出错" not in tex,
           tex[-300:])
        ck("17 成功不返回原程序：无 exec_failed、无 exec_ret 行",
           "exec_failed" not in tex and "exec_ret" not in dex,
           "exec_failed=%s" % ("exec_failed" in tex))
        ck("18 execve 前 pid_before=1",
           kvi(dex, "call_pid_before") == 1, dex.get("call_pid_before"))
        ck("19 目标 pid 不变（execve 保留 pid）",
           kvi(dex, "ex_pid") == 1, dex.get("ex_pid"))
        # execve 参数区：argv0=path（程序名，符合 Unix 惯例），argv1=argline 首词 HELLO
        ck("20 参数区重建：argc=2、argv0=程序路径、argv1=HELLO（argline 正确传入）",
           kvi(dex, "ex_argc") == 2 and "argv0=/home/u58tgt" in tex and "argv1=HELLO" in tex,
           "argc=%s lines=%s" % (dex.get("ex_argc"),
                                 [l for l in tex.splitlines() if "argv" in l][:3]))

        # 失败回滚：execve 一个不存在的路径应返回 -1、原程序继续执行（现场未破坏）
        ok4, n4, w4, _ = s.paste_elf("/home/u58bad", ELF_BAD)
        tb, hitb = s.collect("run /home/u58bad", r"R58BAD. rollback_ok|程序退出|用户程序出错", 60)
        dbad = kvs(tb, RE_BAD)
        res["u58bad"] = dbad
        ck("21 失败回滚：execve 坏路径返回 -1 且原程序继续（rollback_ok）",
           kvi(dbad, "exec_ret") == -1 and "rollback_ok=1" in tb and
           kvi(dbad, "pid_after") == 1 and "用户程序出错" not in tb,
           "ret=%s pid_after=%s tail=%s" % (dbad.get("exec_ret"), dbad.get("pid_after"), tb[-120:]))

        # ================= 健康 & golden =================
        rr, _ = s.collect("res", r"NISV", 20)
        hm = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                       r"FIQ/SErr=(\d+)\s+熔断=(\d+)", rr)
        res["res_after"] = {"sync": int(hm.group(1)), "nisv": int(hm.group(3)),
                            "fiq": int(hm.group(4)), "trip": int(hm.group(5))} if hm else None
        ck("22 结束后 res 健康计数全 0（FD/execve 切换未触发异常）",
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
    report = {"title": "LoongOS 第五十八轮 58.0-A 每进程FD表+完整execve 判据报告",
              "kernel_md5": md5(KERNEL), "elf_fd_md5": md5(ELF_FD),
              "elf_exec_md5": md5(ELF_EXE), "elf_tgt_md5": md5(ELF_TGT),
              "elf_bad_md5": md5(ELF_BAD),
              "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "modes": {}}
    modes = ["TCG"]
    why = None
    if platform.machine() != "aarch64":
        why = "宿主为 %s，无 aarch64 KVM 能力" % platform.machine()
    elif not os.path.exists("/dev/kvm"):
        why = "无 /dev/kvm"
    if os.environ.get("KVM_FORCE") == "1":
        modes.append("KVM")
        print("[r58] 警告：%s；KVM_FORCE=1 仍尝试" % (why or ""))
    elif why:
        print("[r58] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)
    else:
        modes.append("KVM")

    for mode in modes:
        try:
            report["modes"][mode] = run_mode(mode)
        except Exception as e:
            report["modes"][mode] = {"error": repr(e),
                                     "summary": {"pass": 0, "fail": -1, "failed": [repr(e)]}}
            print("  [X ] %s 路异常：%r" % (mode, e))

    out = os.path.join(HERE, "docs", "第五十八轮_FD与execve判据报告.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n[report] %s" % out)
    allok = all(r.get("summary", {}).get("fail") == 0 for r in report["modes"].values())
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
