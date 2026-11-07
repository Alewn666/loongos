#!/usr/bin/env python3
"""LoongOS 阶段5④ 端到端测试：重定向（真实 pty 喂键盘）。
覆盖：> 输出落盘（cat/ls/df/run hello/help）→ 输出不串口泄漏 →
< 输入给 write（= 文件复制）→ 语法错（缺路径/重复）→ 输入文件不存在 →
不支持 < 的命令报错 → DAC（demo 写 /etc 被拒）→ 未知命令的抱怨走串口
（相当于 stderr 不进文件）→ worker 心跳仍活着。
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
        raw = (bytes(buf)[STEP[0]:]
               .replace(b"\r\n", b"\n").replace(b"\r\n", b"\n")
               .decode("utf-8", "replace"))
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

        print("== > 输出重定向：cat / ls / run ==")
        send("write m1 hi-redirect\r", 0.5)
        check("建源文件 m1", "已写入")
        send("cat m1 > /m2\r", 0.5)
        check("重定向落盘回报", "已重定向")
        check("落盘字节数", "字节 -> /m2")
        check("输出没泄漏到串口", "hi-redirect", expect=False)
        send("cat /m2\r", 0.5)
        check("文件内容对", "hi-redirect")
        send("ls /etc > /lstxt\r", 0.5)
        check("ls 重定向", "已重定向")
        send("cat /lstxt\r", 0.5)
        check("list 里有 hostname", "hostname")
        check("list 里有 passwd", "passwd")
        send("run /bin/hello > /out.txt\r", 1.2)
        check("run 重定向", "已重定向")
        check("用户程序输出没进串口", "你好", expect=False)
        send("cat /out.txt\r", 0.5)
        check("out.txt 有程序输出", "你好")
        check("out.txt 有退出码", "程序退出")
        send("df > /d.txt\r", 0.5)
        send("cat /d.txt\r", 0.5)
        check("df 输出进文件", "RAM 盘")
        send("help > /h.txt\r", 0.5)
        send("cat /h.txt\r", 0.5)
        check("help 进文件且带重定向说明", "重定向")

        print("== 语法错 ==")
        send("cat >\r", 0.4)
        check("> 后缺路径报错", "语法错")
        send("cat /x > /a > /b\r", 0.4)
        check("重复 > 报错", "语法错")

        print("== < 输入重定向 ==")
        send("cat /m1 < /nofile\r", 0.4)
        check("输入文件不存在", "没有这个文件")
        send("df < /m1\r", 0.4)
        check("不支持 < 的命令明确报错", "不支持输入重定向")
        send("write /cp1 < /m2\r", 0.5)
        check("write 吃 <", "来自 < 文件")
        send("cat /cp1\r", 0.5)
        check("复制内容对", "hi-redirect")
        send("write /cp2 < /out.txt\r", 0.5)
        send("cat /cp2\r", 0.5)
        check("多行文件也复制对", "你好")

        print("== 未知命令的抱怨走串口（不进文件） ==")
        send("nosuchcmd > /e.txt\r", 0.4)
        check("未知命令报在串口", "未知命令")
        send("cat /e.txt\r", 0.5)
        check("抱怨没进文件", "未知命令", expect=False)

        print("== DAC：demo 写 /etc 被拒 ==")
        send("su demo\r", 0.5)
        # ⚠️ 设计如此：root 切别人免口令直切（非 root 才要口令）——别把测试期望写岔
        check("root 直切 demo", "现在是 demo")
        send("whoami\r", 0.4)
        check("身份 demo", "demo (uid=1000)")
        send("df > /etc/x.txt\r", 0.5)
        check("重定向写 /etc 被拒", "权限不够")
        send("su root\r", 0.5)
        check("回 root 要口令", "口令")
        send("loong2026\r", 0.4)
        send("whoami\r", 0.4)
        check("身份 root", "root (uid=0)")

        print("== 回归：cwd + 心跳 ==")
        send("pwd\r", 0.4)
        check("cwd 仍是 /", "/\n>")
        STEP[0] = len(buf)
        LAST[0] = ""
        read_for(2.5)
        tail = (bytes(buf)[STEP[0]:].replace(b"\r\n", b"\n")
                .replace(b"\r\n", b"\n").decode("utf-8", "replace"))
        hb = "[task1] 我在干活" in tail
        print(("  [OK] " if hb else "  [X] ") + "worker 心跳活着")
        ok += hb; fail += (not hb)

        print(f"\n== 结果: {ok} 过 / {fail} 挂 ==")
        return 1 if fail else 0
    finally:
        try:
            os.write(master, b"quit\r")
            time.sleep(0.2)
        except OSError:
            pass
        proc.kill()
        proc.wait(timeout=5)
        os.close(master)

if __name__ == "__main__":
    raise SystemExit(main())
