#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
同步本轮产物到 NAS（fnOS / 飞牛）。
⚠️ 安全：所有连接信息必须走环境变量，禁止在源码/注释/文档里写明文密码或真实地址。
   历史教训：早期版本曾在用法注释里写过真实口令并推送到 public 仓库，导致凭据泄露。
   该口令必须视为已泄露、立即在 NAS 上更换，并清理 Git 历史（见 docs/安全审计报告.md）。

  NAS_HOST  必填（无默认值）
  NAS_PORT  必填（无默认值）
  NAS_USER  必填（无默认值）
  NAS_PASS  必填；未设则交互式提示
  NAS_BASE  可选，默认 /vol1/1000/项目/loongos/arm

用法：
  NAS_HOST=<主机> NAS_PORT=<端口> NAS_USER=<用户> NAS_PASS='<口令>' python3 sync_nas.py
已上传文件做 md5 对账：相同则跳过，不同则直接覆盖写（fnOS 的 rename 不支持覆盖）。
"""
import os, sys, hashlib, paramiko

HOST  = os.environ.get('NAS_HOST') or input('NAS 主机: ')
PORT  = int(os.environ.get('NAS_PORT') or input('NAS SSH 端口: '))
USER  = os.environ.get('NAS_USER') or input('NAS 用户: ')
PASS  = os.environ.get('NAS_PASS') or input('NAS 密码: ')
BASE  = os.environ.get('NAS_BASE', '/vol1/1000/项目/loongos/arm')  # ARM 项目根（非敏感）

LOCAL_ROOT = os.path.dirname(os.path.abspath(__file__))  # /workspace/安卓/loongos

# (本地相对路径, 远端相对 BASE 的路径)
FILES = [
    ('本体/kernel.c',             '本体/kernel.c'),
    ('本体/kernel.elf',           '本体/kernel.elf'),
    ('本体/kernel.o',             '本体/kernel.o'),
    ('本体/desktop.c',            '本体/desktop.c'),
    ('本体/build.sh',             '本体/build.sh'),
    ('本体/path.h',               '本体/path.h'),
    ('本体/test_path_host.c',     '本体/test_path_host.c'),
    ('本体/b64.h',                '本体/b64.h'),
    ('本体/test_b64_host.c',      '本体/test_b64_host.c'),
    ('本体/test_b64_io.py',       '本体/test_b64_io.py'),
    ('本体/test_kbd.py',          '本体/test_kbd.py'),
    ('本体/docs/检查与修复记录.md', '本体/docs/检查与修复记录.md'),
    ('本体/docs/安全审计报告.md',   '本体/docs/安全审计报告.md'),
    ('README.md',                 '本体/README.md'),
    ('本体/iso/loongos.iso',      'iso/自研内核ISO/loongos.iso'),
    ('本体/test_minimize.py',     '本体/test_minimize.py'),
    # 第四十二轮：桌面全面优化（脏矩形感知 + 拖动零整屏 + 裁剪栈对称）
    ('本体/gfx.c',                '本体/gfx.c'),
    ('本体/bss_budget.txt',       '本体/bss_budget.txt'),
    ('本体/test_desktop_p3.py',   '本体/test_desktop_p3.py'),
    ('本体/diag_gfxstat.py',      '本体/diag_gfxstat.py'),
    ('本体/test_drag_render.py',  '本体/test_drag_render.py'),
    ('本体/拖动逐帧诊断.png',      '本体/拖动逐帧诊断.png'),
    # 第四十/四十一轮遗留未同步：W 键修复与手机触屏走查
    ('本体/test_kbd_desktop_w.py','本体/test_kbd_desktop_w.py'),
    ('本体/test_touch.py',        '本体/test_touch.py'),
    ('本体/触屏走查总览.png',      '本体/触屏走查总览.png'),
    # 第四十四轮：多网卡 + 多型号 + 开机自动联网（8 张混杂网卡 / 4 型号）
    ('本体/nic.c',                '本体/nic.c'),
    ('本体/nic.h',                '本体/nic.h'),
    ('本体/e1000.c',              '本体/e1000.c'),
    ('本体/rtl8139.c',            '本体/rtl8139.c'),
    ('本体/net.c',                '本体/net.c'),
    ('本体/loongos.h',            '本体/loongos.h'),
    ('本体/virtio_net.c',         '本体/virtio_net.c'),
    ('本体/test_net_host.c',      '本体/test_net_host.c'),
    ('本体/test_nic.py',          '本体/test_nic.py'),
]

def md5_local(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()

def md5_remote(sftp, rpath):
    try:
        with sftp.open(rpath, 'rb') as f:
            h = hashlib.md5()
            for chunk in iter(lambda: f.read(65536), b''):
                h.update(chunk)
        return h.hexdigest()
    except IOError:
        return None

def mkdir_p(sftp, remote_dir):
    """逐级创建远端目录（兼容中文路径）。"""
    parts = remote_dir.split('/')
    cur = ''
    for p in parts:
        if not p:
            cur = '/'
            continue
        cur = cur + p if cur == '/' else cur + '/' + p
        try:
            sftp.stat(cur)
        except IOError:
            try:
                sftp.mkdir(cur)
            except IOError:
                pass

def main():
    # 脱敏：不把真实 主机:端口:用户 打进日志/工具快照（S1 教训）
    print(f'[sync] -> <已配置连接:主机/端口/用户已隐藏>  BASE={BASE}')
    try:
        c = paramiko.SSHClient()
        c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        c.connect(HOST, port=PORT, username=USER, password=PASS,
                  timeout=15, look_for_keys=False, allow_agent=False)
    except Exception as e:
        print(f'[sync] ✗ 连接失败: {type(e).__name__}: {e}')
        sys.exit(1)

    sftp = c.open_sftp()
    sftp.chdir(BASE)
    ok = skip = fail = 0
    for local_rel, remote_rel in FILES:
        lpath = os.path.join(LOCAL_ROOT, local_rel)
        rpath = BASE + '/' + remote_rel
        if not os.path.exists(lpath):
            print(f'  ! 本地缺失跳过: {local_rel}')
            fail += 1
            continue
        lm = md5_local(lpath)
        rm = md5_remote(sftp, rpath)
        if rm == lm:
            print(f'  = 相同跳过: {remote_rel}')
            skip += 1
            continue
        rdir = os.path.dirname(rpath)
        mkdir_p(sftp, rdir)
        # 注：fnOS 的 SFTP rename 不支持覆盖已存在文件（报 OSError: Failure），
        # 故直接用 open(rpath,'wb') 覆盖写（已验证可行）。失败时文件可能不完整，
        # 重跑会再次覆盖；如需原子性可改用「写临时名 + remove 旧 + rename」。
        try:
            with sftp.open(rpath, 'wb') as f:
                with open(lpath, 'rb') as src:
                    while True:
                        chunk = src.read(262144)
                        if not chunk:
                            break
                        f.write(chunk)
            print(f'  ✓ 已同步({os.path.getsize(lpath)//1024}KB): {remote_rel}')
            ok += 1
        except Exception as e:
            print(f'  ✗ 上传失败 {remote_rel}: {type(e).__name__}: {e}')
            fail += 1
    sftp.close()
    c.close()
    print(f'[sync] 完成: 新增/更新 {ok} · 相同跳过 {skip} · 失败 {fail}')
    sys.exit(0 if fail == 0 else 2)

if __name__ == '__main__':
    main()
