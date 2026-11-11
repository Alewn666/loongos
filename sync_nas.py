#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
同步本轮产物到 NAS（fnOS / 飞牛）。
凭据走环境变量（不硬编码）：
  NAS_HOST  默认 <NAS_HOST>
  NAS_PORT  默认 <NAS_PORT>
  NAS_USER  默认 <NAS_USER>   （root 在此穿透端口认证失败，用 <NAS_USER>）
  NAS_PASS  默认 从环境变量读，未设则提示
  NAS_BASE  默认 /vol1/1000/项目/loongos/arm   （ARM 项目根，对应本地 本体/）

用法：
  NAS_PASS='<REDACTED_PASSWORD>' python3 sync_nas.py
  NAS_HOST=新IP NAS_PORT=新端口 python3 sync_nas.py
已上传文件做 md5 对账：相同则跳过，不同则临时文件 + 原子 mv 覆盖。
"""
import os, sys, hashlib, paramiko

HOST  = os.environ.get('NAS_HOST', '<NAS_HOST>')
PORT  = int(os.environ.get('NAS_PORT', '<NAS_PORT>'))
USER  = os.environ.get('NAS_USER', '<NAS_USER>')
PASS  = os.environ.get('NAS_PASS') or input('NAS 密码: ')
BASE  = os.environ.get('NAS_BASE', '/vol1/1000/项目/loongos/arm')  # ARM 项目根

LOCAL_ROOT = os.path.dirname(os.path.abspath(__file__))  # /workspace/安卓/loongos

# (本地相对路径, 远端相对 BASE 的路径)
FILES = [
    ('本体/kernel.c',             '本体/kernel.c'),
    ('本体/kernel.elf',           '本体/kernel.elf'),
    ('本体/kernel.o',             '本体/kernel.o'),
    ('本体/desktop.c',            '本体/desktop.c'),
    ('本体/build.sh',             '本体/build.sh'),
    ('本体/docs/检查与修复记录.md', '本体/docs/检查与修复记录.md'),
    ('README.md',                 '本体/README.md'),
    ('本体/iso/loongos.iso',      'iso/自研内核ISO/loongos.iso'),
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
    print(f'[sync] -> {USER}@{HOST}:{PORT}  BASE={BASE}')
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
