#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十三轮 · 高频中断专项压测（真机 / KVM 双路判据脚本）。

背景
----
第五十一轮串口双路、第五十二轮图形双路都在「诚实的边界」里留了同一条：

    内核抢占 timer 是 10Hz，KDF 期间未必被高频 IRQ 打断，
    「800B 异常帧在真实中断下存 q0..q31」只证明 KDF 没被破坏，
    未做高频中断专项压测。

本轮用**故障注入**把这条补严，而不是继续拿「口令验证通过」当间接证据：

  * `irqstress` 命令把硬件 timer 提到 1000Hz，
    把 q0..q31 填成 32 组各不相同的 expect[] pattern；
  * `handle_irq()` 在每次 TIMER IRQ 返回前调用 `irqstress_poison()`
    把**全部 32 个 q 寄存器**写成 0x5A 脏值；
  * 紧接着 boot.S 的 `irq_entry` 走 `RESTORE_CTX` 恢复 800B 异常帧；
  * 每来一次真实 IRQ，`irqstress_probe()` 就逐寄存器回读低/高两个
    64bit lane 与 expect[] 比对，累计 `q_mismatch`。

若 SAVE_CTX/RESTORE_CTX 漏存、或 stp/ldp 偏移错位任何一个 q，
投毒值就会漏回来，`q_mismatch > 0`。另设 `calib`：直接调用**真实投毒函数**
弄脏全部 32 个 q，要求逐寄存器比对报满 64 个 lane 失配，一次同时证明
「投毒确实改写了每个 q」与「比对逻辑真的能检出差异」，杜绝判据恒真。
压测期抑制 task_switch，避免 RESTORE_CTX 从 task1 的帧恢复 q 造成实验失真。

判据（16 项 × 双路）
--------------------
  1  ISO 引导 / 内核横幅        9  q_mismatch == 0（核心）
  2  shell 提示符就绪           10 timeout == 0
  3  res 基线健康全 0          11 RESULT PASS
  4  irqstress 输出可解析       12 二次 400 次 IRQ 仍 PASS
  5  1000Hz 真实 IRQ（间隔判定）13 结束后健康全 0 且唤醒恢复 10Hz
  6  每次 IRQ 都投毒            14 su root 出现口令提示
  7  每次 IRQ 后都做了比对      15 高频 IRQ 后 5 万轮 KDF 口令验证通过
  8  calib == 64（投毒+比对有效）16 错误口令仍被拒

用法
----
  python3 test_irqstress.py                 # 自动：有 /dev/kvm 跑 TCG+KVM，否则只 TCG
  IRQ_ISO=/abs/loongos.iso ...             # 指定 ISO（NAS 上中文路径用这个覆盖）
  KVM_FORCE=1 python3 test_irqstress.py    # 无 /dev/kvm 也强制尝试（明确报错）

