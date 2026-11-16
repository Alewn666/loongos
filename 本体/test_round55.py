#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十五轮 · syscall 扩容 + 用户态管道 双路判据脚本（TCG / 真机 KVM）。

第五十五轮做了什么（演进路线 §六 第 55 轮）：
  - syscall 从 6 个扩到 20 个（6~19 号新增），0~5 号行为逐字不变；
  - 统一 fd / pipe 对象层 usysfd.c，pipe 环形缓冲用第五十四轮 kmalloc 动态分配；
  - 两个用户态 ELF：/home/u55test（逐项冒烟 19 号 syscall）、
    /home/u55sh（用真 pipe + dup2 装配 ls | grep | wc 全链路）。
  - 严格遵循文档 §七「不在 kernel.c 加 cmd_*」：全部能力走 syscall，
    用户程序经 b64 注入 /home，不内嵌内核镜像、不改启动日志、不碰 ls /bin。

为什么用 -kernel 直启而不是 ISO/UEFI：
  本轮判据的目标是 syscall 语义与用户态管道，直启 TCG/KVM 最快最干净；
  ISO 引导 / 装盘 / 图形桌面由全套 15 项回归单独覆盖（见 run_regression_r.sh）。

判据原则（文档 §八「不接受串口打印说成功」）：
  程序打印的 [R55] chk ... ok 只是「程序自报」，本脚本**独立重算关键数值**——
  pipe 往返的字符串、dup2 重定向读回的内容、grep 保留行、wc 字节数、
  getdents 目录项、注入字节数、mmstat 零泄漏——全部从串口原文正则核对。

启动：
  python3 test_round55.py                 # 自动：aarch64 + /dev/kvm 则双路，否则只 TCG
  KVM_FORCE=1 python3 test_round55.py     # 无 /dev/kvm 也强制尝试（会明确失败，不冒充）
  R55_KERNEL=<kernel.elf>                # 覆盖内核路径（真机 KVM 用）
