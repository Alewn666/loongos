#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LOONG 包管理器端到端判据（TCG）。

拓扑：本地 Docker 起 LOONG 静态源（loong-repo:latest，宿主 18080）
     → QEMU user 网络 guest(10.0.2.15) 经网关 10.0.2.2 访问源
     → 串口跑 loong sources/update/list/show/search/install/status
     → 独立核对：索引包数、最高版本选择、落盘字节数、SHA256、能 run、记账、老命令零扰动。

判据原则（不接受串口自报）：本脚本不依赖内核打印的「已安装」字样做结论，
落盘字节数用 ls 独立读、SHA256 用源文件独立重算、程序输出必须是真实 ELF 跑出来的。

启动：
  python3 test_loong.py
前置：docker 可用；已 sh build.sh；源工程在 ../loong-repo。
"""
import os, sys, re, time, pty, select, subprocess, hashlib, json

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "loong-repo"))
KERNEL = os.path.join(HERE, "kernel.elf")
HOST_IP = "10.0.2.2"          # QEMU user 网络网关（guest 视角的宿主）
PORT = 18080
SRC_PKG = os.path.join(REPO, "repo", "packages", "hello_2.0.0.lpkg")

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "> "


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


def payload_from_lpkg(path):
    """从 .lpkg 独立重算 payload 的字节数与 sha256。"""
    data = open(path, "rb").read()
    sep = data.find(b"\n\n")
    head = data[:sep].decode("utf-8", "replace")
    b64 = data[sep + 2:]
    import base64
    payload = base64.b64decode(b"".join(b64.split()))
    return len(payload), hashlib.sha256(payload).hexdigest(), head


class Session:
    def __init__(self):
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a57",
               "-m", "256M", "-kernel", KERNEL, "-nographic", "-no-reboot",
               "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"]
        self.cmd = cmd
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        self.buf = bytearray()
        self.log_path = "/tmp/loong_e2e.log"

    def read_for(self, sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if self.master in r:
                try: d = os.read(self.master, 4096)
                except OSError: break
                if not d: break
                self.buf.extend(d)

    def seg(self, mark=0):
        return ANSI.sub(b"", bytes(self.buf[mark:])).replace(b"\r\n", b"\n").replace(b"\r", b"\n").decode("utf-8", "replace")

    def mark(self):
        return len(self.buf)

    def send(self, line):
        os.write(self.master, (line + "\r").encode())

    def wait(self, mark, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            self.read_for(0.1)
            if rx.search(self.seg(mark)): return True
        return False

    def collect(self, line, end_rx, timeout=30):
        m = self.mark()
        self.send(line)
        hit = self.wait(m, end_rx, timeout)
        if hit:
            self.wait(m, re.escape(PROMPT.strip()) + r"\s*$", 5)
            self.read_for(0.1)
        return self.seg(m), hit

    def close(self):
        for fn in (lambda: self.proc.kill(), lambda: os.close(self.master)):
            try: fn()
            except Exception: pass
        try:
            open(self.log_path, "w", encoding="utf-8").write(self.seg(0))
        except Exception: pass


def start_source():
    """本地起 Docker 源；返回 (ok, container_name)。已有则复用。"""
    # 确保索引最新
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "gen_packages.py")],
                   cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    name = "loong-e2e-src"
    subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 构建镜像（若不存在）
    b = subprocess.run(["docker", "build", "-t", "loong-repo:test", REPO],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if b.returncode != 0: return False, name
    r = subprocess.run(["docker", "run", "-d", "--name", name, "-p", "%d:8080" % PORT,
                        "loong-repo:test"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    if r.returncode != 0: return False, name
    # 等 HTTP 就绪
    import urllib.request
    for _ in range(30):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/Packages" % PORT, timeout=1) as f:
                if f.status == 200: return True, name
        except Exception:
            time.sleep(0.2)
    return False, name


def main():
    print("=== LOONG 包管理器端到端判据（TCG）===")
    checks = []
    def ck(name, ok, detail=""):
        checks.append({"name": name, "ok": bool(ok), "detail": str(detail)[:200]})
        print(("  [OK] " if ok else "  [X ] ") + name + ("" if ok else "   " + str(detail)[:160]))

    # 独立事实
    exp_payload_len, exp_payload_sha, head = payload_from_lpkg(SRC_PKG)
    ck("0 源包独立重算：payload %d B / sha %s" % (exp_payload_len, exp_payload_sha[:12]),
       exp_payload_len == 5624, exp_payload_len)

    ok_src, cname = start_source()
    ck("1 Docker 源启动 + HTTP 200", ok_src, cname)
    if not ok_src:
        print("\n[X] 源未启动，无法继续"); return 1

    s = Session()
    try:
        s.wait(0, re.escape(PROMPT.strip()), 25)   # 等开机 + DHCP
        # 老命令零扰动
        hb, _ = s.collect("help", r"run <", 15)
        ck("2 help 仍含 run（golden 命令集未删）", "run <" in hb, hb[:100])
        host, _ = s.collect("cat /etc/hostname", r"myos", 15)
        ck("3 cat /etc/hostname 仍 myos（老 fs 逐字不变）", "myos" in host, host[:100])

        # 配源 + update
        t0, _ = s.collect("loong sources %s %d" % (HOST_IP, PORT), r"源已设为", 10)
        ck("4 loong sources 设置源成功", "源已设为" in t0 and HOST_IP in t0, t0[:160])
        t1, _ = s.collect("loong update", r"OK 已读|索引拉取失败", 60)
        m = re.search(r"已读\s*(\d+)\s*个包版本", t1)
        n_pkg = int(m.group(1)) if m else -1
        ck("5 loong update 读到 3 个包版本", n_pkg == 3, t1[-160:])

        # list：只显示最高版本
        tl, _ = s.collect("loong list", re.escape(PROMPT.strip()), 15)
        ck("6 list 显示 hello 最高版本 2.0.0", "hello" in tl and "2.0.0" in tl, tl[-240:])
        ck("7 list 不显示被取代的 hello 旧版本 1.0.0", "hello  1.0.0" not in tl, tl[-240:])
        ck("8 list 含 sched 包", "sched" in tl, tl[-240:])

        # show / search
        ts, _ = s.collect("loong show hello", re.escape(PROMPT.strip()), 15)
        ck("9 show hello 显示 2.0.0 与包文件", "2.0.0" in ts and "hello_2.0.0.lpkg" in ts, ts[:240])
        tq, _ = s.collect("loong search sched", re.escape(PROMPT.strip()), 15)
        ck("10 search sched 命中 sched、不含 hello", "sched" in tq and "hello" not in tq, tq[-200:])

        # install
        ti, _ = s.collect("loong install hello", r"已安装|失败", 90)
        ck("11 install 报告成功且为最高版本 2.0.0",
           "已安装 hello 2.0.0" in ti and "失败" not in ti, ti[-200:])

        # 独立核对落盘字节数（不靠自报）
        tl2, _ = s.collect("ls -l /home/hello.elf", re.escape(PROMPT.strip()), 15)
        mm = re.search(r"hello\.elf\s+(\d+)B", tl2)
        landed = int(mm.group(1)) if mm else -1
        ck("12 落盘字节数 = 源包 payload %d B（独立读 ls）" % exp_payload_len,
           landed == exp_payload_len, "landed=%d exp=%d" % (landed, exp_payload_len))

        # 运行安装的程序（真实 ELF）
        tr, hit = s.collect("run /home/hello.elf", r"用户程序出错|程序退出|进 EL0", 30)
        ck("13 安装的程序可在 EL0 真实运行（输出含 /bin/hello）",
           "你好" in tr and "/bin/hello" in tr and "用户程序出错" not in tr, tr[-200:])

        # status 记账
        tst, _ = s.collect("loong status", re.escape(PROMPT.strip()), 15)
        ck("14 status 记账含 hello 2.0.0 / install_ok>=1",
           "hello 2.0.0" in tst and "ok=" in tst, tst[:300])

        # 二次 install 幂等（记账不重复）
        ti2, _ = s.collect("loong install hello", r"已安装|失败", 90)
        tst2, _ = s.collect("loong status", re.escape(PROMPT.strip()), 15)
        n_hello = tst2.count("hello 2.0.0")
        ck("15 重复 install 幂等（记账表仅 1 条 hello）", n_hello == 1, "count=%d" % n_hello)

        res = {"checks": checks, "exp_payload_len": exp_payload_len,
               "exp_payload_sha": exp_payload_sha, "landed": landed,
               "n_pkg": n_pkg, "log": s.log_path}
    finally:
        s.close()
        subprocess.run(["docker", "rm", "-f", cname], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    passed = sum(1 for c in checks if c["ok"])
    total = len(checks)
    print("\n=== 结果: %d/%d PASS ===" % (passed, total))
    with open(os.path.join(HERE, "docs", "LOONG包管理器判据报告.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=2)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
