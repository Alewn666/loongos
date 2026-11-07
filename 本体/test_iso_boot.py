#!/usr/bin/env python3
"""LoongOS 阶段5⑤ - ISO 真机冒烟：UEFI 从 iso/loongos.iso 启动 → 内核 → shell 命令

判据（全部必须命中）：
  1. 固件加载 BOOTAA64.EFI（stub 打印阶段进度）
  2. 内核横幅「灵珑系统 | LoongOS」
  3. shell 提示符 `>`
  4. 命令输出正确：help / ls / cat /home/readme.txt / whoami / uname 等
不用管道一次性灌入（会把行序搞乱），改为「看到提示符再发下一条」。
"""
import os
import re
import subprocess
import sys
import time
# 宿主资源护栏（内存门槛 + 单实例锁）：NAS 只有 2GB，别把宿主拖死
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_guard import preflight, release
if not preflight(need_mb=400, label=os.path.basename(__file__)):
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
ISO = os.path.join(HERE, 'iso', 'loongos.iso')
VARS_SRC = '/usr/share/AAVMF/AAVMF_VARS.fd'
VARS = os.path.join(HERE, 'build', 'AAVMF_VARS_smoke.fd')
CODE = '/usr/share/AAVMF/AAVMF_CODE.fd'

for p in (ISO, VARS_SRC, CODE):
    if not os.path.exists(p):
        sys.exit(f'缺文件：{p}')

os.makedirs(os.path.dirname(VARS), exist_ok=True)
open(VARS, 'wb').write(open(VARS_SRC, 'rb').read())

# KVM=1 → 真 KVM + host CPU（复现飞牛虚拟机环境；TCG 不校验 VBAR 对齐，KVM 才会截断）
KVM = os.environ.get('KVM') == '1'
cmd = [
    'qemu-system-aarch64', '-machine', 'virt,gic-version=' + os.environ.get('GICVER', '2'),
    '-cpu', 'host' if KVM else 'cortex-a57',
]
if KVM:
    cmd += ['-enable-kvm']
cmd += [
    '-m', '128M',
    '-drive', f'if=pflash,format=raw,readonly=on,file={CODE}',
    '-drive', f'if=pflash,format=raw,file={VARS}',
    '-cdrom', ISO, '-nographic', '-no-reboot',
]

proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, bufsize=0)

ANSI = re.compile(rb'\x1b\[[0-9;?]*[A-Za-z]|\x1b\[=[0-9][a-z]')
buf = bytearray()
ok = []
fail = []


def pump(seconds):
    """读 seconds 秒的输出，返回期间累计的文本"""
    end = time.time() + seconds
    chunk_start = len(buf)
    while time.time() < end:
        try:
            data = proc.stdout.read(1)
        except Exception:
            break
        if not data:
            break
        buf.extend(data)
    return ANSI.sub(b'', bytes(buf[chunk_start:])).decode('utf-8', 'replace')


def wait_for(pattern, timeout=25):
    """等某个字符串出现（在累计 buffer 里）"""
    end = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < end:
        text = ANSI.sub(b'', bytes(buf)).decode('utf-8', 'replace')
        if rx.search(text):
            return True
        try:
            data = proc.stdout.read(1)
        except Exception:
            break
        if not data:
            break
        buf.extend(data)
    return False


def send(line, settle=2.0):
    proc.stdin.write((line + '\n').encode())
    proc.stdin.flush()
    return pump(settle)


def check(name, cond):
    (ok if cond else fail).append(name)
    print(('  ✅ ' if cond else '  ❌ ') + name)


try:
    print('等待内核横幅…')
    got_banner = wait_for(r'灵珑系统.*LoongOS', timeout=40)
    check('内核横幅出现（ISO 启动成功）', got_banner)
    check('stub 引导器运行（已装载）', wait_for(r'\[stub\] stub image', timeout=8))
    check('MMU 已开', wait_for(r'MMU 已开启', timeout=10))
    check('GIC 初始化', wait_for(r'\[gic \] GICv', timeout=10))
    check('文件系统就绪', wait_for(r'内存盘 1MB', timeout=10))
    check('权限系统就绪', wait_for(r'/etc/passwd', timeout=10))
    check('shell 提示符', wait_for(r'\n> |\r\n> ', timeout=15))

    out = send('help', 2.0)
    check('help 有命令清单', ('ls' in out or '命令' in out) and len(out) > 5)

    wait_for(r'\n> |\r\n> ', timeout=5)
    out = send('whoami', 2.0)
    check('whoami 返回身份', 'root' in out or 'demo' in out)

    wait_for(r'\n> |\r\n> ', timeout=5)
    out = send('ls /', 2.5)
    check('ls / 列出目录', ('etc' in out and 'home' in out) or 'bin' in out)

    wait_for(r'\n> |\r\n> ', timeout=5)
    out = send('cat /home/readme.txt', 2.5)
    check('cat 能读文件', len(out.strip()) > 20)

    # 重定向（阶段 5④ 能力）
    wait_for(r'\n> |\r\n> ', timeout=5)
    send('whoami > /home/who.txt', 2.5)
    wait_for(r'\n> |\r\n> ', timeout=5)
    out = send('cat /home/who.txt', 4.0)   # 2.5s 偶发不够（心跳刷屏挤占串口），放宽
    check('重定向 → 文件 再 cat 回来', 'root' in out or 'demo' in out)

finally:
    try:
        proc.kill()
    except Exception:
        pass
    try:
        open('/tmp/iso_smoke_full.log', 'w').write(
            ANSI.sub(b'', bytes(buf)).decode('utf-8', 'replace'))
    except Exception:
        pass

print(f"\n结果：{len(ok)} 通过 / {len(fail)} 失败")
if fail:
    print('失败项：', fail)
    sys.exit(1)
print('✅ ISO 冒烟全部通过')

release()