"""
import os, sys, re, json, time, pty, select, base64, hashlib, subprocess, platform, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("R55_KERNEL", os.path.join(HERE, "kernel.elf"))
ELF_TEST = os.environ.get("R55_TEST", os.path.join(HERE, "build/u55test.elf"))
ELF_SH = os.environ.get("R55_SH", os.path.join(HERE, "build/u55sh.elf"))

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "

# ---------- 数值/断言解析（从串口原文独立取，不信任程序自报） ----------
RE_KV = re.compile(r"^\[R55\] ([A-Za-z0-9_]+)=(.*)$", re.M)
RE_CHK = re.compile(r"^\[R55\] chk (\S+) (ok|FAIL)$", re.M)
RE_RES = re.compile(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                    r"FIQ/SErr=(\d+)\s+熔断=(\d+)")
RE_PROMPT = re.compile(r"^>\s*$", re.M)

def kvs(t):
    return {k.strip(): v.strip() for k, v in RE_KV.findall(t)}

def kvi(d, k):
    try:
        return int(d.get(k, ""))
    except (ValueError, TypeError):
        return None

def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


class Session:
    """一路 QEMU（-nographic，串口走 pty 双向）。b64 注入需要逐行精确节拍，
    故用 pty + select + 小步 read_for，命令一律按结束标记驱动采集。"""

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
        self.log_path = "/tmp/loong_round55_%s.log" % mode.lower()

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

    def collect(self, line, end_rx, timeout=40):
        """发命令 → 等结束标记 → 再等新提示符，返回该段串口原文。"""
        m = self.mark()
        self.send(line)
        hit = self.wait(m, end_rx, timeout)
        if hit:
            self.wait(m, re.escape(PROMPT.strip()) + r"\s*$", 6)
            time.sleep(0.05)
            self.read_for(0.1)
        return self.seg(m), hit

    def paste_elf(self, path, elf, chunk=40, pause=0.5):
        """经 b64in 把 ELF 注入 /home；返回 (ok, 字节数, 该段原文)。"""
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
        return (n == len(data)), n, len(data), t

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            os.close(self.master)
        except Exception:
            pass
        try:
            with open(self.log_path, "w", encoding="utf-8") as f:
                f.write(self.seg(0))
        except Exception:
            pass


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
        # ---------- 0) 启动 ----------
        boot = s.wait(0, r"LoongOS stage5", 20)
        s.wait(0, re.escape(PROMPT.strip()), 20)
        ck("内核横幅 + shell 提示符就绪", boot, "未见横幅/提示符")

        # ---------- 1) b64 注入两个用户态 ELF ----------
        ok1, n1, w1, t1 = s.paste_elf("/home/u55test", ELF_TEST)
        ok2, n2, w2, t2 = s.paste_elf("/home/u55sh", ELF_SH)
        res["inject"] = {"u55test": {"bytes": n1, "expect": w1, "ok": ok1},
                         "u55sh": {"bytes": n2, "expect": w2, "ok": ok2}}
        ck("u55test.elf 经 b64 注入字节数一致", ok1, "写入 %d / 期望 %d" % (n1, w1))
        ck("u55sh.elf 经 b64 注入字节数一致", ok2, "写入 %d / 期望 %d" % (n2, w2))
        ls_home, _ = s.collect("ls /home", r"u55sh|readme", 20)
        ck("ls /home 含新程序且原 readme.txt 仍在",
           "u55test" in ls_home and "u55sh" in ls_home and "readme" in ls_home,
           ls_home[:160])

        # ---------- 2) run /home/u55test：19 号 syscall 逐项 ----------
        t, hit = s.collect("run /home/u55test", r"u55test done", 45)
        d = kvs(t)
        chks = RE_CHK.findall(t)
        nfail = sum(1 for _, v in chks if v == "FAIL")
        res["u55test"] = {"pass_self": kvi(d, "PASS"), "fail_self": kvi(d, "FAIL"),
                          "chk_count": len(chks), "key_values": d}
        ck("u55test 结束标志 + 体面退出 code=0",
           hit and "程序退出 code=0" in t and "已从 EL0 撤回内核" in t,
           t[-160:])
        ck("u55test 自报 PASS/FAIL 且 chk 项 >= 30 条、零 FAIL 行",
           kvi(d, "FAIL") == 0 and (kvi(d, "PASS") or 0) >= 30 and
           len(chks) >= 30 and nfail == 0,
           "PASS=%s FAIL=%s chk=%d nfail=%d" % (d.get("PASS"), d.get("FAIL"), len(chks), nfail))

        # 独立数值核对（不靠 chk ok）
        ck("getcwd=/ 且 chdir(/etc) 后=/etc、坏目录被拒",
           d.get("getcwd_val") == "/" and kvi(d, "getcwd_rc") == 1 and
           d.get("getcwd_after_chdir") == "/etc" and kvi(d, "chdir_bad_rc") == -1,
           {k: d.get(k) for k in ("getcwd_val", "getcwd_after_chdir", "chdir_bad_rc")})
        ck("getpid=1（单进程模型）", kvi(d, "getpid") == 1, d.get("getpid"))
        ck("stat /etc/hostname size=5 type=2 uid=0 + 坏路径/坏内核指针被拒",
           kvi(d, "stat_size") == 5 and kvi(d, "stat_type") == 2 and
           kvi(d, "stat_uid") == 0 and kvi(d, "stat_bad_rc") == -1 and
           kvi(d, "stat_badptr_rc") == -1 and kvi(d, "stat_relative_rc") == 0,
           {k: d.get(k) for k in ("stat_size", "stat_type", "stat_badptr_rc", "stat_relative_rc")})
        gd = d.get("getdents_etc_val", "")
        ck("getdents /etc 真实列出 hostname/passwd/shadow",
           kvi(d, "getdents_etc_rc") == 28 and
           all(x in t for x in ("hostname", "passwd", "shadow")) and
           kvi(d, "getdents_bad_rc") == -1,
           "rc=%s" % d.get("getdents_etc_rc"))
        ck("mkdir/creat/rename/unlink 语义（旧名消失新名到位 + 重复/二次删被拒）",
           (kvi(d, "mkdir_rc") == 0 and kvi(d, "mkdir_stat_type") == 1 and
            kvi(d, "mkdir_dup_rc") == -1 and kvi(d, "creat_rc") == 0 and
            kvi(d, "creat_stat_type") == 2 and kvi(d, "rename_rc") == 0 and
            kvi(d, "rename_old_gone") == 1 and kvi(d, "rename_new_here") == 1 and
            kvi(d, "unlink_rc") == 0 and kvi(d, "unlink_again_rc") == -1 and
            kvi(d, "unlink_dir_rc") == 0),
           {k: d.get(k) for k in ("rename_old_gone", "rename_new_here", "unlink_dir_rc")})
        ck("0~5 号老 syscall 回归：open fd=3 / read 'myos' / lseek / close / fstat",
           kvi(d, "open_fd") == 3 and kvi(d, "read_n") == 5 and
           d.get("read_val") == "myos" and kvi(d, "lseek_set0") == 0 and
           kvi(d, "close_rc") == 0 and kvi(d, "fstat_size") == 5 and
           kvi(d, "fstat_bad_rc") == -1,
           {k: d.get(k) for k in ("open_fd", "read_val", "fstat_bad_rc")})
        ck("pipe 往返字符串 + dup 共享 + dup2 stdout 重定向读回 REDIR",
           kvi(d, "pipe_rc") == 0 and kvi(d, "pipe_rfd") == 6 and
           kvi(d, "pipe_wfd") == 7 and kvi(d, "pipe_write_n") == 14 and
           d.get("pipe_read_val") == "PIPE-ROUNDTRIP" and
           kvi(d, "dup_fd") == 8 and kvi(d, "dup2_rc") == 1 and
           d.get("dup2_read_val") == "REDIR",
           {k: d.get(k) for k in ("pipe_rfd", "pipe_read_val", "dup2_read_val")})
        ck("kill 语义（pid!=1 拒 / pid==1 TERM 回 0 不真杀）",
           kvi(d, "kill_badpid_rc") == -1 and kvi(d, "kill_self_rc") == 0,
           {k: d.get(k) for k in ("kill_badpid_rc", "kill_self_rc")})

        # ---------- 3) run /home/u55sh hostname：ls|grep|wc 全链路 ----------
        t2, hit2 = s.collect("run /home/u55sh hostname", r"u55sh pipeline done", 45)
        d2 = kvs(t2)
        chks2 = RE_CHK.findall(t2)
        res["u55sh"] = {"pass_self": kvi(d2, "PASS"), "fail_self": kvi(d2, "FAIL"),
                        "key_values": d2}
        ck("u55sh 结束标志 + 体面退出 + 零 FAIL 行",
           hit2 and "程序退出 code=0" in t2 and
           kvi(d2, "FAIL") == 0 and (kvi(d2, "PASS") or 0) >= 7 and
           all(v == "ok" for _, v in chks2),
           "PASS=%s FAIL=%s chk=%d" % (d2.get("PASS"), d2.get("FAIL"), len(chks2)))
        ck("stage1 ls：getdents 字节数与 u55test 对账（28B / 5 行）",
           kvi(d2, "ls_bytes") == kvi(d, "getdents_etc_rc") == 28 and
           kvi(d2, "ls_lines") == 5 and kvi(d2, "grep_read_n") == 28,
           {k: d2.get(k) for k in ("ls_bytes", "ls_lines", "grep_read_n")})
        ck("两个独立真 pipe（4 个 fd 各不相同且 >= 6）",
           len({kvi(d2, "pipeA_rfd"), kvi(d2, "pipeA_wfd"),
                kvi(d2, "pipeB_rfd"), kvi(d2, "pipeB_wfd")}) == 4 and
           min(kvi(d2, "pipeA_rfd"), kvi(d2, "pipeB_wfd")) >= 6,
           {k: d2.get(k) for k in ("pipeA_rfd", "pipeA_wfd", "pipeB_rfd", "pipeB_wfd")})
        ck("stage2 grep：读回 5 行、按关键词保留 1 行 'hostname'、9 字节入 pipeB",
           kvi(d2, "grep_src_lines") == 5 and kvi(d2, "grep_kept_lines") == 1 and
           kvi(d2, "grep_bytes") == 9 and kvi(d2, "grep_write_n") == 9 and
           "grep_result=hostname" in t2,
           {k: d2.get(k) for k in ("grep_src_lines", "grep_kept_lines", "grep_bytes")})
        ck("stage3 wc：从 pipeB 读回 9B，行/词/字节与 grep 输出自洽",
           kvi(d2, "wc_read_n") == 9 and kvi(d2, "wc_lines") == 1 and
           kvi(d2, "wc_words") == 1 and kvi(d2, "wc_bytes") == 9 and
           kvi(d2, "wc_bytes") == kvi(d2, "grep_bytes") and
           "[SH] wc: lines=1 words=1 bytes=9" in t2,
           {k: d2.get(k) for k in ("wc_read_n", "wc_lines", "wc_bytes")})

        # ---------- 4) 换关键词复跑，证明 grep 不是写死 ----------
        t3, hit3 = s.collect("run /home/u55sh passwd", r"u55sh pipeline done", 45)
        d3 = kvs(t3)
        res["u55sh_passwd"] = {"key_values": d3}
        ck("换关键词 passwd：grep 结果随之变化且不含 hostname（非恒真）",
           hit3 and kvi(d3, "FAIL") == 0 and
           kvi(d3, "grep_kept_lines") == 1 and kvi(d3, "grep_bytes") == 7 and
           "grep_result=passwd" in t3 and "grep_result=hostname" not in t3,
           {k: d3.get(k) for k in ("grep_kept_lines", "grep_bytes")})

        # ---------- 5) 跨 run 零泄漏 + 复位有效性：连跑 3 次 ----------
        seq = []
        for i in range(3):
            ti, hi = s.collect("run /home/u55sh hostname", r"u55sh pipeline done", 45)
            di = kvs(ti)
            seq.append(kvi(di, "FAIL") == 0 and hi and "程序退出 code=0" in ti)
        mm, _ = s.collect("mmstat", r"\[mm \] audit\s*=", 20)
        maud = re.search(r"\[mm \] audit\s*=\s*(\d+)", mm)
        mlive = re.search(r"live=(\d+)\s+used_bytes=(\d+)", mm)
        mcnt = re.search(r"alloc=(\d+)\s+free=(\d+)\s+fail=(\d+)\s+badfree=(\d+)", mm)
        audit = int(maud.group(1)) if maud else -1
        live = int(mlive.group(1)) if mlive else -1
        ub = int(mlive.group(2)) if mlive else -1
        ac, fc, ec, bfc = (tuple(int(x) for x in mcnt.groups()) if mcnt
                           else (-1, -1, -1, -1))
        res["repeat"] = {"runs": 3, "all_ok": all(seq), "mmstat_audit": audit,
                         "live": live, "used_bytes": ub, "alloc": ac, "free": fc,
                         "fail": ec, "badfree": bfc}
        # 每次 u55sh 用 2 个 pipe，各 kmalloc 1 个 1024B ring；连跑多次后 alloc
        # 累计数应 >= 3，但 live/used_bytes 必须归零、alloc==free —— 这是「ring
        # 真的走了第54轮动态堆、且 close/ufd_reset 零泄漏」的量化证据。
        ck("连跑 3 次全通过 + mmstat 零泄漏（audit=0 live=0 used=0 alloc==free fail=0 badfree=0）",
           all(seq) and audit == 0 and live == 0 and ub == 0 and
           ac == fc and ac >= 3 and ec == 0 and bfc == 0,
           "seq=%s audit=%s live=%s used=%s alloc=%s free=%s fail=%s badfree=%s" %
           (seq, audit, live, ub, ac, fc, ec, bfc))

        # ---------- 6) 行为基线不扰动 ----------
        stat_t, _ = s.collect("stat /etc/hostname", r"mtime:", 20)
        ck("stat /etc/hostname 仍逐字输出 mtime: 0 秒（54 轮基线不动）",
           "mtime: 0 秒" in stat_t, stat_t[:160])
        lsbin, _ = s.collect("ls /bin", r"hang", 20)
        ck("ls /bin 仍 5 个老程序（用户 ELF 不内嵌镜像、不碰 /bin）",
           all(x in lsbin for x in ("hello", "cat", "args", "bad", "hang")) and
           "u55" not in lsbin, lsbin[:160])
        hello, hh = s.collect("run /bin/hello", r"程序退出 code=0", 30)
        ck("run /bin/hello 老 syscall 0/1 逐字不变（你好 + exit 0）",
           hh and "你好！我是 /bin/hello" in hello, hello[:160])

        # ---------- 7) 结束健康（用第 54 轮同款正则做真实健康判据）----------
        rr, rh = s.collect("res", r"健康: 同步异常=", 20)
        mr = RE_RES.search(rr)
        health = [int(x) for x in mr.groups()] if mr else [-1]
        res["res_health"] = health
        ck("结束后 res 健康全 0（同步异常/自愈/NISV/FIQ/熔断）",
           bool(mr) and all(v == 0 for v in health) and rh,
           "health=%s raw=%s" % (health, rr[:140]))

    finally:
        s.close()

    npass = sum(1 for c in checks if c["ok"])
    res["checks"] = checks
    res["summary"] = {"pass": npass, "fail": len(checks) - npass,
                      "failed": [c["name"] for c in checks if not c["ok"]]}
    print("\n  [%s] 判据结果：%d 过 / %d 挂" % (mode, npass, len(checks) - npass))
    return res


def main():
    print("LoongOS 第五十五轮判据 · kernel=%s" % KERNEL)
    for p in (KERNEL, ELF_TEST, ELF_SH):
        if not os.path.exists(p):
            print("缺少产物：", p)
            return 2
    report = {"title": "LoongOS 第五十五轮 · syscall 扩容 + 用户态管道判据报告",
              "kernel_md5": md5(KERNEL),
              "elftest_md5": md5(ELF_TEST), "elftest_size": os.path.getsize(ELF_TEST),
              "elfsh_md5": md5(ELF_SH), "elfsh_size": os.path.getsize(ELF_SH),
              "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
              "modes": {}}
    modes = ["TCG"]
    # 双路：只有 aarch64 宿主 + /dev/kvm 才跑 KVM；本地 x86_64 如实只跑 TCG
    why = None
    if platform.machine() != "aarch64":
        why = "宿主为 %s，无 aarch64 KVM 能力" % platform.machine()
    elif not os.path.exists("/dev/kvm"):
        why = "无 /dev/kvm"
    if os.environ.get("KVM_FORCE") == "1":
        modes.append("KVM")
        print("[r55] 警告：%s；KVM_FORCE=1 仍尝试（可能失败）" % (why or ""))
    elif why:
        print("[r55] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)
    else:
        modes.append("KVM")

    for mode in modes:
        try:
            report["modes"][mode] = run_mode(mode)
        except Exception as e:
            report["modes"][mode] = {"error": repr(e),
                                     "summary": {"pass": 0, "fail": -1, "failed": [repr(e)]}}
            print("  [X ] %s 路异常：%r" % (mode, e))

    # 对照表
    print("\n" + "=" * 70)
    print("  TCG vs KVM 第 55 轮对照表（同一 kernel md5 %s…）" % report["kernel_md5"][:12])
    print("=" * 70)
    rows = [
        ("u55test 自报 PASS", "u55test", "pass_self"),
        ("u55test 独立 chk 条数", "u55test", "chk_count"),
        ("u55sh 自报 PASS", "u55sh", "pass_self"),
        ("ls 目录项字节", "u55sh", "key_values.ls_bytes"),
        ("grep 保留行", "u55sh", "key_values.grep_kept_lines"),
        ("wc 字节", "u55sh", "key_values.wc_bytes"),
        ("换关键词 kept", "u55sh_passwd", "key_values.grep_kept_lines"),
        ("连跑3次全通过", "repeat", "all_ok"),
        ("mmstat audit", "repeat", "mmstat_audit"),
    ]
    def cell(mode, sec, key):
        r = report["modes"].get(mode)
        if not r or "summary" not in r or r.get("summary", {}).get("fail") == -1:
            return "—"
        v = r.get(sec, {})
        for part in key.split("."):
            if isinstance(v, dict):
                v = v.get(part)
            else:
                return "—"
        if v is None:
            return "—"
        if isinstance(v, bool):
            return "OK" if v else "NO"
        return str(v)[:14]
    print("%-30s%16s%16s" % ("指标", "TCG", "KVM"))
    for label, sec, key in rows:
        print("%-30s%16s%16s" % (label, cell("TCG", sec, key), cell("KVM", sec, key)))
    print("注：KVM 列「—」= 该路未执行，不是 0，也不是通过。")

    report["nas_kvm"] = {
        "dual_path_completed": "KVM" in report["modes"] and
                               report["modes"]["KVM"].get("summary", {}).get("fail") == 0,
        "skip_reason": why or "",
        "note": "本地沙箱为 x86_64 无 aarch64 KVM；真机 KVM 双路需 NAS 通道恢复后补跑"}
    with open("/tmp/loong_round55_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n[r55] 报告：/tmp/loong_round55_report.json")

    bad = any(v.get("summary", {}).get("fail", 0) != 0 for v in report["modes"].values())
    if "KVM" not in report["modes"]:
        print("[r55] 提醒：KVM 路未执行，本次不构成「TCG+KVM 双路复验完成」。")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
