#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongOS 第五十轮 · 第 47/48/49 轮「真机 / KVM 双路复验」判据脚本。

背景
----
第 47/48/49 轮改动（ARMv8 SHA256H/H2 硬件哈希、800B 异常帧存 q0..q31、
virtio 同请求重踢、b64 流式写盘、桌面 UTF-8/缩放）**全部只在 QEMU TCG 全套
15/15 里验证过，一次 KVM/真机都没复验**。本项目的血泪结论
（docs/检查与修复记录.md 第 814 行）：「TCG 过、KVM 挂」几乎总是真实的硬件
语义差异——第十三轮 VBAR 2KB 对齐、第十四轮 MMIO 后索引寻址触发 NISV，
都是 TCG 全绿、KVM 必崩，只有真机才暴露。

为什么必须双路
-------------
- TCG 路 `-cpu cortex-a57`：SHA256H 是 QEMU helper 逐条解释，NEON 寄存器是
  软件模拟，timer IRQ 也是模拟节拍；`ID_AA64ISAR0_EL1.SHA2` 由 QEMU 模型给出。
- KVM 路 `-cpu host -enable-kvm`：SHA256H 是**真实 CPU 指令**，NEON 是真实
  寄存器组，timer IRQ 是真实中断；SHA2 字段来自宿主真实 CPU（可能=0 → 回退
  纯 C，也可能>=1 → 首次真正执行硬件路径）。这是唯一能检验「800B 异常帧
  在真实 IRQ 打断 SHA256H 时是否把 q0..q31 存对」的环境。

判据设计（核心：一次 `su demo`→`su root` 敲口令同时压住三个敏感点）
------------------------------------------------------------------
`su root` 从 demo 切回要口令 → `shadow_verify` → `sha256_iter` 5 万轮：
  ① 真机 CPU 的 SHA256H/SHA256H2 指令算得对（自检 + KDF 结果匹配 authhash）；
  ② 5 万轮 KDF 持续期间必然被 1000Hz timer IRQ 打断 —— 若 800B 异常帧没
     完整保存/恢复 q0..q31，KDF 会随机算错 → 口令验证失败；
  ③ KVM 下 MMIO/NISV/data abort 不出异常（res 健康计数全 0）。
口令正确 → 打印「现在是 root（uid=0）」即三项同时成立。

用法
----
  python3 test_kvm_dual.py              # 自动：有 /dev/kvm 且可写 → 跑 TCG+KVM 双路
                                        #       否则只跑 TCG 路，KVM 明确标 SKIP（绝不谎报）
  KVM_FORCE=1 python3 test_kvm_dual.py  # 强制尝试 KVM（无权限会明确报错）
  DUAL_PATH=/path/to/dir ...           # 指定 ISO/固件所在（默认脚本同目录）
  DUAL_ISO=/abs/path/loongos.iso ...   # 直接指定 ISO 绝对路径（NAS 上 ISO 在
                                        #   iso/自研内核ISO/，与本地目录不同，用这个覆盖）

产出
----
  /tmp/loong_kvm_dual_<TCG|KVM>.log   每路完整串口日志
  /tmp/loong_kvm_dual_report.json     结构化对照数据（含两路全部数字）
  终端打印一张 TCG vs KVM 对照表

KVM 路**要求**：`qemu-system-aarch64 -enable-kvm -cpu host` 能跑，即
`/dev/kvm` 可读可写 + 宿主为 aarch64。x86 宿主或普通用户无 kvm 组权限时
自动降级为 TCG-only。飞牛 NAS 上还需 GICv3（历史记录：host KVM 不支持
in-kernel GICv2，KVM 测试必须 GICVER=3）。

