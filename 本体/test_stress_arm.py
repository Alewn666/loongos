#!/usr/bin/env python3
"""LoongOS 第四十六轮验收：ARM 内核长稳 / 异常不再停机 / 时钟跳变 / 串口洪水。

背景（改动前基线，已实测复现）：
    `crash abort` → ESR=0x96000010 (EC=0x25, ISV=0) → handle_sync 打印
    `[停机] NISV/stage-2 数据中止：不能 ELR+4 盲跳，停机保留现场`
    → `for(;;) wfe` 整机永久死。真机 ARM 上 NISV 中止（MMIO / stage-2 / 总线
    错误）远比 TCG 频繁，正是「系统用着用着就死机」的确定性根因。

本脚本把该场景与长时间混合压力固化为回归门禁：
  ① 混合命令 N 轮（含 udf / brk / abort / wronly 四种崩溃演示）
  ② QMP stop→cont 模拟 VM 长暂停 / 虚拟时钟跳变
  ③ 串口 IRQ 洪水 20000 字节
  ④ 长时间空转后心跳
  全程以 `res` 的健康计数作为心跳（不用 `> `：help 文本里含 "> "，会误判）。

判据：
  - 每个心跳都拿到完整 res 输出（含「UART FIFO 收满」尾行）
  - 串口日志中绝不出现 `[停机]` / `[熔断]` / `FATAL ... 停机`
  - NISV 计数 >= 1（证明真的走过 NISV 恢复路径）且自愈计数同步增长
  - TIMER IRQ 在 stop/cont 后继续推进（tick 结算没把中断卡死）

用法：python3 test_stress_arm.py
"""
import os
import re
import socket
import subprocess
import sys
import time
import json

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(BASE, "kernel.elf")
SER = "/tmp/loongos_stress.sock"
QMP = "/tmp/loongos_stress.qmp"
QERR = "/tmp/loongos_stress.err"

ROUNDS = 6
BATCH = ["whoami", "tasks", "mmu", "bench", "disks", "net",
         "ping 10.0.2.2 1", "cpu", "ls /etc", "help",
         "crash udf", "crash brk", "crash abort", "crash wronly"]

fails = []
LOG = bytearray()


def ck(name, ok, extra=""):
    print("  %-52s %s%s" % (name, "✅" if ok else "❌", ("  " + extra) if extra else ""), flush=True)
    if not ok:
        fails.append(name)


class Qmp:
    def __init__(self, path):
        end = time.time() + 20
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.1)
        self.s.settimeout(6)
        self.buf = b""
        self._line()
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8", "replace"))

    def cmd(self, ex, args=None):
        m = {"execute": ex}
        if args:
            m["arguments"] = args
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                return r


class VM:
    def __init__(self):
        for p in (SER, QMP):
            try:
                os.unlink(p)
            except OSError:
                pass
        self.err = open(QERR, "w")
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2",
               "-cpu", "cortex-a57", "-m", "256", "-smp", "2",
               "-kernel", KERNEL, "-display", "none", "-monitor", "none",
               "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % SER,
               "-serial", "chardev:s0",
               "-qmp", "unix:%s,server=on,wait=off" % QMP]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=self.err)
        self.s = None
        end = time.time() + 45
        while time.time() < end:
            if os.path.exists(SER):
                try:
                    self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    self.s.connect(SER)
                    self.s.settimeout(0.2)
                    break
                except OSError:
                    self.s = None
            time.sleep(0.15)
        if self.s is None:
            raise RuntimeError("连不上串口 socket")
        boot = bytearray()
        end = time.time() + 70
        while time.time() < end and b"> " not in boot:
            self._pump_into(boot, 0.5)
        LOG.extend(boot)
        self.booted = b"> " in boot
        self.qmp = Qmp(QMP)

    def _pump_into(self, buf, sec):
        end = time.time() + sec
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if d:
                    buf.extend(d)
            except socket.timeout:
                pass

    def drain(self, sec=0.3):
        b = bytearray()
        self._pump_into(b, sec)
        LOG.extend(b)
        return bytes(b)

    def send_raw(self, line, settle=0.45):
        self.drain(0.2)
        self.s.sendall((line + "\n").encode())
        b = bytearray()
        self._pump_into(b, settle)
        LOG.extend(b)
        return b.decode("utf-8", "replace")

    def heartbeat(self, timeout=9.0):
        """严格心跳：必须收到 res 最后一行「UART FIFO 收满」才算系统活着。"""
        self.drain(0.2)
        self.s.sendall(b"res\n")
        b = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            self._pump_into(b, 0.25)
            if b"UART FIFO" in b:
                break
        LOG.extend(b)
        txt = b.decode("utf-8", "replace")
        m = re.search(r"健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+FIQ/SErr=(\d+)\s+熔断=(\d+)", txt)
        irq = re.search(r"TIMER IRQ 累计=(\d+)", txt)
        jump = re.search(r"时钟跳变截断=(\d+)", txt)
        return {
            "alive": b"UART FIFO" in b,
            "sync": int(m.group(1)) if m else -1,
            "rec": int(m.group(2)) if m else -1,
            "nisv": int(m.group(3)) if m else -1,
            "fatal": int(m.group(4)) if m else -1,
            "trip": int(m.group(5)) if m else -1,
            "irq": int(irq.group(1)) if irq else -1,
            "jump": int(jump.group(1)) if jump else -1,
            "raw": txt,
        }

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        try:
            self.err.close()
        except OSError:
            pass


