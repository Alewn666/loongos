#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 LoongOS 的「开发文档 + 宣传PPT + 判据/交付报告 + 同步对账」类资料
归到 NAS 集中资料目录 /vol1/1000/资料/LoongOS/ 下，按子目录分类。

- 凭据从 /root/.loongos/nas_cred 读取，端点从历史会话指纹恢复（无明文、不回显）。
- md5 对账：相同跳过、不同覆盖写（fnOS 的 rename 不支持覆盖）。
- 原文件保留（归档 = 复制），不改动项目代码目录结构。
"""
import os, sys, glob, hashlib, json, re, paramiko

CRED = "/root/.loongos/nas_cred"
TPL  = "/root/.codebuddy/projects/workspace/2f8a7d30-9c21-4f0b-1d67-1ffc13c9c000.jsonl"
FP   = {"HOST": "17819a", "PORT": "b9503b"}
BASE = "/vol1/1000/资料/LoongOS"
WS   = "/workspace"

# ---- 凭据 ----
cred = {}
for line in open(CRED, encoding="utf-8"):
    if "=" in line:
        k, v = line.rstrip("\n").split("=", 1); cred[k.strip()] = v
if not cred.get("NAS_USER") or not cred.get("NAS_PASS"):
    print("[docs] 凭据缺失"); sys.exit(2)

# ---- 端点恢复（SHA256 指纹，无明文）----
vp = re.compile(r"NAS_(HOST|PORT)\s*=\s*(?:'([^']*)'|\"([^\"]*)\"|([^\s;'\\\"]+))")
vals = {}
def walk(o):
    if isinstance(o, str):
        if "sync_nas.py" in o and "NAS_PASS" in o:
            for m in vp.finditer(o):
                k = m.group(1); v = m.group(2) or m.group(3) or m.group(4)
                if hashlib.sha256(v.encode()).hexdigest().startswith(FP[k]):
                    vals[k] = v
    elif isinstance(o, dict):
        for x in o.values(): walk(x)
    elif isinstance(o, list):
        for x in o: walk(x)
for line in open(TPL, encoding="utf-8", errors="replace"):
    try: walk(json.loads(line))
    except Exception: walk(line)
if not all(k in vals for k in FP):
    print("[docs] 端点恢复失败"); sys.exit(3)

# ---- 收集文件（本地绝对路径 -> 远端子目录）----
DOCROOT = os.path.join(WS, "安卓/loongos")
items = []  # (local_abs, remote_rel)

def add(pattern, subdir):
    for p in sorted(glob.glob(pattern)):
        if os.path.isfile(p):
            items.append((p, os.path.join(subdir, os.path.basename(p))))

# 01 宣传PPT
add(os.path.join(WS, "LoongOS宣传演示/*.pptx"), "01_宣传PPT")
add(os.path.join(WS, "LoongOS开发者指南_*.pptx"), "01_宣传PPT")
# 02 开发文档（核心文档 + 各 TODO + README/说明）
for name in ["LoongOS开发者指南.md", "桌面系统_架构与路线.md", "内核空间预算与扩容指引.md",
             "系统资源调配.md", "安全审计报告.md", "检查与修复记录.md",
             "检查与修复记录_第20轮_virtio-scsi磁盘适配.md", "演进路线_逐项状态.md", "OS_DEV_TODO.md"]:
    add(os.path.join(DOCROOT, "本体/docs", name), "02_开发文档")
for p in sorted(glob.glob(os.path.join(DOCROOT, "本体/LOONGOS_*TODO.md"))):
    items.append((p, os.path.join("02_开发文档", os.path.basename(p))))
# 同名 README/说明 加前缀，避免远端 basename 冲突互相覆盖
def add_as(local, rr):
    if os.path.isfile(local):
        items.append((local, rr))
add_as(os.path.join(DOCROOT, "README.md"),           "02_开发文档/README_主项目.md")
add_as(os.path.join(DOCROOT, "手机版/说明.md"),       "02_开发文档/说明_手机版.md")
add_as(os.path.join(DOCROOT, "loong-repo/README.md"), "02_开发文档/README_loong-repo.md")
add_as(os.path.join(DOCROOT, "x86/README.md"),        "02_开发文档/README_x86.md")
# 03 判据报告（各轮 json）
add(os.path.join(DOCROOT, "本体/docs/*.json"), "03_判据报告")
# 04 交付报告（顶层 + docs 内）
add(os.path.join(WS, "LoongOS_*交付报告.md"), "04_交付报告")
add(os.path.join(DOCROOT, "本体/docs/*交付报告.md"), "04_交付报告")
# 05 同步对账
add(os.path.join(WS, "nas_sync_对账_*.txt"), "05_同步对账")

# ---- 连接 ----
c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(vals["HOST"], port=int(vals["PORT"]), username=cred["NAS_USER"],
          password=cred["NAS_PASS"], timeout=15, look_for_keys=False, allow_agent=False)
sftp = c.open_sftp()

def md5_local(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for ch in iter(lambda: f.read(65536), b""): h.update(ch)
    return h.hexdigest()

def md5_remote(rp):
    try:
        with sftp.open(rp, "rb") as f:
            h = hashlib.md5()
            for ch in iter(lambda: f.read(65536), b""): h.update(ch)
        return h.hexdigest()
    except IOError:
        return None

def mkdir_p(d):
    cur = ""
    for part in d.split("/"):
        if not part: cur = "/"; continue
        cur = cur + part if cur == "/" else cur + "/" + part
        try: sftp.stat(cur)
        except IOError:
            try: sftp.mkdir(cur)
            except IOError: pass

print(f"[docs] -> <连接已隐藏>  BASE={BASE}")
print(f"[docs] 共收集 {len(items)} 份资料")
ok = skip = fail = 0
for lp, rr in items:
    rp = BASE + "/" + rr
    if not os.path.exists(lp):
        print(f"  ! 本地缺失: {rr}"); fail += 1; continue
    lm = md5_local(lp)
    if md5_remote(rp) == lm:
        skip += 1; continue
    mkdir_p(os.path.dirname(rp))
    try:
        with sftp.open(rp, "wb") as f, open(lp, "rb") as src:
            while True:
                ch = src.read(262144)
                if not ch: break
                f.write(ch)
        print(f"  ✓ {rr} ({os.path.getsize(lp)//1024}KB)")
        ok += 1
    except Exception as e:
        print(f"  ✗ 失败 {rr}: {type(e).__name__}: {e}"); fail += 1
sftp.close(); c.close()
print(f"[docs] 完成: 新增/更新 {ok} · 相同跳过 {skip} · 失败 {fail}")
sys.exit(0 if fail == 0 else 2)