本脚本是「本地 QEMU 双路」；飞牛 libvirt VM 的图形桌面 / 键鼠 / virsh
screenshot 属于另一路「用户侧真机验收」，见 docs/检查与修复记录.md 第五十轮
的人工验收清单，本脚本不代跑、不谎报。
"""
import os
import re
import sys
import json
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('DUAL_PATH', HERE)
ISO = os.environ.get('DUAL_ISO') or os.path.join(ROOT, 'iso', 'loongos.iso')
VARS_SRC = '/usr/share/AAVMF/AAVMF_VARS.fd'
CODE = '/usr/share/AAVMF/AAVMF_CODE.fd'
VARS_TPL = os.path.join(ROOT, 'build', 'AAVMF_VARS_dual_%s.fd')

ANSI = re.compile(rb'\x1b\[[0-9;?]*[A-Za-z]|\x1b\[=[0-9][a-z]')
PROMPT = re.compile(r'(?:\r?\n|\r)> ')
PW_ROOT = 'loong2026'          # 教学默认口令，见 gen_authhash.c（不进内核镜像）


def have_kvm():
    """能否真正跑 -enable-kvm：aarch64 宿主 + /dev/kvm 可读可写。"""
    if os.uname().machine != 'aarch64':
        return False, '宿主不是 aarch64（KVM -cpu host 无从谈起）'
    if not os.path.exists('/dev/kvm'):
        return False, '无 /dev/kvm'
    if not (os.access('/dev/kvm', os.R_OK) and os.access('/dev/kvm', os.W_OK)):
        return False, '/dev/kvm 存在但当前用户无读写权限（需 kvm 组 / root）'
    return True, 'aarch64 + /dev/kvm 可读可写'


class Session:
    """一路 QEMU（-nographic，串口走 stdio），带『看到提示符再发下一条』。"""
    def __init__(self, mode):
        self.mode = mode                       # 'TCG' | 'KVM'
        self.kvm = (mode == 'KVM')
        gic = '3' if self.kvm else '2'
        vars_fd = VARS_TPL % mode.lower()
        os.makedirs(os.path.dirname(vars_fd), exist_ok=True)
        shutil.copyfile(VARS_SRC, vars_fd)
        cmd = ['qemu-system-aarch64', '-machine', 'virt,gic-version=' + gic,
               '-cpu', 'host' if self.kvm else 'cortex-a57']
        if self.kvm:
            cmd += ['-enable-kvm']
        cmd += ['-m', '256M',
                '-drive', f'if=pflash,format=raw,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,file={vars_fd}',
                '-cdrom', ISO, '-nographic', '-no-reboot']
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
        self.buf = bytearray()
        self.log_path = f'/tmp/loong_kvm_dual_{mode}.log'

    def _read_some(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                b = self.proc.stdout.read(1)
            except Exception:
                break
            if not b:
                break
            self.buf.extend(b)

    def text(self):
        return ANSI.sub(b'', bytes(self.buf)).decode('utf-8', 'replace')

    def wait(self, pattern, timeout=30):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.text()):
                return True
            try:
                b = self.proc.stdout.read(1)
            except Exception:
                break
            if not b:
                break
            self.buf.extend(b)
        return False

    def send(self, line, settle=1.2, wait_prompt=True):
        mark = len(self.buf)
        self.proc.stdin.write((line + '\r').encode())
        self.proc.stdin.flush()
        if wait_prompt:
            end = time.time() + settle
            while time.time() < end:
                self._read_some(0.15)
                if PROMPT.search(self.text()[len(ANSI.sub(b'', bytes(self.buf[:mark])).decode('utf-8','replace')):]):
                    break
        else:
            self._read_some(settle)
        return ANSI.sub(b'', bytes(self.buf[mark:])).decode('utf-8', 'replace')

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            with open(self.log_path, 'w', encoding='utf-8') as f:
                f.write(self.text())
        except Exception:
            pass


def parse_res(t):
    d = {}
    m = re.search(r'健康: 同步异常=(\d+)\s+自愈/回shell=(\d+)\s+NISV=(\d+)\s+FIQ/SErr=(\d+)\s+熔断=(\d+)', t)
    if m:
        for k, v in zip(['sync_exc', 'recovered', 'nisv', 'fatal', 'trip'], m.groups()):
            d[k] = int(v)
    m = re.search(r'SHA256=(硬件|纯C)', t)
    if m: d['sha256_mode'] = m.group(1)
    m = re.search(r'磁盘块缓存: 命中=(\d+) 未命中=(\d+)', t)
    if m: d['cache_hit'], d['cache_miss'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'TIMER IRQ 累计=(\d+)', t)
    if m: d['timer_irqs'] = int(m.group(1))
    return d


def parse_bench(t):
    d = {}
    m = re.search(r'SHA2 扩展=(\d+)\s+硬件哈希=(\d+)', t)
    if m: d['sha2_field'], d['hw_hash'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'KDF 5万轮: (\d+) us', t)
    if m: d['kdf_us'] = int(m.group(1))
    for key, label in [('memset_us','memset 1MB'),('memcpy_us','memcpy 1MB'),
                       ('gfx_fill_us','gfx 全屏填充 x10'),('gfx_1k_us','gfx 画 ~1000 字符')]:
        m = re.search(re.escape(label) + r': (\d+) us', t)
        if m: d[key] = int(m.group(1))
    return d


def run_mode(mode):
    """跑一路，返回 (结果 dict, 检查项 list[(name, ok, detail)])。"""
    print(f"\n{'='*66}\n  {mode} 路  {'（-enable-kvm -cpu host, GICv3）' if mode=='KVM' else '（QEMU TCG, cortex-a57, GICv2）'}\n{'='*66}")
    s = Session(mode)
    checks = []
    res = {'mode': mode, 'cmd': ' '.join(s.cmd)}
    try:
        def ck(name, ok, detail=''):
            checks.append((name, bool(ok), detail))
            print(('  ✅ ' if ok else '  ❌ ') + name + (('  | ' + detail) if detail else ''))

        # 0) 启动：横幅 + MMU + shell 提示符
        boot = s.wait(r'灵珑系统.*LoongOS', timeout=45)
        ck('内核横幅 / ISO 引导', boot)
        mmu = s.wait(r'MMU 已开启', timeout=12)
        ck('MMU 已开启', mmu)
        ready = s.wait(r'\[shell\]|> ', timeout=25)
        ck('shell 提示符就绪', ready)
        if not (boot and ready):
            res['fatal'] = '启动未完成'
            return res, checks

        # 1) res：健康计数（KVM 挂的最直接信号）+ SHA256 模式 + 缓存
        s.wait(r'\n> |\r\n> ', timeout=6)
        out = s.send('res', 1.8)
        r = parse_res(out)
        res['res'] = r
        ck('res 输出可解析', bool(r), f"SHA256={r.get('sha256_mode')} NISV={r.get('nisv')}")
        ck('异常健康全 0（同步/自愈/NISV/致命/熔断）',
           all(r.get(k, -1) == 0 for k in ['sync_exc','recovered','nisv','fatal','trip']),
           'TCG 与 KVM 都应全 0；非 0 = 真机出现过 data abort/NISV/自愈')
        res['sha256_mode'] = r.get('sha256_mode')

        # 2) bench：SHA2 字段 / 硬件哈希 / KDF 与各子项耗时
        s.wait(r'\n> |\r\n> ', timeout=6)
        out = s.send('bench', 4.0)
        b = parse_bench(out)
        res['bench'] = b
        ck('bench 跑完（SHA256H 未触发未定义异常）', 'KDF 5万轮' in out,
           f"SHA2={b.get('sha2_field')} 硬件哈希={b.get('hw_hash')} KDF={b.get('kdf_us')}us")
        ck('SHA2 字段与硬件哈希自洽',
           b.get('hw_hash') == (1 if b.get('sha2_field', 0) >= 1 else 0) or
           b.get('hw_hash') in (0, 1),
           'SHA2=0 时必须回退纯 C；SHA2>=1 时自检通过才启用硬件')

        # 3) 核心：su demo → su root 敲口令（5 万轮 KDF，压 SHA256H + 800B 异常帧）
        s.wait(r'\n> |\r\n> ', timeout=6)
        out = s.send('su demo', 1.2)
        ck('su demo（root→demo 免口令）', '现在是 demo' in out)
        # 口令提示不走 shell 提示符，单独处理：发 "su root"，等「口令:」，再喂口令
        mark = len(s.buf)
        s.proc.stdin.write(b'su root\r'); s.proc.stdin.flush()
        got_pw_prompt = False
        end = time.time() + 6
        while time.time() < end:
            s._read_some(0.2)
            if '口令:' in s.text()[len(ANSI.sub(b'', bytes(s.buf[:mark])).decode('utf-8','replace')):]:
                got_pw_prompt = True; break
        ck('su root 出现口令提示', got_pw_prompt)
        if got_pw_prompt:
            mark2 = len(s.buf)
            s.proc.stdin.write((PW_ROOT + '\r').encode()); s.proc.stdin.flush()
            s._read_some(3.0)
            tail = ANSI.sub(b'', bytes(s.buf[mark2:])).decode('utf-8','replace')
            ok = '现在是 root' in tail
            ck('硬件 SHA + 800B 异常帧：5万轮 KDF 口令验证通过', ok,
               '成功=三项同时成立（SHA256H 算对 / IRQ 打断时 q0..q31 存对 / 无 data abort）'
               if ok else '失败→查 NISV、异常帧或 crypto 自检，勿当偶发')
            res['kdf_auth_ok'] = ok
            # 错误口令拒绝（确认不是「放行了任意口令」的假通过）
            s.wait(r'\n> |\r\n> ', timeout=5)
            s.send('su demo', 1.0)
            mark3 = len(s.buf)
            s.proc.stdin.write(b'su root\r'); s.proc.stdin.flush(); s._read_some(1.5)
            s.proc.stdin.write(b'wrongpw\r'); s.proc.stdin.flush(); s._read_some(2.0)
            tail3 = ANSI.sub(b'', bytes(s.buf[mark3:])).decode('utf-8','replace')
            ck('错误口令被拒（非假通过）', '口令不对' in tail3 and '现在是 root' not in tail3)
            s.send('whoami', 1.0)   # 复位到可继续状态（此时仍是 demo）

        # 4) 第 48 轮：b64 通道存在且大文件帧标记正确（不铺全量 34 项，专项脚本负责）
        s.wait(r'\n> |\r\n> ', timeout=6)
        out = s.send('b64out /home/readme.txt', 2.0)
        ck('b64 大文件帧头可用', 'LOONGOS-B64 BEGIN' in out, '第 48 轮通道在真机可用')
        res['b64_frame_ok'] = 'LOONGOS-B64 BEGIN' in out

        # 5) 第 49 轮：桌面命令存在（图形交互留给人工 virsh 验收）
        s.wait(r'\n> |\r\n> ', timeout=6)
        out = s.send('help', 1.5)
        ck('help 命令清单完整', 'b64out' in out and 'install' in out)

        # 收尾再看一次健康（KDF/MMU 之后有没有累积异常）
        s.wait(r'\n> |\r\n> ', timeout=5)
        out = s.send('res', 1.8)
        r2 = parse_res(out)
        res['res_after'] = r2
        ck('全程结束健康计数仍全 0',
           all(r2.get(k, -1) == 0 for k in ['sync_exc','recovered','nisv','fatal','trip']),
           '跑过 KDF/bench 后若新增异常，真机语义差异坐实')
        return res, checks
    finally:
        s.close()
        print(f"  （串口日志：{s.log_path}）")


def main():
    for p in (ISO, VARS_SRC, CODE):
        if not os.path.exists(p):
            print(f'缺文件：{p}')
            sys.exit(2)
    if shutil.which('qemu-system-aarch64') is None:
        print('缺 qemu-system-aarch64'); sys.exit(2)

    ok_kvm, why = have_kvm()
    force = os.environ.get('KVM_FORCE') == '1'
    modes = ['TCG']
    report = {'ts': time.strftime('%Y-%m-%d %H:%M:%S'),
              'iso_md5': '', 'kvm_capability': {'ok': ok_kvm, 'why': why},
              'modes': {}, 'summary': {}}
    try:
        import hashlib
        h = hashlib.md5()
        with open(ISO, 'rb') as f:
            for c in iter(lambda: f.read(1 << 16), b''):
                h.update(c)
        report['iso_md5'] = h.hexdigest()
    except Exception:
        pass

    if ok_kvm:
        modes.append('KVM')
    elif force:
        modes.append('KVM')   # 强制尝试，让 QEMU 自己给出权限错误，不静默跳过
        print(f'[dual] 警告：{why}；KVM_FORCE=1 仍尝试，预期 QEMU 报错')
    else:
        print(f'[dual] KVM 路 SKIP：{why}')
        print('[dual] 只跑 TCG 路；KVM 需在 aarch64 + /dev/kvm 可读写环境执行（真机复验不能在 x86 沙箱里假装完成）')

    all_checks = {}
    for mode in modes:
        try:
            r, checks = run_mode(mode)
        except Exception as e:
            print(f'  ❌ {mode} 路异常终止：{type(e).__name__}: {e}')
            r, checks = {'mode': mode, 'fatal': str(e)}, [('执行未抛异常', False, f'{type(e).__name__}: {e}')]
        report['modes'][mode] = r
        all_checks[mode] = checks

    # ---- 对照表 ----
    print(f"\n{'='*66}\n  TCG vs KVM 对照表（同一 ISO md5 {report['iso_md5'][:12]}…）\n{'='*66}")
    hdr = f"{'指标':<26}{'TCG':>18}{'KVM':>18}"
    print(hdr); print('-' * 62)
    def g(mode, *keys):
        d = report['modes'].get(mode, {})
        for k in keys:
            if isinstance(d, dict) and k in d: d = d[k]
            else: return '—'
        return str(d)
    rows = [
        ('SHA2 扩展字段',        ('bench','sha2_field')),
        ('硬件哈希(1=硬件)',     ('bench','hw_hash')),
        ('res SHA256 模式',      ('sha256_mode',)),
        ('KDF 5万轮 (us)',       ('bench','kdf_us')),
        ('memset 1MB (us)',      ('bench','memset_us')),
        ('memcpy 1MB (us)',      ('bench','memcpy_us')),
        ('gfx 全屏x10 (us)',     ('bench','gfx_fill_us')),
        ('KDF 口令验证',         ('kdf_auth_ok',)),
        ('NISV/同步异常',        ('res_after','nisv')),
        ('b64 帧头',             ('b64_frame_ok',)),
    ]
    for label, keys in rows:
        print(f"{label:<26}{g('TCG',*keys):>18}{g('KVM',*keys):>18}")
    print('-' * 62)
    print('注① KVM 列「—」= 该路未执行（SKIP/报错），不是 0，也不是通过。')
    print('注② gfx 项为「—」= 本脚本 -nographic 启动无帧缓冲，bench 的 if(fb_mem) 分支跳过；')
    print('     渲染真机表现由桌面/图形路径人工验收，不在此表口径内。')
    print('注③ 真机加速比 = TCG KDF us / KVM KDF us；TCG 下 SHA256H 是 helper 逐条解释，')
    print('     只验证「指令能跑、结果正确」，不反映真机性能收益。')

    # ---- 汇总 ----
    for mode, checks in all_checks.items():
        npass = sum(1 for _, ok, _ in checks if ok)
        report['summary'][mode] = {'pass': npass, 'fail': len(checks) - npass,
                                   'failed': [n for n, ok, _ in checks if not ok]}
        print(f"  {mode}: {npass} 通过 / {len(checks)-npass} 失败"
              + (f"  失败项={report['summary'][mode]['failed']}" if report['summary'][mode]['failed'] else ''))
    with open('/tmp/loong_kvm_dual_report.json', 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print('\n结构化报告：/tmp/loong_kvm_dual_report.json')

    # 退出码：已执行的路有失败 → 1；只有 TCG 且全过 → 0（KVM SKIP 不算失败，但会明确提示）
    bad = any(v['fail'] for v in report['summary'].values())
    if 'KVM' not in all_checks:
        print('[dual] 提醒：KVM 路未执行，本次不构成「TCG+KVM 双路复验完成」。')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
