#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十六轮 · VFS + /dev + /proc 双路判据脚本（TCG / 真机 KVM）。

第五十六轮做了什么（演进路线 §六 第 56 轮 / §3.1）：
  - 新增 vfs.c/h：挂载点 → vnode_ops 抽象，挂载 /proc /dev；
  - 老路径优先：fs_lookup 命中即走原逻辑，29 条 golden 命令逐字不变；
  - procfs：/proc/meminfo（真实 mm_stat）、uptime、version、cpuinfo、mounts；
  - devfs：/dev/null /dev/zero /dev/console /dev/tty；
  - 用户态 u56vfs.elf 经 b64 注入，走 SYS_vfsread/SYS_vfsinfo 读伪文件。

判据原则（文档 §八「不接受串口打印说成功」）：
  程序打印的 [R56] pass= 只是自报，本脚本独立重算——meminfo 数值、devfs
  读写、负例返回值、老路径内容、内核 shell 的 cat/ls 伪文件输出，全从串口
  原文正则核对；并与 golden_behavior.txt 的 cat /nope、cat /etc/hostname 对照。

启动：
  python3 test_round56.py                 # 本地 x86_64 只跑 TCG（如实标注）
  R56_KERNEL=<kernel.elf> python3 test_round56.py
"""
import os, sys, re, json, time, pty, select, base64, hashlib, subprocess, platform

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.environ.get("R56_KERNEL", os.path.join(HERE, "kernel.elf"))
ELF_VFS = os.environ.get("R56_VFS", os.path.join(HERE, "build/u56vfs.elf"))

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "
RE_KV = re.compile(r"^\[R56\] ([A-Za-z0-9_]+)=(.*)$", re.M)


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
        self.log_path = "/tmp/loong_round56_%s.log" % mode.lower()

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

        # 内核 shell：VFS 挂载点 cat/ls（独立核对，不靠用户程序）
        mem, _ = s.collect("cat /proc/meminfo", r"KernelStack:", 20)
        ck("2 cat /proc/meminfo 走 VFS（MemTotal/Free/kB + 真实数值）",
           "MemTotal:" in mem and "MemFree:" in mem and "kB" in mem and
           re.search(r"MemTotal:\s+\d+", mem) is not None, mem[:160])
        mt = re.search(r"MemTotal:\s+(\d+)", mem)
        mf = re.search(r"MemFree:\s+(\d+)", mem)
        res["meminfo"] = {"total_kb": int(mt.group(1)) if mt else -1,
                          "free_kb": int(mf.group(1)) if mf else -1}
        ck("3 meminfo 数值自洽（0<=Free<=Total, Total>0）",
           mt and mf and 0 <= int(mf.group(1)) <= int(mt.group(1)) and
           int(mt.group(1)) > 0, res["meminfo"])

        ver, _ = s.collect("cat /proc/version", r"LoongOS", 20)
        ck("4 cat /proc/version 返回版本串", "56.0-vfs" in ver, ver[:120])
        lsp, _ = s.collect("ls /proc", r"mounts", 20)
        ck("5 ls /proc 列出 5 个伪文件",
           all(k in lsp for k in ("meminfo", "uptime", "version", "cpuinfo", "mounts")),
           lsp[:200])
        lsd, _ = s.collect("ls /dev", r"tty", 20)
        ck("6 ls /dev 列出 4 个节点",
           all(k in lsd for k in ("null", "zero", "console", "tty")), lsd[:160])
        mounts, _ = s.collect("cat /proc/mounts", r"devfs", 20)
        ck("7 cat /proc/mounts 列出 /proc 与 /dev 真实挂载",
           "/proc" in mounts and "/dev" in mounts and "procfs" in mounts and "devfs" in mounts,
           mounts[:160])
        # 老路径逐字不变（golden 对照）
        nope, _ = s.collect("cat /nope", r"没有这个路径", 20)
        ck("8 cat /nope 仍报「没有这个路径」（未挂载路径不受 VFS 影响）",
           "没有这个路径: /nope" in nope, nope[:120])
        host, _ = s.collect("cat /etc/hostname", r"myos", 20)
        ck("9 cat /etc/hostname 仍为 myos（老 fs 路径逐字不变）",
           "myos" in host and "MemTotal" not in host, host[:120])

        # 用户态程序注入 + run
        ok_inj, n, w, _ = s.paste_elf("/home/u56vfs", ELF_VFS)
        ck("10 u56vfs.elf 经 b64 注入字节数一致", ok_inj, "写入 %d / 期望 %d" % (n, w))
        ls_home, _ = s.collect("ls /home", r"u56vfs|readme", 20)
        ck("11 ls /home 含新程序且 readme 仍在",
           "u56vfs" in ls_home and "readme" in ls_home, ls_home[:160])

        t, hit = s.collect("run /home/u56vfs", r"R56. done|程序退出", 45)
        d = kvs(t)
        res["u56vfs"] = d
        ck("12 用户程序跑完并体面退出 code=0（无 EL0 fault）",
           hit and "程序退出 code=0" in t and "用户程序出错" not in t, t[-200:])
        ck("13 自报 pass>=12 且 fail=0",
           (kvi(d, "pass") or 0) >= 12 and kvi(d, "fail") == 0,
           "pass=%s fail=%s" % (d.get("pass"), d.get("fail")))
        # 独立重算关键数值
        ck("14 vfsinfo=2（开机挂载 /proc /dev）", kvi(d, "vfsinfo") == 2, d.get("vfsinfo"))
        ck("15 meminfo_rc>0 且 total/free 与内核 shell 对账一致",
           (kvi(d, "meminfo_rc") or 0) > 0 and
           kvi(d, "meminfo_total_kb") == res["meminfo"]["total_kb"] and
           kvi(d, "meminfo_free_kb") == res["meminfo"]["free_kb"],
           "elf total=%s/%s shell total=%s" %
           (d.get("meminfo_total_kb"), d.get("meminfo_free_kb"), res["meminfo"]))
        ck("16 devfs zero 读满 32B / null 读 0B",
           kvi(d, "zero_rc") == 32 and kvi(d, "null_rc") == 0,
           "zero=%s null=%s" % (d.get("zero_rc"), d.get("null_rc")))
        ck("17 负例 /proc/nope 与未挂载 /etc 均返回 -1",
           kvi(d, "proc_nope_rc") == -1 and kvi(d, "unmounted_rc") == -1,
           "nope=%s etc=%s" % (d.get("proc_nope_rc"), d.get("unmounted_rc")))
        ck("18 老路径 open/read /etc/hostname 仍为 myos",
           (kvi(d, "old_open_fd") or 0) >= 3 and d.get("old_hostname", "").strip() == "myos",
           "fd=%s host=%s" % (d.get("old_open_fd"), d.get("old_hostname")))

        # 结束后健康
        rr, _ = s.collect("res", r"NISV", 20)
        hm = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                       r"FIQ/SErr=(\d+)\s+熔断=(\d+)", rr)
        res["res_after"] = {"sync": int(hm.group(1)), "nisv": int(hm.group(3)),
                            "fiq": int(hm.group(4)), "trip": int(hm.group(5))} if hm else None
        ck("19 结束后 res 健康计数全 0",
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
    report = {"title": "LoongOS 第五十六轮 VFS 判据报告",
              "kernel_md5": md5(KERNEL), "elf_vfs_md5": md5(ELF_VFS),
              "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "modes": {}}
    modes = ["TCG"]
    why = None
    if platform.machine() != "aarch64":
        why = "宿主为 %s，无 aarch64 KVM 能力" % platform.machine()
    elif not os.path.exists("/dev/kvm"):
        why = "无 /dev/kvm"
    if os.environ.get("KVM_FORCE") == "1":
        modes.append("KVM")
        print("[r56] 警告：%s；KVM_FORCE=1 仍尝试（可能失败）" % (why or ""))
    elif why:
        print("[r56] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)
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
    print("  TCG vs KVM 第 56 轮对照表（同一 kernel md5 %s…）" % report["kernel_md5"][:12])
    print("=" * 70)
    rows = [("meminfo total KB", "meminfo", "total_kb"),
            ("meminfo free KB", "meminfo", "free_kb"),
            ("elf meminfo total", "u56vfs", "meminfo_total_kb"),
            ("vfsinfo", "u56vfs", "vfsinfo"),
            ("dev zero rc", "u56vfs", "zero_rc"),
            ("dev null rc", "u56vfs", "null_rc"),
            ("自报 pass", "u56vfs", "pass"),
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
    for label, sec, key in rows:
        print("%-26s%16s%16s" % (label, cell("TCG", sec, key), cell("KVM", sec, key)))
    print("注：KVM 列「—」= 该路未执行，不是 0，也不是通过。")

    report["nas_kvm"] = {
        "dual_path_completed": "KVM" in report["modes"] and
                               report["modes"]["KVM"].get("summary", {}).get("fail") == 0,
        "skip_reason": why or "",
        "note": "本地沙箱 x86_64 无 aarch64 KVM；真机 KVM 双路需 NAS 通道恢复后补跑"}
    with open("/tmp/loong_round56_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n[r56] 报告：/tmp/loong_round56_report.json")

    bad = any(v.get("summary", {}).get("fail", 0) != 0 for v in report["modes"].values())
    if "KVM" not in report["modes"]:
        print("[r56] 提醒：KVM 路未执行，本次不构成「TCG+KVM 双路复验完成」。")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
