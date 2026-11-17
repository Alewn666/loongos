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
    # 第四十五轮：安装盘「安装系统」+ 可选目标硬盘
    # 注：loongos.h 已在第四十四轮登记，此处不重复
    ('本体/blkdev.c',             '本体/blkdev.c'),
    ('本体/blkdev.h',             '本体/blkdev.h'),
    ('本体/golden_behavior.txt',  '本体/golden_behavior.txt'),
    ('本体/test_install_disk.py', '本体/test_install_disk.py'),
    ('本体/test_install.py',      '本体/test_install.py'),
    # 第四十六轮：ARM 内核稳定性根治 + 全面优化
    ('本体/arm_io.h',             '本体/arm_io.h'),
    ('本体/boot.S',               '本体/boot.S'),
    ('本体/test_stress_arm.py',   '本体/test_stress_arm.py'),
    ('本体/run_regression_r.sh',  '本体/run_regression_r.sh'),
    ('本体/test_mousecal.py',     '本体/test_mousecal.py'),
    ('本体/iso/loongos.iso.md5',  'iso/自研内核ISO/loongos.iso.md5'),
    # 第四十七轮：渲染 / 内存 / 文件系统 / ARM SHA-256 提速
    # 注：kernel.c / kernel.elf / gfx.c / build.sh / boot.S / bss_budget.txt /
    #     golden_behavior.txt / docs 已在上面登记，此处只补历史漏登的编译单元。
    ('本体/crypto.c',             '本体/crypto.c'),
    ('本体/crypto.h',             '本体/crypto.h'),
    ('本体/fs.c',                 '本体/fs.c'),
    ('本体/fs.h',                 '本体/fs.h'),
    ('本体/gfx.h',                '本体/gfx.h'),
    # 第四十七轮收尾：ESP 写抖动的「同请求重踢 + 三次重试」修复
    # （.o 为构建中间产物，沿用清单惯例不登记，只同步源码与 kernel.elf / ISO / md5）
    ('本体/virtio_blk.c',         '本体/virtio_blk.c'),
    # 第四十八轮：b64 通道去掉 4KB 上限（流式写盘 + 临时文件原子替换）
    # 注：kernel.c / kernel.elf / fs.c / fs.h / bss_budget.txt / golden_behavior.txt /
    #     test_b64_io.py / docs / README.md / ISO / md5 已在上面登记，此处只补漏登项。
    ('手机版/说明.md',            '手机版/说明.md'),
    # 第四十九轮：桌面首批应用可用性 + 大文件只读保护 + 窗口缩放
    # 注：desktop.c / kernel.elf / bss_budget.txt / docs / README.md / ISO / md5
    #     已在上面登记，此处只补本轮新增与修改的文件。
    ('本体/test_desktop_apps.py', '本体/test_desktop_apps.py'),
    ('本体/LOONGOS_DESKTOP_TODO.md', '本体/LOONGOS_DESKTOP_TODO.md'),
    # 第五十轮：真机/KVM 双路复验准备（供 NAS 侧路线 A 直接取用；零内核源码改动）
    ('本体/test_kvm_dual.py',     '本体/test_kvm_dual.py'),
    # 第五十二轮：真机 / KVM 图形桌面双路复验（TCG 15/15 + KVM 15/15）
    # 注：README.md / docs/检查与修复记录.md / 两个 TODO 已在上面登记，此处只补新增产物。
    ('本体/test_kvm_graphic.py',  '本体/test_kvm_graphic.py'),
    ('本体/docs/真机KVM_图形复验报告.json', '本体/docs/真机KVM_图形复验报告.json'),
    ('本体/LOONGOS_KVM_FIX_TODO.md', '本体/LOONGOS_KVM_FIX_TODO.md'),
    ('本体/LOONGOS_HB_FIX_TODO.md',  '本体/LOONGOS_HB_FIX_TODO.md'),
    # 真机图形证据截图
    ('本体/docs/screenshots/真机KVM_图形交互总览.png', '本体/docs/screenshots/真机KVM_图形交互总览.png'),
    ('本体/docs/screenshots/kvm_01_desktop.png',      '本体/docs/screenshots/kvm_01_desktop.png'),
    ('本体/docs/screenshots/kvm_02_menu_open.png',    '本体/docs/screenshots/kvm_02_menu_open.png'),
    ('本体/docs/screenshots/kvm_03_app_open.png',     '本体/docs/screenshots/kvm_03_app_open.png'),
    ('本体/docs/screenshots/kvm_04_before_drag.png',  '本体/docs/screenshots/kvm_04_before_drag.png'),
    ('本体/docs/screenshots/kvm_05_after_drag.png',   '本体/docs/screenshots/kvm_05_after_drag.png'),
    ('本体/docs/screenshots/tcg_01_desktop.png',      '本体/docs/screenshots/tcg_01_desktop.png'),
    # 第五十三轮：高频中断专项压测（故障注入 + 逐寄存器回读，真机 TCG/KVM 各 16/16）
    # 注：kernel.c / boot.S / kernel.elf / bss_budget.txt / ISO / md5 / docs /
    #     README.md 已在上面登记，此处只补新增脚本、改动的判据脚本与真机报告。
    ('本体/test_irqstress.py',   '本体/test_irqstress.py'),
    ('本体/test_desktop_p2.py',  '本体/test_desktop_p2.py'),
    ('本体/docs/真机高频中断压测报告.json', '本体/docs/真机高频中断压测报告.json'),
    # 第五十四轮：演进路线地基能力（动态内存 + 真实墙钟 + 常量时间比较）
    # 注：kernel.c / kernel.elf / build.sh / bss_budget.txt / crypto.c/h / fs.c/h /
    #     ISO / iso.md5 / docs/检查与修复记录.md / README.md 已在上面登记，
    #     此处只补本轮新增的编译单元、单测、判据脚本、状态文档与报告。
    ('本体/mm.c',                 '本体/mm.c'),
    ('本体/mm.h',                 '本体/mm.h'),
    ('本体/rtc.c',                '本体/rtc.c'),
    ('本体/rtc.h',                '本体/rtc.h'),
    ('本体/test_mm_rtc_host.c',   '本体/test_mm_rtc_host.c'),
    ('本体/test_crypto.c',        '本体/test_crypto.c'),
    ('本体/test_fs_host.c',       '本体/test_fs_host.c'),
    ('本体/test_round54.py',      '本体/test_round54.py'),
    ('本体/docs/演进路线_逐项状态.md', '本体/docs/演进路线_逐项状态.md'),
    ('本体/docs/第五十四轮_地基能力判据报告.json', '本体/docs/第五十四轮_地基能力判据报告.json'),
    ('本体/LOONGOS_ROUTE_TODO.md', '本体/LOONGOS_ROUTE_TODO.md'),
    # 第五十五轮：syscall 扩容 + 用户态管道（14 个新 syscall，ls|grep|wc 真 pipe）
    # 注：kernel.c / kernel.elf / build.sh / bss_budget.txt / user/user.h /
    #     ISO / iso.md5 / docs/检查与修复记录.md / README.md 已在上面登记，
    #     此处只补本轮新增的 ABI 头、fd/pipe 编译单元、单测、用户态 ELF、
    #     判据脚本、状态文档与报告。
    ('本体/usys.h',                 '本体/usys.h'),
    ('本体/usysfd.h',               '本体/usysfd.h'),
    ('本体/usysfd.c',               '本体/usysfd.c'),
    ('本体/test_usysfd_host.c',     '本体/test_usysfd_host.c'),
    ('本体/user/u55_util.h',        '本体/user/u55_util.h'),
    ('本体/user/u55_systest.c',     '本体/user/u55_systest.c'),
    ('本体/user/u55_sh.c',          '本体/user/u55_sh.c'),
    ('本体/test_round55.py',        '本体/test_round55.py'),
    ('本体/docs/演进路线_逐项状态.md', '本体/docs/演进路线_逐项状态.md'),
    ('本体/docs/第五十五轮_syscall判据报告.json', '本体/docs/第五十五轮_syscall判据报告.json'),
    # 第五十六轮：VFS + /dev + /proc（vfs.c/h，procfs 真实数据，老路径零扰动）
    # 注：kernel.c / kernel.elf / build.sh / bss_budget.txt / usys.h / user/user.h /
    #     ISO / iso.md5 / docs/检查与修复记录.md / README.md 已在上面登记，
    #     此处只补本轮新增的 VFS 编译单元、宿主单测、用户态 ELF、判据脚本与报告。
    ('本体/vfs.h',                  '本体/vfs.h'),
    ('本体/vfs.c',                  '本体/vfs.c'),
    ('本体/test_vfs_host.c',        '本体/test_vfs_host.c'),
    ('本体/user/u56_vfs.c',         '本体/user/u56_vfs.c'),
    ('本体/test_round56.py',        '本体/test_round56.py'),
    ('本体/docs/第五十六轮_VFS判据报告.json', '本体/docs/第五十六轮_VFS判据报告.json'),
    ('本体/docs/演进路线_逐项状态.md', '本体/docs/演进路线_逐项状态.md'),
    # 第五十七轮：进程模型地基 + 每进程地址空间 + 合作式 fork/wait
    # 注：kernel.c / kernel.elf / kernel.o / build.sh / bss_budget.txt /
    #     golden_behavior.txt / docs/检查与修复记录.md / README.md / usys.h /
    #     ISO / iso.md5 / 演进路线_逐项状态.md / LOONGOS_ROUTE_TODO.md 已在上面登记，
    #     此处只补本轮新增的编译单元、宿主单测、用户态 ELF、判据脚本与报告。
    ('本体/user/user.h',            '本体/user/user.h'),
    ('本体/proc.h',                 '本体/proc.h'),
    ('本体/proc.c',                 '本体/proc.c'),
    ('本体/test_proc_host.c',       '本体/test_proc_host.c'),
    ('本体/user/u57_proc.c',        '本体/user/u57_proc.c'),
    ('本体/test_round57.py',        '本体/test_round57.py'),
    ('本体/docs/第五十七轮_进程模型判据报告.json', '本体/docs/第五十七轮_进程模型判据报告.json'),
    # 第五十八轮 58.0-A：每进程 FD 表 + 完整 execve
    # 注：kernel.c / kernel.elf / kernel.o / proc.h / build.sh / bss_budget.txt /
    #     user/user.h / docs/检查与修复记录.md / README.md / ISO / iso.md5 /
    #     演进路线_逐项状态.md / LOONGOS_ROUTE_TODO.md 已在上面登记，
    #     此处只补本轮新增的编译单元、宿主单测、用户态 ELF、判据脚本与报告。
    ('本体/fdtab.h',                '本体/fdtab.h'),
    ('本体/fdtab.c',                '本体/fdtab.c'),
    ('本体/test_fdtab_host.c',      '本体/test_fdtab_host.c'),
    ('本体/user/u58_fdproc.c',      '本体/user/u58_fdproc.c'),
    ('本体/user/u58_exec.c',        '本体/user/u58_exec.c'),
    ('本体/user/u58_target.c',      '本体/user/u58_target.c'),
    ('本体/user/u58_badexec.c',     '本体/user/u58_badexec.c'),
    ('本体/test_round58.py',        '本体/test_round58.py'),
    ('本体/docs/第五十八轮_FD与execve判据报告.json', '本体/docs/第五十八轮_FD与execve判据报告.json'),
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
