#!/usr/bin/env python3
"""LoongOS 阶段5 端到端测试：真实 pty 喂键盘验证 用户/口令/DAC 权限。
覆盖：boot 身份三件套 → whoami → passwd 可读 → shadow 只见哈希 →
su 错口令拒 / 对口令过（且不回显）→ demo 受 DAC 约束（私密读拒/越权写删拒/自己家可写）→
su root 回来 → root 通吃 → 老功能回归（ls/crash 自愈/tasks）。
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
    def whole():
        return bytes(buf).replace(b"\r\n", b"\n").decode("utf-8", "replace")
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
        check("身份三件套已建", "[auth] /etc/passwd + /etc/shadow")
        check("口令只存哈希说明", "SHA-256 迭代哈希")
        check("默认用户提示", "root(0) / demo(1000)")   # 横幅不再打印明文口令（2026-10-28）

        print("== whoami / passwd ==")
        send("whoami\r", 0.5)
        check("开机即 root", "root (uid=0)")
        send("cat /etc/passwd\r", 0.6)
        check("passwd 含 root:0", "root:0")
        check("passwd 含 demo:1000", "demo:1000")
        send("cat /etc/shadow\r", 0.6)
        check("shadow 行格式 name:salt:hash:iter", ":50000")   # 迭代数 2000→50000（auth_cred.h）
        check("shadow 是哈希不是明文", "loong2026", expect=False)

        print("== su：root 免口令直切 / 非 root 要口令 ==")
        send("su\r", 0.4)
        check("su 空参=已经是 root", "已经是 root")
        send("su demo\r", 0.6)
        check("root 切 demo 免口令直切", "现在是 demo")
        send("whoami\r", 0.4)
        check("whoami=demo", "demo (uid=1000)")
        send("su demo\r", 0.4)
        check("su 到当前身份=已经是", "已经是 demo")

        print("== DAC：demo 受约束 ==")
        send("cat /etc/shadow\r", 0.6)
        check("demo 读 shadow 被拒", "权限不够")
        send("write /home/mine.txt hi\r", 0.6)
        check("demo 在 root 的 /home 建文件被拒", "权限不够")
        send("cat /home/readme.txt\r", 0.6)
        check("公开文件人人可读", "第一个文件")
        send("write /home/readme.txt hacked\r", 0.6)
        check("demo 改 root 文件被拒", "权限不够")
        send("rm /home/readme.txt\r", 0.6)
        check("demo 删 root 文件被拒", "权限不够")
        send("write /home/demo/own.txt hello demo\r", 0.7)
        check("demo 在自己家建文件", "已写入 10 字节")
        send("cat /home/demo/own.txt\r", 0.6)
        check("读回自己写的", "hello demo")
        send("mkdir /home/demo/sub\r", 0.5)
        check("demo 在自己家建目录", "已建目录 /home/demo/sub")
        send("rm /home/demo/own.txt\r", 0.5)
        check("demo 删自己的文件", "已删除 /home/demo/own.txt")

        print("== su root：错口令 / demo 口令开不了 root / root 口令才行 ==")
        send("su root\r", 0.4)
        send("wrongpw\r", 0.5)
        check("错口令被拒", "口令不对")
        send("su root\r", 0.4)
        send("demo2026\r", 0.5)
        check("拿 demo 口令 su root 被拒", "口令不对")
        send("su root\r", 0.4)
        send("loong2026\r", 0.8)
        check("root 口令切换成功", "现在是 root")
        send("whoami\r", 0.4)
        check("回到 root", "root (uid=0)")
        send("write /home/demo/other.txt r\r", 0.6)
        check("root 在 demo 家建文件（通吃）", "已写入 1 字节")
        send("rm /home/demo/other.txt\r", 0.5)
        check("root 删 demo 的文件（通吃）", "已删除")
        send("rm /home/demo/sub\r", 0.5)
        check("root 删 demo 的空目录", "已删除")

        print("== 口令不回显（全缓冲负断言） ==")
        # 2026-10-28 起横幅不再打印明文口令、内核镜像也不含明文 → 全程出现次数必须为 0；
        # 若 su 输密码时有回显，立刻 >0 —— 这是"未回显 + 无泄漏"的硬证据
        whole_txt = whole()
        n_root = whole_txt.count("loong2026")
        n_demo = whole_txt.count("demo2026")
        good = (n_root == 0) and (n_demo == 0)
        print(("  [OK] " if good else "  [X] ") +
              f"口令输入全程未回显且横幅无明文（loong2026 x{n_root}, demo2026 x{n_demo}，均=0）")
        ok += good; fail += (not good)

        print("== 回归：老功能还在 ==")
        send("ls /\r", 0.5)
        check("ls 见 etc", "etc")
        check("ls 见 home", "home")
        send("crash udf\r", 0.7)
        check("udf 崩溃自愈", "[自愈]")
        send("tasks\r", 0.5)
        check("任务表可见", "任务表（共")
        send("df\r", 0.5)
        check("df 可用", "RAM 盘")

    finally:
        proc.kill(); proc.wait(); os.close(master)
    print(f"== 阶段5 端到端：{ok}/{ok+fail} 通过 ==")
    return 1 if fail else 0

if __name__ == "__main__":
    raise SystemExit(main())
