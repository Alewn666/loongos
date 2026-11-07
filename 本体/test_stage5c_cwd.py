#!/usr/bin/env python3
"""LoongOS 阶段5③ 端到端测试：cwd + 相对路径（真实 pty 喂键盘）。
覆盖：pwd 初始 / → cd 绝对/相对/. /.. → write/ls/cat 相对路径 →
规范化（./../ 折叠）→ run 相对路径 → cd 拒非目录/不存在 → cd 无参回根 →
DAC 回归（su demo 不能在 / 建目录）→ worker 心跳不中断。
"""
import os, subprocess, time, pty, select

HERE = os.path.dirname(os.path.abspath(__file__))

def main():
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
           "-nographic", "-m", "128M", "-kernel", os.path.join(HERE, "kernel.elf")]
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    buf = bytearray()
    def read_for(sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.1)
            if master in r:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    break
                if not data:
                    break
                buf.extend(data)
    ok = 0; fail = 0
    STEP = [0]
    LAST = [""]
    def send(s, wait=0.5):
        STEP[0] = len(buf)
        LAST[0] = s
        os.write(master, s.encode())
        read_for(wait)
    def text():
        # ⚠️ 串口流里是 \r\r\n（pty 本地回显 + 客人回显叠加），要归一两次
        raw = bytes(buf)[STEP[0]:].replace(b"\r\n", b"\n").replace(b"\r\n", b"\n").decode("utf-8", "replace")
        e = LAST[0].rstrip("\r\n")
        if e and raw.startswith(e):
            raw = raw[len(e):]
        return raw
    def check(name, needle, expect=True):
        nonlocal ok, fail
        found = needle in text()
        good = found == expect
        print(("  [OK] " if good else "  [X] ") + name +
              ("" if good else f"（期望{'有' if expect else '无'}『{needle}』）"))
        ok += good; fail += (not good)

    try:
        print("== boot ==")
        read_for(8)
        check("横幅 stage5", "LoongOS stage5")

        print("== pwd / cd 绝对 ==")
        send("pwd\r", 0.5)
        check("pwd 打出 /", "/\n>")
        send("cd /home/demo\r", 0.5)
        send("pwd\r", 0.5)
        check("cd 绝对后 pwd=/home/demo", "/home/demo")

        print("== cd 相对 / . .. ==")
        send("cd ..\r", 0.5)
        send("pwd\r", 0.5)
        check("cd .. 到 /home", "/home\n")
        send("cd demo\r", 0.5)
        send("pwd\r", 0.5)
        check("cd 相对回 /home/demo", "/home/demo")
        send("cd ./../demo/../demo\r", 0.5)
        send("pwd\r", 0.5)
        check("./../.. 折叠规范化", "/home/demo")

        print("== 相对路径的 write/ls/cat ==")
        send("write notes hi-from-demo\r", 0.5)
        check("相对写入落规范路径", "-> /home/demo/notes")
        send("ls\r", 0.5)
        check("ls 无参列当前目录", "notes")
        send("cat notes\r", 0.5)
        check("相对 cat", "hi-from-demo")

        print("== 相对路径 run ==")
        send("cd /bin\r", 0.5)
        send("run hello\r", 2.0)
        check("相对 run 成功", "你好！我是 /bin/hello")
        check("相对 run 退出码", "[run] 程序退出 code=0")

        print("== cd 错误路径 ==")
        send("cd /junk2\r", 0.5)
        check("不存在拒 cd", "没有这个目录: /junk2")
        send("mkdir /junk2\r", 0.5)
        send("cd /junk2\r", 0.5)
        send("pwd\r", 0.5)
        check("建好后可 cd", "/junk2")
        send("cd /home/demo/notes\r", 0.5)
        check("绝对路径也拒非目录", "不是目录: /home/demo/notes")
        send("cd nope\r", 0.5)
        check("相对不存在也拒", "没有这个目录: /junk2/nope")
        send("cd\r", 0.5)
        send("pwd\r", 0.5)
        check("cd 无参回根", "/")

        print("== DAC 回归（cwd 不豁免权限）==")
        send("su demo\r", 0.5)
        read_for(0.2)
        os.write(master, b"demo2026\r"); read_for(0.5)
        send("mkdir sneakdir\r", 0.5)
        check("demo 在 / 建目录仍被拒", "权限不够")
        send("su root\r", 0.5)          # demo 切 root 也要 root 口令（"root 免"指 root 切别人）
        read_for(0.2)
        os.write(master, b"loong2026\r"); read_for(0.5)

        print("== 收尾 ==")
        send("crash udf\r", 2.3)
        # 心跳按内核虚拟时间每 10 秒一行；TCG 下虚拟时钟偏慢，最近一段未必覆盖，
        # 因此查整场会话，没出现就再等一个周期。
        _whole = bytes(buf).replace(b"\r\n", b"\n").decode("utf-8", "replace")
        if "我在干活" not in _whole:
            read_for(12)
            _whole = bytes(buf).replace(b"\r\n", b"\n").decode("utf-8", "replace")
        _hb = "我在干活" in _whole
        print(("  [OK] " if _hb else "  [X] ") + "worker 心跳仍在")
        ok += _hb; fail += (not _hb)
        check("内核自愈不破", "[自愈] ELR+4")
        send("run /bin/bad\r", 2.0)
        check("ELF 回归不破", "[run] 用户指针越界")

        print(f"\n== 结果：{ok} 过 / {fail} 挂 ==")
        return 1 if fail else 0
    finally:
        proc.kill(); proc.wait(); os.close(master)

if __name__ == "__main__":
    raise SystemExit(main())
