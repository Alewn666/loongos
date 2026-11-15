#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十四轮 · 地基能力双路判据脚本（TCG / 真机 KVM）。

背景
----
用户附件《LoongOS 演进路线：对齐 Linux/BSD》优先级 0 点名三块地基，其中
2.1（物理页分配器 + kmalloc）、2.2（真实墙钟 RTC）本轮真实落地；另把优先级
2 里「落地即可」的 sysctl/uptime/free/version、优先级 3.3 的常量时间口令比较、
以及 `ls -l` 的真实时间戳一并带上。

本脚本只做「判据」，不负责改代码，也不接受「串口打印说成功」：
  * 每条命令用**结束标记驱动**采集（第五十一/五十二轮 IO 教训），不用固定窗口；
  * 数值型结论必须可解析、可交叉对账（date epoch 对宿主、sysctl 对 mmstat、
    mmtest 前后水位、uptime 墙钟对 date epoch）；
  * 反向判据同样齐全：未知 sysctl oid 必须报 unknown、错误口令必须被拒、
    `stat` 的 mtime 仍须是基线里的「0 秒」、`help` 不得出现新命令。

判据（22 项 × 双路）
--------------------
  1  ISO 引导 / 内核横幅       12 sysctl 单键 + 未知 oid 反向
  2  shell 提示符就绪          13 free 数值自洽 + audit=0
  3  res 基线健康全 0          14 uptime 格式完整 + 墙钟自洽
  4  date 输出完整 valid=1     15 version 输出版本/架构/mm/rtc/sha2
  5  date epoch 对宿主 <5s     16 ls -l 出现真实墙钟年份
  6  date -u 与 UTC 行一致     17 stat 仍逐字输出 mtime: 0 秒
  7  date tz 切换生效并恢复    18 help 不含新命令（基线不受扰动）
  8  mmstat 布局正确 audit=0   19 su root 出现口令提示
  9  mmtest 零泄漏 + 负例检出  20 正确口令通过
 10  二次 mmtest 页复用不涨    21 错误口令被拒（非假通过）
 11  sysctl -a 全键 + 同阶段对账 22 结束后 res 健康全 0

两条容易误判的设计（第五十四轮本地 TCG 实测教训）：
  * `kmalloc_test()` 会故意注入「重复释放 + 窗口外野指针」两个非法释放，
    因此 `badfree=2` 是**自检成功检出**，不是堆被破坏；判据必须要求 ==2。
  * size-class 的 slab 页 grow 后常驻（等价 slab cache），第一次 mmtest 会让
    free_pages 从 2047 降到约 1598；零泄漏判据是「第二轮同序列 free_pages
    不再下降」，而不是「回到 mmtest 之前的水位」。跨命令对账必须取**同阶段**
    （sysctl -a 之后紧随的 mmstat）数值，不能拿 mmtest 之前的 mmstat。

用法
----
  python3 test_round54.py                      # 自动：aarch64 + /dev/kvm 则双路，否则只 TCG
  R54_ISO=/abs/loongos.iso python3 test_round54.py     # 指定 ISO（NAS 中文路径用这个）
  R54_CODE=/usr/share/AAVMF/AAVMF_CODE.fd ...           # AAVMF 路径覆盖
  KVM_FORCE=1 python3 test_round54.py          # 无 /dev/kvm 也强制尝试（会明确报错）

产出
----
  /tmp/loong_round54_<tcg|kvm>.log     每路完整串口日志
  /tmp/loong_round54_report.json       结构化对照数据
