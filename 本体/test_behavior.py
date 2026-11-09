#!/usr/bin/env python3
"""LoongOS 行为黄金基线 —— 「功能不变」重构的验收尺子。

用途：架构重构（抽头文件 / 驱动表 / 命令注册 / 构建链）会改变机器码，
md5 无法证明行为等价。本脚本从 QEMU 串口采集两类输出，规范化后逐字对比：

  * 启动全日志（banner / gic / mmu / fs / auth / user / timer / shell）
  * 一组确定性命令的输出（覆盖命令表分发、重定向、管道、错误路径）

规范化只抹掉「每次运行必然不同但无语义」的量：
  - 0x.. 十六进制地址（链接布局平移、bootinfo、栈地址、inode 号）
  - >=4 位十进制数（tick / counter / 时间戳 / cycle / 容量字节）
  - worker 心跳行 [task1]（10Hz 抢占，出现时机不定）
  - CR 字符

命令输出里凡含「随机熵 / 时间 / 计数」的，也一并被规范化覆盖；
不采集 install / crash / el0 / su / net / desktop 等有副作用或依赖外设的命令。

用法：
  python3 test_behavior.py --save /tmp/golden.txt     # 采集基线
  python3 test_behavior.py --compare /tmp/golden.txt  # 与基线逐行对比
  python3 test_behavior.py                            # 只打印当前输出
"""
import os
import re
import socket
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(BASE, "kernel.elf")
SOCK = "/tmp/loongos_behavior.sock"

# 逐字对比：输出完全确定、无副作用、覆盖命令表分发与错误路径
EXACT_CMDS = [
    "help",
    "pwd",
    "whoami",
    "df",
    "ls",
    "ls /etc",
    "ls /bin",
    "cat /etc/hostname",
    "cat /home/readme.txt",
    "stat /etc/hostname",
    "wc /etc/hostname",
    "head /etc/hostname",
    "grep LoongOS /home/readme.txt",
    "ls /etc | wc | cat",
    "ls /etc | head 3",
    "ls /etc > t.txt",
    "cat t.txt",
    "write cp.txt < /etc/hostname",
    "cat cp.txt",
    "foobar",                    # 未知命令分支
    "ls /nope",                  # 路径不存在
    "cat",                       # 缺参数
    "mousecal",
    "disks",
    "scsi",
    "rng 8",
    "mmu",
    "tasks",
    "cpu",
]

# 规范化：顺序有讲究，先 hex 再长数字，避免把 hex 内部数字拆开
RE_HEX = re.compile(rb"0x[0-9a-fA-F]+")
RE_LONGDEC = re.compile(rb"(?<![0-9A-Za-z])[0-9]{4,}(?![0-9A-Za-z])")
RE_HEARTBEAT = re.compile(rb"\[task1\][^\n]*\n?")
# cpu / tasks 打印的 10Hz 节拍计数（2~3 位），每次启动相位不同但无语义
RE_TICK = re.compile(rb"(tick\s*=?\s*)[0-9]+")


def normalize(raw: bytes) -> str:
    s = raw.replace(b"\r", b"")
    s = RE_HEARTBEAT.sub(b"", s)
    s = RE_HEX.sub(b"0xADDR", s)
    s = RE_TICK.sub(rb"\1#", s)
    s = RE_LONGDEC.sub(b"#", s)
    lines = [ln.rstrip() for ln in s.decode("utf-8", "replace").split("\n")]
    # 去掉尾部空行，但保留中间空行（结构变化也要能测出来）
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines)


def drain(s, quiet=0.35, hard=4.0):
    """收集直到 quiet 秒无新数据，或 hard 秒上限。"""
    buf = b""
    last = time.time()
    end = time.time() + hard
    while time.time() < end and time.time() - last < quiet:
        s.settimeout(0.12)
        try:
            d = s.recv(8192)
        except socket.timeout:
            continue
        if d:
            buf += d
            last = time.time()
    return buf


def capture():
    for p in (SOCK,):
        try:
            os.unlink(p)
        except OSError:
            pass
    # 不挂 framebuffer/ramfb：桌面优雅降级为串口 shell，避免图形层干扰
    # wait=on 让 QEMU 等客户端连上再启动，否则启动日志（gic/mmu/fs/磁盘探测）
    # 会在连接建立前被 socket chardev 丢弃，基线就丢了最关键的一段。
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a57", "-m", "256", "-smp", "1",
        "-kernel", KERNEL, "-display", "none", "-monitor", "none",
        "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % SOCK,
        "-serial", "chardev:s0",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    s = None
    try:
        end = time.time() + 20
        while time.time() < end:
            if os.path.exists(SOCK):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(SOCK)
                    break
                except OSError:
                    s = None
            time.sleep(0.15)
        if s is None:
            raise RuntimeError("连不上串口 socket")

        # 启动日志：等提示符出现，再把剩余启动输出收干净
        boot = b""
        end = time.time() + 25
        while time.time() < end and b"> " not in boot:
            boot += drain(s, quiet=0.25, hard=1.0)
        boot += drain(s, quiet=0.5)

        out = ["=== BOOT ===", normalize(boot)]
        for line in EXACT_CMDS:
            # 清掉上一条命令残留（心跳等）
            drain(s, quiet=0.12, hard=0.4)
            s.sendall((line + "\n").encode())
            r = drain(s, quiet=0.38, hard=4.0)
            out.append("=== CMD: %s ===" % line)
            out.append(normalize(r))
        return "\n".join(out) + "\n"
    finally:
        if s:
            s.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    if "--save" in sys.argv:
        path = sys.argv[sys.argv.index("--save") + 1]
        text = capture()
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print("基线已保存: %s (%d 字节, %d 行)" %
              (path, len(text.encode()), text.count("\n") + 1))
        print(text[:900])
        return 0
    text = capture()
    if "--compare" in sys.argv:
        path = sys.argv[sys.argv.index("--compare") + 1]
        with open(path, encoding="utf-8") as f:
            golden = f.read()
        if text == golden:
            print("✅ 行为基线逐字一致：启动日志 + %d 条命令输出全部相同" %
                  len(EXACT_CMDS))
            return 0
        import difflib
        gl = golden.splitlines()
        cl = text.splitlines()
        diff = list(difflib.unified_diff(gl, cl, fromfile="基线",
                                         tofile="当前", lineterm=""))
        print("❌ 行为基线出现差异（%d 行 diff）" % len(diff))
        print("\n".join(diff[:120]))
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
