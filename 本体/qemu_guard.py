#!/usr/bin/env python3
"""QEMU 测试的宿主资源护栏（2026-10-25 加）。

背景：今早在 NAS（2GB 内存、少核）上并发跑多个测试虚拟机，把宿主拖死过一次。
所有会起 QEMU 的脚本都应该在启动前调用 preflight()：

    from qemu_guard import preflight, release
    if not preflight(need_mb=500):
        sys.exit(2)
    try:
        ... 起 QEMU、跑测试 ...
    finally:
        release()

三条规矩：
  1. 可用内存 < need_mb → 拒绝运行（宁可测试不跑，也不让宿主进 swap 抖动）
  2. 同一时刻只允许一个测试 VM（/tmp/loongos_qemu.lock 上的 flock）
  3. 调用方必须 finally: release()（锁释放；QEMU 进程由调用方自己 kill）

环境变量：
  LOONGOS_GUARD_OFF=1  跳过护栏（明确知道自己在干什么时用；默认永不设置）
"""
import fcntl
import os
import sys

LOCK_PATH = "/tmp/loongos_qemu.lock"
_lock_file = None


def available_mb():
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 10 ** 6          # 读不到就当作内存充足（非 Linux 环境）


def preflight(need_mb=500, label=""):
    """返回 True 表示可以起 QEMU。失败时已打印原因。"""
    global _lock_file
    if os.environ.get("LOONGOS_GUARD_OFF") == "1":
        print("[guard] ⚠️ 护栏被 LOONGOS_GUARD_OFF=1 关闭，自行确保不会拖垮宿主")
        return True

    avail = available_mb()
    if avail < need_mb:
        print("[guard] ✗ 拒绝运行%s：可用内存 %d MB < 需要 %d MB（保护 NAS，别把它拖进 swap）"
              % (("：" + label) if label else "", avail, need_mb))
        return False

    try:
        _lock_file = open(LOCK_PATH, "w")
        fcntl.flock(_lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("[guard] ✗ 拒绝运行%s：已有另一个测试 VM 在跑（%s 被占用）"
              % (("：" + label) if label else "", LOCK_PATH))
        return False
    except OSError as e:
        print("[guard] ⚠️ 锁文件不可用（%s），继续但请勿并发" % e)
        _lock_file = None

    print("[guard] ✓ 可用内存 %d MB（需要 %d MB），单实例锁已拿%s"
          % (avail, need_mb, ("：" + label) if label else ""))
    return True


def release():
    global _lock_file
    if _lock_file is not None:
        try:
            fcntl.flock(_lock_file, fcntl.LOCK_UN)
            _lock_file.close()
        except OSError:
            pass
        _lock_file = None