"""
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("R54_PATH", HERE)
ISO = os.environ.get("R54_ISO") or os.path.join(ROOT, "iso", "loongos.iso")
CODE = os.environ.get("R54_CODE", "/usr/share/AAVMF/AAVMF_CODE.fd")
VARS_SRC = os.environ.get("R54_VARS", "/usr/share/AAVMF/AAVMF_VARS.fd")
VARS_TPL = "/tmp/loong_round54_vars_%s.fd"

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b\[=[0-9][a-z]")
PROMPT_END = r"(?:\r?\n|\r)> "
PW_ROOT = "loong2026"          # 教学默认口令，见 gen_authhash.c（不进内核镜像）
VERSION = "LoongOS 54.0-foundation (aarch64 / armvirt)"

NEW_CMDS = ["date", "mmstat", "mmtest", "sysctl", "uptime", "free", "version"]
SYSCTL_KEYS = ["kern.version", "kern.ostype", "kern.uptime", "kern.heap_free",
               "kern.heap_peak", "kern.heap_used", "kern.heap_audit", "kern.tz",
               "kern.rtc", "kern.clocksource", "hw.ncpu", "hw.pagesize",
               "hw.sha2", "hw.machine"]

# ---- date ----
RE_LOCAL = re.compile(r"\[date\] local\s+=\s+(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}) "
                      r"\w{3} UTC([+-])(\d{4})")
RE_EPOCH = re.compile(r"\[date\] epoch\s+=\s+(\d+) s \(UTC\)")
RE_UTC = re.compile(r"\[date\] utc\s+=\s+(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
RE_TZ = re.compile(r"\[date\] tz\s+=\s+UTC([+-])(\d+):(\d+)\s+"
                   r"source=PL031\(0x09010000\)\s+valid=(\d+)")
RE_SETTZ = re.compile(r"\[date\] 时区设为 UTC([+-])(\d+) \(offset=(-?\d+) min\)")
# ---- mm ----
RE_HEAP = re.compile(r"\[mm \] heap\s+=\s+(0x[0-9a-fA-F]+)\s+bytes=(\d+)\s+page=(\d+)B")
RE_PAGES = re.compile(r"\[mm \] pages\s+=\s+(\d+)\s+free=(\d+)\s+used=(\d+)")
RE_OBJS = re.compile(r"\[mm \] objs\s+=\s+live=(\d+)\s+used_bytes=(\d+)\s+peak_bytes=(\d+)")
RE_COUNTS = re.compile(r"\[mm \] counts = alloc=(\d+)\s+free=(\d+)\s+fail=(\d+)\s+badfree=(\d+)")
RE_MMAUDIT = re.compile(r"\[mm \] audit\s+=\s+(\d+)")
RE_MMBSS = re.compile(r"\[mm \] BSS 余量=(\d+) B")
RE_MMTEST = re.compile(r"\[mmtest\] rounds=1000x2\s+result=(PASS|FAIL)\s+bad=(\d+)")
RE_MMTEST_AFTER = re.compile(r"\[mmtest\] after: live=(\d+)\s+used_bytes=(\d+)\s+"
                             r"peak_bytes=(\d+)\s+free_pages=(\d+)")
RE_MMTEST_AUDIT = re.compile(r"\[mmtest\] audit=(\d+)\s+alloc=(\d+)\s+free=(\d+)\s+badfree=(\d+)")
# ---- sysctl / free / uptime / version / ls -l / stat ----
RE_KV = re.compile(r"^([\w.]+) = (.+)$", re.M)
RE_FREE = re.compile(r"\[free\] 动态堆.*?total=(\d+) KB\s+free=(\d+) KB\s+used_bytes=(\d+) B")
RE_FREE2 = re.compile(r"\[free\] 静态区: \.bss 余量=(\d+) B\s+mm\.audit=(\d+)")
RE_UPTIME = re.compile(r"\[uptime\] up (\d+) s \((\d+)h (\d+)m (\d+)s\)\s+"
                       r"boot_wall=(\d+)\s+now_wall=(\d+)")
RE_LSL = re.compile(r"^[-d][rwx-]{9}\s+\d+\s+root\s+\d+\s+(\d{4}-\d{2}-\d{2})", re.M)
RE_STAT = re.compile(r"mtime: (\d+) 秒")
RE_RES = re.compile(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                    r"FIQ/SErr=(\d+)\s+熔断=(\d+)")


class Session:
    """一路 QEMU（-nographic，串口走 stdio 双向），后台 pump 线程抽干串口。

    沿用第五十一轮 IO 重构：后台 daemon 线程 read(4096) + 主线程 5ms 轮询，
    避免阻塞 read(1) 被 [task1] 心跳拖死；命令一律按结束标记驱动采集。
    """
    def __init__(self, mode):
        self.mode = mode
        self.kvm = (mode == "KVM")
        gic = "3" if self.kvm else "2"
        vars_fd = VARS_TPL % mode.lower()
        shutil.copyfile(VARS_SRC, vars_fd)
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=" + gic,
               "-cpu", "host" if self.kvm else "cortex-a57"]
        if self.kvm:
            cmd += ["-enable-kvm"]
        cmd += ["-m", "256M",
                "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
                "-drive", "if=pflash,format=raw,file=" + vars_fd,
                "-cdrom", ISO, "-nographic", "-no-reboot"]
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
        self.buf = bytearray()
        self.log_path = "/tmp/loong_round54_%s.log" % mode.lower()
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        fd = self.proc.stdout
        while True:
            try:
                chunk = fd.read(4096)
            except Exception:
                break
            if not chunk:
                break
            with self._lock:
                self.buf.extend(chunk)

    def seg(self, mark=0):
        with self._lock:
            raw = bytes(self.buf[mark:])
        return ANSI.sub(b"", raw).decode("utf-8", "replace")

    def text(self):
        return self.seg(0)

    def mark(self):
        with self._lock:
            return len(self.buf)

    def wait(self, pattern, timeout=45):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.text()):
                return True
            time.sleep(0.005)
        return False

    def wait_seg(self, mark, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.seg(mark)):
                return True
            time.sleep(0.005)
        return False

    def raw_send(self, line):
        self.proc.stdin.write((line + "\r").encode())
        self.proc.stdin.flush()

    def collect(self, line, end_rx, timeout=30, prompt=True):
        """结束标记驱动采集：命中 end_rx 后再等新提示符，确保输出收干净。"""
        m = self.mark()
        self.raw_send(line)
        rx = re.compile(end_rx)
        end = time.time() + timeout
        hit = False
        while time.time() < end:
            if rx.search(self.seg(m)):
                hit = True
                break
            time.sleep(0.005)
        if hit and prompt:
            rp = re.compile(PROMPT_END)
            e2 = time.time() + 10
            while time.time() < e2:
                if rp.search(self.seg(m)):
                    break
                time.sleep(0.005)
        return self.seg(m), hit

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            with open(self.log_path, "w", encoding="utf-8") as f:
                f.write(self.text())
        except Exception:
            pass


def parse_res(t):
    d = {}
    m = RE_RES.search(t)
    if m:
        for k, v in zip(["sync_exc", "recovered", "nisv", "fatal", "trip"], m.groups()):
            d[k] = int(v)
    m = re.search(r"硬件唤醒=(\d+)Hz", t)
    if m:
        d["wake_hz"] = int(m.group(1))
    m = re.search(r"TIMER IRQ 累计=(\d+)", t)
    if m:
        d["timer_irqs"] = int(m.group(1))
    m = re.search(r"SHA256=(硬件|纯C)", t)
    if m:
        d["sha256_mode"] = m.group(1)
    return d


def healthy(d):
    return all(d.get(k, -1) == 0 for k in
               ["sync_exc", "recovered", "nisv", "fatal", "trip"])


def run_mode(mode):
    print("\n" + "=" * 68)
    print("  %s 路  %s" % (mode, "(-enable-kvm -cpu host, GICv3, 真 CPU)"
                          if mode == "KVM" else "(QEMU TCG, cortex-a57, GICv2)"))
    print("=" * 68)
    s = Session(mode)
    checks = []
    res = {"mode": mode, "cmd": " ".join(s.cmd)}

    def ck(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(("  ✅ " if ok else "  ❌ ") + name + (("  | " + detail) if detail else ""))

    try:
        boot = s.wait(r"灵珑系统.*LoongOS", timeout=60)
        ck("内核横幅 / ISO 引导成功", boot)
        ready = s.wait(r"\[shell\]|> ", timeout=30)
        ck("shell 提示符就绪", ready)
        if not (boot and ready):
            res["fatal"] = "启动未完成"
            return res, checks

        # res 基线健康
        out, _ = s.collect("res", r"SHA256=(?:硬件|纯C)", timeout=25)
        r0 = parse_res(out)
        res["res_before"] = r0
        ck("压测前 res 健康计数全 0", healthy(r0), str(r0))

        # date：完整输出 / epoch 对宿主 / -u / tz 切换
        out, hit = s.collect("date", r"\[date\] tz\s+= UTC.*valid=\d+", timeout=25)
        ml, me, mu, mt = (RE_LOCAL.search(out), RE_EPOCH.search(out),
                          RE_UTC.search(out), RE_TZ.search(out))
        host = int(time.time())
        epoch = int(me.group(1)) if me else -1
        skew = abs(host - epoch)
        res["date"] = {"local": ml.group(0) if ml else "", "epoch": epoch,
                       "host_epoch": host, "skew_s": skew,
                       "utc": mu.group(1) if mu else "", "tz": mt.group(0) if mt else ""}
        date_ok = bool(hit and ml and me and mt and mt.group(4) == "1")
        ck("date 输出完整（local/epoch/utc/tz）且 valid=1、source=PL031",
           date_ok, "%s" % (ml.group(0) if ml else "解析失败"))
        ck("date epoch 与宿主 UTC 偏差 <5s（真实墙钟，不是开机秒数）",
           epoch > 1577836800 and skew < 5,
           "guest=%s host=%s 偏差=%ss" % (epoch, host, skew))
        out_u, hit_u = s.collect("date -u", r"\[date\] tz\s+= UTC.*valid=\d+", timeout=25)
        ml_u = RE_LOCAL.search(out_u)
        utc_ok = bool(hit_u and ml_u and mu and
                      ml_u.group(1) + " " + ml_u.group(2) == mu.group(1) and
                      ml_u.group(4) == "0000")
        ck("date -u 与 date 的 utc 行逐字一致（强制 UTC+0000）",
           utc_ok, "local=-u: %s / utc: %s" %
           (ml_u.group(0) if ml_u else "解析失败", mu.group(1) if mu else "—"))
        out_tz, hit_tz = s.collect("date tz -300", r"\[date\] 时区设为 UTC", timeout=20)
        ms = RE_SETTZ.search(out_tz)
        out_tz2, _ = s.collect("date", r"\[date\] tz\s+= UTC.*valid=\d+", timeout=25)
        mt2 = RE_TZ.search(out_tz2)
        tz_ok = bool(hit_tz and ms and ms.group(3) == "-300" and mt2 and
                     mt2.group(1) == "-" and mt2.group(2) == "5")
        ck("date tz -300 时区切换生效（-5:0）", tz_ok,
           "设置=%s / 查询=%s" % (ms.group(0) if ms else "—", mt2.group(0) if mt2 else "—"))
        s.collect("date tz 480", r"\[date\] 时区设为 UTC", timeout=20)  # 恢复默认，不影响后续判据

        # mmstat：布局 + audit
        out, hit = s.collect("mmstat", r"\[mm \] BSS 余量=\d+ B", timeout=25)
        mh, mp, mo, mc, ma, mb = (RE_HEAP.search(out), RE_PAGES.search(out),
                                  RE_OBJS.search(out), RE_COUNTS.search(out),
                                  RE_MMAUDIT.search(out), RE_MMBSS.search(out))
        mm = {"heap": mh.group(1) if mh else "", "bytes": int(mh.group(2)) if mh else 0,
              "pages": int(mp.group(1)) if mp else 0, "free": int(mp.group(2)) if mp else 0,
              "used": int(mp.group(3)) if mp else 0,
              "live": int(mo.group(1)) if mo else -1,
              "audit": int(ma.group(1)) if ma else -1,
              "bss_left": int(mb.group(1)) if mb else -1}
        res["mmstat"] = mm
        mmstat_ok = bool(hit and mh and mp and mo and mc and ma and mb and
                         int(mh.group(1), 16) == 0x42000000 and
                         mm["bytes"] == 8 * 1024 * 1024 and mm["pages"] == 2048 and
                         mm["used"] == 2048 - mm["free"] and
                         mm["audit"] == 0 and mm["bss_left"] > 0 and
                         int(mc.group(3)) == 0 and int(mc.group(4)) == 0)
        ck("mmstat 堆布局 0x42000000 / 8MB / 2048 页、audit=0、BSS 余量>0",
           mmstat_ok, str(mm))

        # mmtest：1000x2 轮零泄漏
        #    ⚠️ kmalloc_test 第③步故意注入「重复释放 + 窗口外野指针」两个负例，
        #    要求 badfree_cnt 恰好 +2 才算自检有效，故 badfree=2 是检出成功。
        #    slab 页预热后常驻（free_pages 2047→约1598）属正常设计，零泄漏判据
        #    是「第二轮同序列 free_pages 不再下降」，由内核自判 PASS 覆盖。
        out, hit = s.collect("mmtest", r"\[mmtest\] audit=\d+.*badfree=\d+", timeout=60)
        mr, ma2, mc2 = (RE_MMTEST.search(out), RE_MMTEST_AFTER.search(out),
                        RE_MMTEST_AUDIT.search(out))
        mmt = {"result": mr.group(1) if mr else "", "bad": int(mr.group(2)) if mr else -1,
               "live": int(ma2.group(1)) if ma2 else -1,
               "used_bytes": int(ma2.group(2)) if ma2 else -1,
               "free_pages": int(ma2.group(4)) if ma2 else -1,
               "audit": int(mc2.group(1)) if mc2 else -1,
               "alloc": int(mc2.group(2)) if mc2 else -1,
               "free": int(mc2.group(3)) if mc2 else -1,
               "badfree": int(mc2.group(4)) if mc2 else -1}
        res["mmtest"] = mmt
        ck("mmtest 1000x2 轮零泄漏（PASS / live=0 / used=0 / audit=0 / 配对 / 负例检出）",
           bool(hit and mr and ma2 and mc2 and mmt["result"] == "PASS" and
                mmt["bad"] == 0 and mmt["live"] == 0 and mmt["used_bytes"] == 0 and
                mmt["audit"] == 0 and mmt["alloc"] == mmt["free"] and
                mmt["badfree"] == 2), str(mmt))

        # 二次 mmtest：free_pages 不得继续下降（slab 页常驻 + 页复用）
        out_b, hit_b = s.collect("mmtest", r"\[mmtest\] audit=\d+.*badfree=\d+", timeout=60)
        mr_b, ma_b = RE_MMTEST.search(out_b), RE_MMTEST_AFTER.search(out_b)
        reuse_ok = bool(hit_b and mr_b and ma_b and mr_b.group(1) == "PASS" and
                        int(ma_b.group(4)) == mmt["free_pages"] and
                        int(ma_b.group(1)) == 0 and int(ma_b.group(2)) == 0)
        ck("二次 mmtest 后 free_pages 不再下降（页复用、无持续泄漏）",
           reuse_ok, "第1次=%s 第2次=%s" %
           (mmt["free_pages"], ma_b.group(4) if ma_b else "—"))

        # sysctl -a：14 键齐全 + 与「紧随其后」的 mmstat 同阶段对账
        #     （mmstat 第一次在 mmtest 之前采集，水位已被 slab 预热改变，不能拿来比）
        out, hit = s.collect("sysctl -a", r"hw\.machine = aarch64", timeout=25)
        kv = {k.strip(): v.strip() for k, v in RE_KV.findall(out)}
        res["sysctl"] = kv
        out_ms2, _ = s.collect("mmstat", r"\[mm \] BSS 余量=\d+ B", timeout=25)
        mp2 = RE_PAGES.search(out_ms2)
        keys_ok = all(k in kv for k in SYSCTL_KEYS)
        cross_ok = bool(mp2 and "kern.heap_free" in kv and
                        kv["kern.heap_free"] == mp2.group(2) and
                        "kern.heap_audit" in kv and kv["kern.heap_audit"] == "0" and
                        "hw.pagesize" in kv and kv["hw.pagesize"] == "4096" and
                        "hw.machine" in kv and kv["hw.machine"] == "aarch64" and
                        "kern.version" in kv and kv["kern.version"] == VERSION)
        ck("sysctl -a 全 14 键齐全，heap_free/audit/pagesize 与同阶段 mmstat 对账一致",
           bool(hit and keys_ok and cross_ok),
           "键数=%d heap_free=%s(mmstat2=%s)" %
           (len(kv), kv.get("kern.heap_free"), mp2.group(2) if mp2 else "—"))

        # 单键 + 未知 oid 反向判据（独立一条判据，杜绝「恒真通过」）
        out_k, _ = s.collect("sysctl kern.version", r"kern\.version = ", timeout=20)
        out_bad, _ = s.collect("sysctl no_such_oid", r"unknown oid", timeout=20)
        one_ok = VERSION in out_k and "unknown oid" in out_bad
        ck("sysctl 单键查询正确且未知 oid 明确报错（非假通过）", one_ok,
           "单键=%s / 未知=%s" % ("OK" if VERSION in out_k else "FAIL",
                                "OK" if "unknown oid" in out_bad else "FAIL"))

        # free：数值自洽（free KB 必须与同阶段 mmstat2 的空闲页数 ×4KB 相等）
        out, hit = s.collect("free", r"\[free\] 静态区: .*mm\.audit=\d+", timeout=25)
        mf, mf2 = RE_FREE.search(out), RE_FREE2.search(out)
        free_ok = bool(hit and mf and mf2 and mp2 and int(mf.group(1)) == 8192 and
                       int(mf.group(2)) == int(mp2.group(2)) * 4 and
                       int(mf2.group(2)) == 0 and int(mf2.group(1)) == mm["bss_left"])
        ck("free 动态堆 8192KB、静态区余量与 mmstat 一致、audit=0", free_ok,
           "total=%sKB free=%sKB bss=%sB" %
           (mf.group(1) if mf else "—", mf.group(2) if mf else "—",
            mf2.group(1) if mf2 else "—"))

        # uptime：格式完整（含秒）+ 墙钟自洽
        out, hit = s.collect("uptime", r"\[uptime\] up \d+ s .*now_wall=\d+", timeout=25)
        mup = RE_UPTIME.search(out)
        up_ok = bool(hit and mup and
                     int(mup.group(1)) == int(mup.group(2)) * 3600 +
                     int(mup.group(3)) * 60 + int(mup.group(4)) and
                     int(mup.group(5)) <= int(mup.group(6)) and
                     abs(int(mup.group(6)) - epoch) < 5)
        res["uptime"] = mup.group(0) if mup else out[-200:]
        ck("uptime 格式完整（Xh Ym Zs）+ boot_wall<=now_wall 且与 date epoch 一致",
           up_ok, res["uptime"])

        # version
        out, hit = s.collect("version", r"arch=aarch64.*sha2=(?:hw|c)", timeout=20)
        ver_ok = bool(hit and VERSION in out and "mm=on" in out and
                      "rtc=PL031" in out)
        ck("version 输出版本号 / arch / mm=on / rtc=PL031 / sha2", ver_ok,
           out.strip().replace("\r", "")[:100])

        # ls -l：真实墙钟年份
        out, hit = s.collect("ls -l /etc", PROMPT_END, timeout=25)
        years = RE_LSL.findall(out)
        year_ok = bool(hit and years and
                       all(y.startswith("20") for y in years) and
                       "(无墙钟)" not in out)
        res["ls_long"] = {"rows": len(years), "years": years}
        ck("ls -l /etc 每行都是真实墙钟日期（非「开机第几秒」/「无墙钟」）",
           year_ok, "行数=%d 年份=%s" % (len(years), sorted(set(years))))

        # stat：行为基线逐字不动
        out, hit = s.collect("stat /etc/hostname", PROMPT_END, timeout=25)
        mst = RE_STAT.search(out)
        stat_ok = bool(hit and mst and mst.group(1) == "0")
        ck("stat /etc/hostname 仍逐字输出 mtime: 0 秒（29 条基线不动）",
           stat_ok, mst.group(0) if mst else "解析失败")

        # help：新命令刻意不进 help
        out, hit = s.collect("help", PROMPT_END, timeout=25)
        leaked = [c for c in NEW_CMDS if re.search(r"\b" + re.escape(c) + r"\b", out)]
        ck("help 不含第 54 轮新命令（新增能力不扰动基线）",
           bool(hit and not leaked), "泄露=%s" % (leaked if leaked else "无"))

        # su root：常量时间口令比较的正确/错误路径
        s.collect("su demo", r"现在是 demo|> ", timeout=12)
        m = s.mark()
        s.raw_send("su root")
        got = s.wait_seg(m, r"口令:", timeout=12)
        ck("su demo → su root 出现口令提示", got)
        if got:
            m2 = s.mark()
            s.raw_send(PW_ROOT)
            s.wait_seg(m2, r"现在是 root|口令不对", timeout=25)
            tail = s.seg(m2)
            ok = "现在是 root" in tail
            ck("正确口令通过（5 万轮 KDF + ct_memeq 常量时间比较）", ok,
               "SHA256H 算对 + q0..q31 存对 + 无 data abort 三项同时成立" if ok
               else "失败→查 NISV / 异常帧 / crypto 自检")
            res["kdf_auth_ok"] = ok
            s.collect("su demo", r"现在是 demo|> ", timeout=12)
            m3 = s.mark()
            s.raw_send("su root")
            s.wait_seg(m3, r"口令:", timeout=12)
            s.raw_send("wrongpw")
            s.wait_seg(m3, r"口令不对|现在是 root", timeout=25)
            t3 = s.seg(m3)
            ck("错误口令仍被拒（非假通过）",
               "口令不对" in t3 and "现在是 root" not in t3)
        else:
            ck("正确口令通过（5 万轮 KDF + ct_memeq 常量时间比较）", False, "前置口令提示未出现")
            res["kdf_auth_ok"] = False
            ck("错误口令仍被拒（非假通过）", False, "前置步骤未完成")

        # 结束后健康
        out, _ = s.collect("res", r"SHA256=(?:硬件|纯C)", timeout=25)
        r1 = parse_res(out)
        res["res_after"] = r1
        ck("全部判据跑完后 res 健康计数仍全 0", healthy(r1), str(r1))
        return res, checks
    except Exception as e:
        ck("执行未抛异常", False, "%s: %s" % (type(e).__name__, e))
        res["fatal"] = "%s: %s" % (type(e).__name__, e)
        return res, checks
    finally:
        s.close()
        print("  （串口日志：%s）" % s.log_path)


def have_kvm():
    if os.uname().machine != "aarch64":
        return False, "宿主不是 aarch64"
    if not os.path.exists("/dev/kvm"):
        return False, "无 /dev/kvm"
    if not (os.access("/dev/kvm", os.R_OK) and os.access("/dev/kvm", os.W_OK)):
        return False, "/dev/kvm 无读写权限（需 root / kvm 组）"
    return True, "aarch64 + /dev/kvm 可读可写"


def main():
    for p in (ISO, VARS_SRC, CODE):
        if not os.path.exists(p):
            print("缺文件：%s" % p)
            return 2
    if shutil.which("qemu-system-aarch64") is None:
        print("缺 qemu-system-aarch64")
        return 2

    ok_kvm, why = have_kvm()
    force = os.environ.get("KVM_FORCE") == "1"
    modes = ["TCG"]
    report = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
              "round": 54, "iso_md5": "",
              "kvm_capability": {"ok": ok_kvm, "why": why},
              "modes": {}, "summary": {}}
    try:
        import hashlib
        h = hashlib.md5()
        with open(ISO, "rb") as f:
            for c in iter(lambda: f.read(1 << 16), b""):
                h.update(c)
        report["iso_md5"] = h.hexdigest()
    except Exception:
        pass

    if ok_kvm or force:
        modes.append("KVM")
        if not ok_kvm:
            print("[r54] 警告：%s；KVM_FORCE=1 仍尝试" % why)
    else:
        print("[r54] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)

    all_checks = {}
    for mode in modes:
        r, checks = run_mode(mode)
        report["modes"][mode] = r
        all_checks[mode] = checks

    print("\n" + "=" * 68)
    print("  TCG vs KVM 第 54 轮地基能力对照表（同一 ISO md5 %s…）" % report["iso_md5"][:12])
    print("=" * 68)
    print("%-34s%16s%16s" % ("指标", "TCG", "KVM"))
    print("-" * 66)

    # 每行 = (标签, 报告字段, 子字段)；子字段 None 表示取整体/长度。
    # 第五十四轮修正：之前按固定优先级遍历 dict 键，导致 mmstat.audit 被写成
    # 堆基址、mmtest.result 被写成 audit；改为按行显式指定子字段。
    rows = [
        ("date epoch 与宿主偏差(s)", "date", "skew_s"),
        ("mmstat 堆基址", "mmstat", "heap"),
        ("mmstat audit", "mmstat", "audit"),
        ("mmtest 结果", "mmtest", "result"),
        ("mmtest 负例检出次数", "mmtest", "badfree"),
        ("mmtest after free_pages", "mmtest", "free_pages"),
        ("sysctl 键数", "sysctl", None),
        ("sysctl kern.heap_free", "sysctl", "kern.heap_free"),
        ("ls -l 真实年份行数", "ls_long", "rows"),
        ("KDF 口令验证", "kdf_auth_ok", None),
        ("结束后 NISV", "res_after", "nisv"),
    ]
    for label, key, sub in rows:
        def cell(mode, key=key, sub=sub):
            v = report["modes"].get(mode, {}).get(key)
            if v is None:
                return "—"
            if isinstance(v, dict):
                if sub is None:
                    return str(len(v))
                return str(v.get(sub, "—"))
            if isinstance(v, str) and len(v) > 16:
                return v[:16]
            return str(v)
        print("%-34s%16s%16s" % (label, cell("TCG"), cell("KVM")))
    print("-" * 66)
    print("注① KVM 列「—」= 该路未执行，不是 0，也不是通过。")
    print("注② date 偏差取 |guest_epoch - host_epoch|，QEMU PL031 默认跟随宿主 UTC；")
    print("    绝对值只在同机同会话内对比有意义。")
    print("注③ 第 54 轮只落地优先级 0 的 2.1/2.2 与低风险增量项；VFS/进程模型/")
    print("    shell 迁出等架构级项目未实现，逐项状态见 docs/演进路线_逐项状态.md。")

    for mode, checks in all_checks.items():
        npass = sum(1 for _, ok, _ in checks if ok)
        report["summary"][mode] = {"pass": npass, "fail": len(checks) - npass,
                                   "failed": [n for n, ok, _ in checks if not ok]}
        print("  %s: %d 通过 / %d 失败%s" %
              (mode, npass, len(checks) - npass,
               "  失败项=%s" % report["summary"][mode]["failed"]
               if report["summary"][mode]["failed"] else ""))
    with open("/tmp/loong_round54_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n结构化报告：/tmp/loong_round54_report.json")
    if "KVM" not in all_checks:
        print("[r54] 提醒：KVM 路未执行，本次不构成「TCG+KVM 双路复验完成」。")
    return 1 if any(v["fail"] for v in report["summary"].values()) else 0


if __name__ == "__main__":
    sys.exit(main())
