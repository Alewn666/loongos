#!/usr/bin/env python3
"""LoongOS 第三十七轮 · 批次 2「文件进出通道」端到端测试（真实 pty 喂串口）。

覆盖：
  1. b64out 导出格式（76 字符折行、每行 4 的倍数、字节数标记）
  2. 导出 → b64in 还原 → 再导出，两次 base64 完全一致（完整往返）
  3. UTF-8 中文 / 非 ASCII 字节往返（证明不是纯 ASCII 文本通道）
  4. 非法字符拒绝，且失败后不留空文件
  5. 非 4 倍数行拒绝（防止跨行 bit 被静默丢掉）
  6. 空行结束、`.` 结束
  7. 无参数 / 文件不存在的报错
  8. help 清单列出新命令
"""
import os, subprocess, time, pty, select, base64, re

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
            r, _, _ = select.select([master], [], [], 0.05)
            if master in r:
                try:
                    d = os.read(master, 4096)
                except OSError:
                    break
                if not d:
                    break
                buf.extend(d)

    ok = 0; fail = 0
    STEP = [0]; LAST = [""]

    def send(s, wait=0.5):
        STEP[0] = len(buf); LAST[0] = s
        os.write(master, s.encode())
        read_for(wait)

    def text():
        # 串口流里是 \r\r\n（pty 本地回显 + 客人回显叠加），归一两次
        raw = (bytes(buf)[STEP[0]:]
               .replace(b"\r\n", b"\n").replace(b"\r\n", b"\n")
               .decode("utf-8", "replace"))
        e = LAST[0].rstrip("\r\n")
        if e and raw.startswith(e):
            raw = raw[len(e):]
        return raw

    def check(name, cond, detail=""):
        nonlocal ok, fail
        good = bool(cond)
        print(("  [OK] " if good else "  [X] ") + name + ("" if good else "  " + detail))
        ok += good; fail += (not good)

    def has(name, needle, expect=True):
        t = text()
        check(name, (needle in t) == expect,
              "期望%s『%s』，实际片段: %r" % ("有" if expect else "无", needle, t[:200]))

    def b64_lines(t):
        """从串口文本里挑出纯 base64 行（排除命令回显与中文说明行）。"""
        out = []
        for ln in t.splitlines():
            ln = ln.strip()
            if len(ln) >= 4 and len(ln) % 4 == 0 and re.fullmatch(r"[A-Za-z0-9+/=]+", ln):
                out.append(ln)
        return out

    def norm(b):
        return (b.replace(b"\r\n", b"\n").replace(b"\r\n", b"\n")
                 .decode("utf-8", "replace"))

    def paste(path, lines, end=".\r", wait=0.35):
        """逐行粘贴，返回从 b64in 起的完整交互输出。

        每行必须留足时间给 blocking_getline 从 ring 逐字收割：0.08s 会让
        UART 输入在 ring 里积压甚至溢出，行被截断后触发「不是 4 的倍数」
        提前退出，后续行就被 shell 当命令吃掉（表现为『未知命令: .』）。
        """
        mark = len(buf)
        send("b64in %s\r" % path, 0.3)
        for ln in lines:
            send(ln + "\r", wait)
        send(end, 0.6)
        return norm(bytes(buf)[mark:])

    try:
        print("== boot ==")
        read_for(8)
        has("横幅 stage5", "LoongOS stage5")

        print("== 1) b64out 导出格式 ==")
        send("b64out /home/readme.txt\r", 0.9)
        t1 = text()
        l1 = b64_lines(t1)
        check("导出至少一行 base64", len(l1) >= 1, repr(t1[:300]))
        check("每行 <=76 字符且为 4 的倍数",
              all(len(x) <= 76 and len(x) % 4 == 0 for x in l1),
              repr([len(x) for x in l1]))
        has("带字节数说明标记", "字节文件的 base64")

        print("== 2) 导出 → 还原 → 再导出，内容往返一致 ==")
        r = paste("/copy.txt", l1)
        check("b64in 写回报", "字节 -> /copy.txt" in r, repr(r[:300]))
        send("b64out /copy.txt\r", 0.9)
        l2 = b64_lines(text())
        check("往返后 base64 逐行一致", l1 == l2,
              "导出=%r 还原后=%r" % (l1[:3], l2[:3]))

        print("== 3) UTF-8 中文 / 非 ASCII 字节往返 ==")
        payload = ("LoongOS 灵珑系统 b64 往返 0123456789 "
                   "ABC/xyz+= 第二行\n").encode("utf-8")
        enc = base64.b64encode(payload).decode("ascii")
        lines3 = [enc[i:i + 76] for i in range(0, len(enc), 76)]
        check("构造的折行都是 4 的倍数",
              all(len(x) % 4 == 0 for x in lines3))
        r = paste("/bin.txt", lines3)
        check("中文文件写入回报", "字节 -> /bin.txt" in r, repr(r[:300]))
        send("cat /bin.txt\r", 0.6)
        has("还原文件含中文", "灵珑系统")
        send("b64out /bin.txt\r", 0.9)
        check("中文文件往返 base64 一致", b64_lines(text()) == lines3)

        print("== 4) 非法字符拒绝且不留下空文件 ==")
        send("b64in /bad.txt\r", 0.3)
        # 必须是 4 的倍数（这里 8 字符）才能越过行对齐校验，进入非法字符分支；
        # 写成 "Zm9!!!!" 只有 7 字符，会先被「不是 4 的倍数」拦下。
        send("Zm9v!!!!\r", 0.3)
        has("非法字符被拒", "含非法 base64 字符")
        send("cat /bad.txt\r", 0.4)
        has("失败后不留空文件", "没有这个")

        print("== 5) 非 4 倍数行拒绝 ==")
        send("b64in /bad2.txt\r", 0.3)
        send("Zm9vYmFyZ\r", 0.3)
        has("非 4 倍数行被拒", "不是 4 的倍数")

        print("== 6) 空行结束 ==")
        send("b64in /empty.txt\r", 0.3)
        send("\r", 0.4)
        has("空行结束写入 0 字节", "已写入 0 字节")

        print("== 7) 参数与不存在的报错 ==")
        send("b64out\r", 0.3)
        has("b64out 无参数用法", "用法: b64out")
        send("b64in\r", 0.3)
        has("b64in 无参数用法", "用法: b64in")
        send("b64out /nope.txt\r", 0.3)
        has("b64out 文件不存在", "没有这个文件")

        print("== 8) help 列出新命令 ==")
        send("help\r", 0.6)
        has("help 含 b64out", "b64out <路径>")
        has("help 含 b64in", "b64in  <路径>")

    finally:
        os.close(master)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("\n== 结果: %d 过 / %d 挂 ==" % (ok, fail))
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