def main():
    print("===== LoongOS ARM 内核长稳压力验收 =====", flush=True)
    print("QEMU TCG / -smp 2 / %d 轮混合压力" % ROUNDS, flush=True)
    vm = VM()
    try:
        ck("启动到 shell", vm.booted)
        if not vm.booted:
            print(LOG.decode("utf-8", "replace")[-800:])
            return 1
        hb0 = vm.heartbeat()
        ck("基线 res 心跳", hb0["alive"], "同步异常=%d NISV=%d" % (hb0["sync"], hb0["nisv"]))
        print("    基线计数: sync=%d rec=%d nisv=%d fatal=%d irq=%d" %
              (hb0["sync"], hb0["rec"], hb0["nisv"], hb0["fatal"], hb0["irq"]), flush=True)

        # ① 混合压力：每轮批量命令（含 4 种崩溃演示）后做一次严格心跳
        print("\n① 混合命令压力（每轮含 crash udf/brk/abort/wronly）", flush=True)
        for rnd in range(1, ROUNDS + 1):
            for c in BATCH:
                vm.send_raw(c)
            hb = vm.heartbeat()
            print("  第 %d 轮: alive=%s sync=%d rec=%d nisv=%d irq=%d" %
                  (rnd, hb["alive"], hb["sync"], hb["rec"], hb["nisv"], hb["irq"]), flush=True)
            ck("第 %d 轮压力后心跳" % rnd, hb["alive"])
            if not hb["alive"]:
                print("    串口尾部:", hb["raw"][-400:].replace("\n", " | "), flush=True)
                break
            if rnd == 3:
                # ② VM 长暂停 / 虚拟时钟跳变
                print("\n② QMP stop 6s → cont（模拟 VM 长暂停 / 时钟跳变）", flush=True)
                irq_before = hb["irq"]
                vm.qmp.cmd("stop")
                time.sleep(6)
                vm.qmp.cmd("cont")
                hb2 = vm.heartbeat(12.0)
                ck("stop/cont 后系统仍响应", hb2["alive"])
                ck("stop/cont 后 TIMER IRQ 继续推进",
                   hb2["irq"] >= irq_before and hb2["irq"] - irq_before < 5000,
                   "ΔIRQ=%d" % (hb2["irq"] - irq_before))
                ck("stop/cont 未触发异常", hb2["fatal"] == 0 and hb2["trip"] == 0)
            if rnd == 4:
                # ③ 串口 IRQ 洪水
                print("\n③ 串口洪水 20000 字节（IRQ 上下文有界循环验证）", flush=True)
                vm.s.sendall(b"a" * 20000)
                time.sleep(1.5)
                vm.s.sendall(b"\r")
                vm.drain(1.0)
                hb3 = vm.heartbeat(12.0)
                ck("串口洪水后系统仍响应", hb3["alive"])
                ck("串口洪水未触发异常", hb3["fatal"] == 0 and hb3["trip"] == 0)

        # ④ 长时间空转后心跳（「用着用着」的字面场景）
        print("\n④ 空闲 12 秒后再心跳（长时间运行不退化为假死）", flush=True)
        time.sleep(12)
        hbf = vm.heartbeat(12.0)
        ck("长时空转后心跳", hbf["alive"], "irq=%d 跳变截断=%d" % (hbf["irq"], hbf["jump"]))

        # ⑤ 全局判据
        print("\n⑤ 全局判据", flush=True)
        all_txt = LOG.decode("utf-8", "replace")
        ck("NISV 恢复路径确实被走过", hbf["nisv"] >= 1, "NISV=%d" % hbf["nisv"])
        ck("异常全部落到「自愈/回 shell」", hbf["rec"] >= 3, "rec=%d" % hbf["rec"])
        ck("全程无 [停机]", "[停机]" not in all_txt)
        ck("全程无 [熔断] / 嵌套异常停机", "[熔断]" not in all_txt and "[嵌套异常]" not in all_txt)
        ck("全程无 FIQ/SError 停机", "无法处理，停机" not in all_txt)
        ck("出现 NISV 恢复提示", "NISV/stage-2 数据中止（不盲跳 ELR+4）" in all_txt)
        ck("未发生时钟跳变截断（正常 TCG 拓扑）", hbf["jump"] == 0, "jump=%d" % hbf["jump"])
    finally:
        vm.close()

    print("\n===== 结果：%s =====" % ("全部通过 ✅" if not fails else "%d 项失败 ❌" % len(fails)), flush=True)
    for f in fails:
        print("  ❌ " + f, flush=True)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