产出
----
  /tmp/loong_irqstress_<TCG|KVM>.log      每路完整串口日志
  /tmp/loong_irqstress_report.json        结构化对照数据
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
ROOT = os.environ.get("IRQ_PATH", HERE)
ISO = os.environ.get("IRQ_ISO") or os.path.join(ROOT, "iso", "loongos.iso")
CODE = os.environ.get("IRQ_CODE", "/usr/share/AAVMF/AAVMF_CODE.fd")
VARS_SRC = os.environ.get("IRQ_VARS", "/usr/share/AAVMF/AAVMF_VARS.fd")
VARS_TPL = "/tmp/loong_irqstress_vars_%s.fd"

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b\[=[0-9][a-z]")
PROMPT = re.compile(r"(?:\r?\n|\r)> ")
PW_ROOT = "loong2026"          # 教学默认口令，见 gen_authhash.c（不进内核镜像）

RE_HDR = re.compile(r"\[irqstress\] want=(\d+) hz=1000 elapsed=(\d+) us")
RE_ROW = re.compile(r"\[irqstress\] rounds=(\d+) irq=(\d+)->(\d+) "
                    r"poison=(\d+) timeout=(\d+)")
RE_MIS = re.compile(r"\[irqstress\] q_mismatch=(\d+) calib=(\d+)")
RE_RES = re.compile(r"\[irqstress\] RESULT (PASS|FAIL)")


class Session:
    """一路 QEMU（-nographic，串口走 stdio 双向），后台 pump 线程抽干串口。

    第五十一轮 IO 重构：阻塞 stdout.read(1) 会被 [task1] 每 10 秒心跳拖死，
    改为后台 daemon 线程 read(4096) + 主线程 5ms 轮询；命令一律按结束标记
    驱动采集，不用固定窗口（install 竞态的第五十二轮教训同样适用）。
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
        self.log_path = "/tmp/loong_irqstress_%s.log" % mode.lower()
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

    def collect(self, line, end_rx, timeout=40):
        m = self.mark()
        self.raw_send(line)
        rx = re.compile(end_rx)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.seg(m)):
                break
            time.sleep(0.005)
        return self.seg(m)

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
    m = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+"
                  r"FIQ/SErr=(\d+)\s+熔断=(\d+)", t)
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


def parse_irqstress(t):
    d = {}
    m = RE_HDR.search(t)
    if m:
        d["want"], d["elapsed_us"] = int(m.group(1)), int(m.group(2))
    m = RE_ROW.search(t)
    if m:
        for k, v in zip(["rounds", "irq0", "irq1", "poison", "timeout"], m.groups()):
            d[k] = int(v)
    m = RE_MIS.search(t)
    if m:
        d["q_mismatch"], d["calib"] = int(m.group(1)), int(m.group(2))
    m = RE_RES.search(t)
    if m:
        d["result"] = m.group(1)
    if "irq1" in d and "irq0" in d:
        d["irq_delta"] = d["irq1"] - d["irq0"]
        if d.get("elapsed_us"):
            d["irq_hz"] = round(d["irq_delta"] * 1000000.0 / d["elapsed_us"], 1)
            d["irq_interval_us"] = round(d["elapsed_us"] / max(d["irq_delta"], 1), 1)
    return d


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

        # 3) 压测前基线健康
        out = s.collect("res", r"SHA256=(?:硬件|纯C)", timeout=25)
        r0 = parse_res(out)
        res["res_before"] = r0
        ck("压测前 res 健康计数全 0",
           all(r0.get(k, -1) == 0 for k in
               ["sync_exc", "recovered", "nisv", "fatal", "trip"]), str(r0))

        # 4~11) 第一轮：200 次真实高频 IRQ
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("irqstress 200", r"\[irqstress\] RESULT (?:PASS|FAIL)", timeout=60)
        d = parse_irqstress(out)
        res["run1"] = d
        ck("irqstress 输出四项可解析",
           all(k in d for k in ["want", "elapsed_us", "rounds", "irq0", "irq1",
                                "poison", "timeout", "q_mismatch", "calib", "result"]),
           str(d))
        ck("1000Hz 真实 IRQ（平均间隔 <= 10000us，排除 10Hz 默认档）",
           d.get("irq_delta", 0) >= 200 and
           d.get("irq_interval_us", 1e9) <= 10000,
           "IRQ 增量=%s 耗时=%sus 实测≈%sHz 间隔=%sus" %
           (d.get("irq_delta"), d.get("elapsed_us"), d.get("irq_hz"),
            d.get("irq_interval_us")))
        ck("每次 TIMER IRQ 都投毒了 NEON",
           d.get("poison", 0) >= 200, "poison=%s (want=200)" % d.get("poison"))
        ck("每次 IRQ 后都做了 32x2 逐寄存器比对",
           d.get("rounds", 0) >= 200, "rounds=%s" % d.get("rounds"))
        ck("calib == 64：投毒与比对有效性自检通过",
           d.get("calib", 0) == 64,
           "calib=%s（真实投毒弄脏 32 个 q × 2 lane，应报满 64 失配）" % d.get("calib"))
        ck("q_mismatch == 0：800B 异常帧完整保存/恢复 q0..q31",
           d.get("q_mismatch", -1) == 0,
           "mismatch=%s（漏存/错位任何一个 q 都会 >0）" % d.get("q_mismatch"))
        ck("无超时（每次 IRQ 都等到，没有挂死）", d.get("timeout", 1) == 0)
        ck("内核自判 RESULT PASS", d.get("result") == "PASS")
        if d.get("result") != "PASS":
            res["run1_raw"] = out[-1200:]

        # 12) 第二轮：400 次 IRQ，更长压测，确认不是偶发通过
        s.wait(r"\n> |\r\n> ", timeout=8)
        out2 = s.collect("irqstress 400", r"\[irqstress\] RESULT (?:PASS|FAIL)", timeout=90)
        d2 = parse_irqstress(out2)
        res["run2"] = d2
        ck("二次 400 次 IRQ 长压仍 PASS（mismatch=0 / poison>=400）",
           d2.get("result") == "PASS" and d2.get("q_mismatch", -1) == 0 and
           d2.get("poison", 0) >= 400 and d2.get("timeout", 1) == 0,
           "rounds=%s mismatch=%s poison=%s 实测≈%sHz" %
           (d2.get("rounds"), d2.get("q_mismatch"), d2.get("poison"), d2.get("irq_hz")))

        # 13) 压测结束后健康 + timer 频率已恢复 10Hz
        s.wait(r"\n> |\r\n> ", timeout=8)
        out = s.collect("res", r"SHA256=(?:硬件|纯C)", timeout=25)
        r1 = parse_res(out)
        res["res_after"] = r1
        healthy = all(r1.get(k, -1) == 0 for k in
                      ["sync_exc", "recovered", "nisv", "fatal", "trip"])
        ck("结束后健康计数仍全 0 且硬件唤醒恢复 10Hz",
           healthy and r1.get("wake_hz") == 10,
           "%s wake_hz=%s SHA256=%s" %
           (str(r1), r1.get("wake_hz"), r1.get("sha256_mode")))

        # 14) 高频 IRQ 之后，5 万轮 KDF 口令验证仍正确
        s.wait(r"\n> |\r\n> ", timeout=8)
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
            ck("高频 IRQ 后 5 万轮 KDF 口令验证通过", ok,
               "SHA256H 算对 + q0..q31 存对 + 无 data abort 三项同时成立" if ok
               else "失败→查 NISV / 异常帧 / crypto 自检")
            res["kdf_auth_ok"] = ok
            # 15) 错误口令仍被拒
            s.wait(r"\n> |\r\n> ", timeout=8)
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
            ck("高频 IRQ 后 5 万轮 KDF 口令验证通过", False, "前置口令提示未出现")
            res["kdf_auth_ok"] = False
            ck("错误口令仍被拒（非假通过）", False, "前置步骤未完成")
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
              "iso_md5": "", "kvm_capability": {"ok": ok_kvm, "why": why},
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
            print("[irq] 警告：%s；KVM_FORCE=1 仍尝试" % why)
    else:
        print("[irq] KVM 路 SKIP：%s（只跑 TCG，不冒充双路完成）" % why)

    all_checks = {}
    for mode in modes:
        r, checks = run_mode(mode)
        report["modes"][mode] = r
        all_checks[mode] = checks

    print("\n" + "=" * 68)
    print("  TCG vs KVM 高频中断压测对照表（同一 ISO md5 %s…）" % report["iso_md5"][:12])
    print("=" * 68)
    print("%-32s%16s%16s" % ("指标", "TCG", "KVM"))
    print("-" * 64)

    def g(mode, run, key):
        return str(report["modes"].get(mode, {}).get(run, {}).get(key, "—"))

    rows = [
        ("第1轮 IRQ 次数", "run1", "irq_delta"),
        ("第1轮实测 IRQ 频率(Hz)", "run1", "irq_hz"),
        ("第1轮 投毒次数", "run1", "poison"),
        ("第1轮 比对轮数", "run1", "rounds"),
        ("第1轮 q_mismatch", "run1", "q_mismatch"),
        ("第1轮 calib", "run1", "calib"),
        ("第1轮 RESULT", "run1", "result"),
        ("第2轮(400) IRQ 次数", "run2", "irq_delta"),
        ("第2轮 q_mismatch", "run2", "q_mismatch"),
        ("第2轮 RESULT", "run2", "result"),
        ("结束后 NISV", "res_after", "nisv"),
        ("结束后唤醒(Hz)", "res_after", "wake_hz"),
        ("KDF 口令验证", "", "kdf_auth_ok"),
    ]
    for label, run, key in rows:
        if run:
            print("%-32s%16s%16s" % (label, g("TCG", run, key), g("KVM", run, key)))
        else:
            a = report["modes"].get("TCG", {}).get(key, "—")
            b = report["modes"].get("KVM", {}).get(key, "—")
            print("%-32s%16s%16s" % (label, a, b))
    print("-" * 64)
    print("注① KVM 列「—」= 该路未执行，不是 0，也不是通过。")
    print("注② q_mismatch=0 且 calib==64 才是有效结论：前者证明 RESTORE_CTX 完整，")
    print("     后者证明真实投毒改写了全部 32 个 q、且比对逻辑能逐个检出。")
    print("注③ R 轮曾实测 TCG 设 1000Hz 只触发约 558Hz；本轮本地 TCG 约 960Hz，")
    print("     频率随 QEMU 版本 / 宿主负载变化，绝对值只在同机同会话内对比有意义。")

    for mode, checks in all_checks.items():
        npass = sum(1 for _, ok, _ in checks if ok)
        report["summary"][mode] = {"pass": npass, "fail": len(checks) - npass,
                                   "failed": [n for n, ok, _ in checks if not ok]}
        print("  %s: %d 通过 / %d 失败%s" %
              (mode, npass, len(checks) - npass,
               "  失败项=%s" % report["summary"][mode]["failed"]
               if report["summary"][mode]["failed"] else ""))
    with open("/tmp/loong_irqstress_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\n结构化报告：/tmp/loong_irqstress_report.json")
    if "KVM" not in all_checks:
        print("[irq] 提醒：KVM 路未执行，本次不构成「TCG+KVM 高频中断双路复验完成」。")
    return 1 if any(v["fail"] for v in report["summary"].values()) else 0


if __name__ == "__main__":
    sys.exit(main())
