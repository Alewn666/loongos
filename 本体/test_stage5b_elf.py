#!/usr/bin/env python3
"""LoongOS 阶段5② 端到端测试：ELF 加载器 + 用户程序系统调用（真实 pty 喂键盘）。
覆盖：开机种子 /bin/hello /bin/bad → run 正常程序（syscall write/exit 体面退出）→
run 坏程序（syscall 拒越界指针 -1 / 直读内核被杀回 shell）→ run 非 ELF/不存在 →
el0 演示回归（自愈+SVC）→ worker 心跳不中断。
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
        raw = bytes(buf)[STEP[0]:].replace(b"\r\n", b"\n").decode("utf-8", "replace")
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
        check("用户程序种子行", "[user] /bin/hello /bin/bad")   # 前缀：新增程序名不再让断言失效
        check("镜像没撞用户区（断言未触发）", "撞进用户区", expect=False)

        print("== help / ls /bin ==")
        send("help\r", 0.6)
        check("help 有 run 命令", "run <路径>")
        send("ls /bin\r", 0.6)
        check("有 hello", "hello")
        check("有 bad", "bad")

        print("== run /bin/hello：正常用户程序 ==")
        send("run /bin/hello\r", 2.0)
        # 入口前缀随 build.sh 的 -Ttext 走（USER_BASE 上挪 0x401D0000 后为 0x401d1030）
        check("装载日志带 entry", "[run] 装载 /bin/hello entry=0x00000000402")  # USER_BASE 上调后的地址
        check("syscall 输出①", "你好！我是 /bin/hello")
        check("syscall 输出②（用户区地址）", "我住在 0x401C0000 起的用户区")
        check("syscall 输出③", "跟内核说话只能走 syscall")
        check("exit(0) 体面退出", "[run] 程序退出 code=0")
        check("撤回内核回 shell", "已从 EL0 撤回内核")

        print("== shell 仍活着 ==")
        send("ls /\r", 0.6)
        check("ls 可用", "etc")
        send("crash udf\r", 2.3)   # 空转等心跳节拍（worker 每 20 拍=2s 打一次）
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

        print("== run /bin/bad：两道防线 ==")
        send("run /bin/bad\r", 2.5)
        check("装载成功", "[run] 装载 /bin/bad")
        check("坏事①标题", "坏事①：拿内核地址")
        check("syscall 拒越界指针", "[run] 用户指针越界，拒绝: p=0x0000000040080000")
        check("内核返回 -1（预期）", "→ 内核拒了，返回 -1（预期）")
        check("坏事②标题", "坏事②：不走 syscall")
        check("直读内核被杀", "[run] 用户程序出错(EC=0x")
        check("FAR 指向内核地址", "FAR=0x0000000040080000")
        check("杀掉回 shell", "杀掉回 shell")
        check("bad 没跑到 exit", "程序退出", expect=False)
        check("bad 没摸到内核内容", "?? 活着", expect=False)

        print("== run 其它文件 ==")
        # ⚠️ 长命令分段慢打：PL011 输入 FIFO 浅，一次灌 70+ 字符会丢字（实测丢 10 个）
        send("write /junk 0123456789012345678901234567890123456789", 0.3)
        send("01234567890123456789012345678901234567890123456789\r", 0.6)
        send("run /junk\r", 0.6)
        check("非 ELF 拒跑", "不是 ELF 文件")
        send("run /no/such\r", 0.6)
        check("不存在拒跑", "没有这个文件: /no/such")
        send("run\r", 0.6)
        check("缺参数给用法", "用法: run <路径>")

        print("== el0 演示回归 ==")
        send("el0\r", 2.0)
        check("自愈照旧（ELR+4）", "[自愈] ELR+4")
        check("SVC 退出照旧", "[el0] 收到 SVC！EL0 已完成任务")
        check("页自测值", "page[0]=0x00000000000000ab page[1]=0x00000000000000ac")

        print("== 收尾回归 ==")
        send("crash udf\r", 1.0)
        check("内核自愈不破", "[自愈] ELR+4")
        send("whoami\r", 0.6)
        check("权限系统不破", "root (uid=0)")

        print(f"\n== 结果：{ok} 过 / {fail} 挂 ==")
        return 1 if fail else 0
    finally:
        proc.kill(); proc.wait(); os.close(master)

if __name__ == "__main__":
    raise SystemExit(main())
