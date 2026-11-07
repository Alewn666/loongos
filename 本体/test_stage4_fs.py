#!/usr/bin/env python3
"""LoongOS 阶段4 文件系统交互测试：真实 pty 喂键盘，逐条命令验证输出。"""
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
    STEP = [0]       # 每条命令前的缓冲区水位
    LAST = [""]      # 最近一条命令（用于剔除回显，防断言自绿）
    def send(s, wait=0.5):
        STEP[0] = len(buf)
        LAST[0] = s
        os.write(master, s.encode())
        read_for(wait)
    def text():
        # 只看"最近一条命令之后"的输出，并把该命令的回显从前缀剔掉
        # （第一版没剔回显 → cat 的内容断言匹配到自己打的命令字母，假绿）
        raw = bytes(buf)[STEP[0]:].replace(b"\r\n", b"\n").decode("utf-8", "replace")
        e = LAST[0].rstrip("\r\n")
        if e and raw.startswith(e):
            raw = raw[len(e):]
        return raw
    def check(name, needle, expect=True):
        nonlocal ok, fail
        found = needle in text()
        good = found == expect
        print(("  ✅ " if good else "  ❌ ") + name + ("" if good else f"（期望{'有' if expect else '无'}『{needle}』）"))
        ok += good; fail += (not good)

    print("== 启动 ==")
    read_for(7)
    check("横幅 stage5（阶段4测起改跟横幅）", "LoongOS stage")
    check("内存盘格式化", "[fs  ] 内存盘 1MB")

    print("== ls / cat ==")
    send("ls /\r", 0.6)
    check("ls / 见 etc", "etc")
    check("ls / 见 home", "home")
    send("ls /home\r", 0.6)
    check("ls /home 见 readme.txt", "readme.txt")
    send("cat /home/readme.txt\r", 0.7)
    check("cat readme 中文内容", "第一个文件")
    check("cat readme 结构说明", "superblock")
    send("cat /etc/hostname\r", 0.6)
    check("cat hostname=myos", "myos")
    send("cat /nope\r", 0.6)
    check("不存在路径报错", "没有这个路径")

    print("== write / mkdir / rm ==")
    send("write /home/hello.txt hello from stage4\r", 0.7)
    check("write 成功回执", "已写入 17 字节")
    send("ls /home\r", 0.6)
    check("ls 见新文件", "hello.txt")
    send("cat /home/hello.txt\r", 0.6)
    check("cat 读回新内容", "hello from stage4")
    send("write /home/hello.txt hello world v2\r", 0.7)
    send("cat /home/hello.txt\r", 0.6)
    check("覆写生效", "hello world v2")
    send("mkdir /home/sub\r", 0.6)
    check("mkdir 成功", "已建目录 /home/sub")
    send("write /home/sub/x.txt inner\r", 0.7)
    send("rm /home/sub\r", 0.6)
    check("非空目录拒绝删除", "删不了")
    send("rm /home/sub/x.txt\r", 0.6)
    check("删文件成功", "已删除 /home/sub/x.txt")
    send("rm /home/sub\r", 0.6)
    check("空目录可删", "已删除 /home/sub")
    send("ls /home/sub\r", 0.6)
    check("被删目录已不存在", "没有这个路径: /home/sub")

    print("== df / 退格 / 未知命令 ==")
    send("df\r", 0.6)
    check("df 显示用量", "RAM 盘: 已用")
    os.write(master, b"tass")      # 打错再退格改成 tasks（测试退格键真的能改行）
    read_for(0.2)
    for _ in range(3):
        os.write(master, b"\x7f")
        time.sleep(0.05)
    os.write(master, b"asks\r")    # "tass" 退 3 格剩 "t"，补 "asks" = "tasks"
    read_for(0.6)
    check("退格改错后命令仍可用", "任务表")
    send("foobar\r", 0.5)
    check("未知命令提示", "未知命令: foobar")

    print("== 老功能回归（命令化） ==")
    send("mmu\r", 0.6)
    check("mmu 状态含 SCTLR", "[mmu] SCTLR=")
    check("MMU 已启用 M=1", "M=0x0000000000000001")
    send("crash udf\r", 0.8)
    check("崩溃转储带 ESR", "ESR")
    check("崩溃后自愈回 shell", "自愈")
    send("el0\r", 1.0)
    check("EL0 用户态往返并撤回", "已从 EL0 撤回")
    send("ls /\r", 0.6)
    check("自愈后 fs 还能用", "home")
    read_for(2.5)
    check("worker 心跳仍在", "[task1] 我在干活")

    os.write(master, b"\x03")
    read_for(0.3)
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
    print(f"\n== 结果: {ok} 过 / {fail} 挂 ==")
    return 1 if fail else 0

if __name__ == "__main__":
    raise SystemExit(main())
