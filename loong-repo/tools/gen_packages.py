#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LOONG 软件源索引生成器：扫描 repo/packages/*.lpkg，生成 Debian 风格 Packages 索引。

.lpkg 是纯文本容器：
    LOONGPKG/1
    Name: <包名>
    Version: <x.y.z>
    Path: <安装到客户机的绝对路径>
    Sha256: <payload 解码后的 64 位 hex>
    Desc: <一句话描述>
    <空行>
    <base64 payload>

索引每条记录字段与内核 loong.c 的 loong_parse_index 严格对齐：
    Package / Version / Size / File / Sha256 / Description
    （空行分隔；Size = 解包后字节数）
"""
import os, sys, hashlib, base64, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PKGDIR = os.path.join(ROOT, "repo", "packages")
INDEX = os.path.join(ROOT, "repo", "Packages")
RELEASE = os.path.join(ROOT, "repo", "Release")

FIELD_RE = re.compile(r"^(Name|Version|Path|Sha256|Desc):[ \t]*(.*)$")


def parse_lpkg(path):
    """解析一个 .lpkg，返回 (fields dict, payload_bytes) 或 None。"""
    with open(path, "rb") as f:
        data = f.read()
    # 头与 payload 以空行分隔
    sep = data.find(b"\n\n")
    if sep < 0:
        print("  ! %s: 无空行分隔" % os.path.basename(path)); return None
    head = data[:sep].decode("utf-8", "replace")
    payload_b64 = data[sep + 2:]
    lines = head.splitlines()
    if not lines or lines[0].strip() != "LOONGPKG/1":
        print("  ! %s: magic 错误" % os.path.basename(path)); return None
    fields = {}
    for ln in lines[1:]:
        m = FIELD_RE.match(ln)
        if m:
            fields[m.group(1)] = m.group(2).strip()
    try:
        payload = base64.b64decode(payload_b64, validate=False)
    except Exception as e:
        print("  ! %s: base64 解码失败 %s" % (os.path.basename(path), e)); return None
    for k in ("Name", "Version", "Path", "Sha256"):
        if k not in fields:
            print("  ! %s: 缺字段 %s" % (os.path.basename(path), k)); return None
    real_sha = hashlib.sha256(payload).hexdigest()
    if real_sha != fields["Sha256"].lower():
        print("  ! %s: Sha256 不符 (索引 %s / 实际 %s)" %
              (os.path.basename(path), fields["Sha256"][:12], real_sha[:12]))
        return None
    return fields, payload, real_sha


def main():
    if not os.path.isdir(PKGDIR):
        print("包目录不存在:", PKGDIR); return 1
    entries = []
    for fn in sorted(os.listdir(PKGDIR)):
        if not fn.endswith(".lpkg"):
            continue
        print("扫描", fn)
        r = parse_lpkg(os.path.join(PKGDIR, fn))
        if not r:
            continue
        fields, payload, sha = r
        entries.append((fields["Name"], fields["Version"], len(payload),
                        fn, sha, fields.get("Desc", "")))
    # 按包名排序，写 Packages
    entries.sort(key=lambda e: e[0])
    out = []
    for name, ver, size, fn, sha, desc in entries:
        out += [
            "Package: %s" % name,
            "Version: %s" % ver,
            "Size: %d" % size,
            "File: packages/%s" % fn,
            "Sha256: %s" % sha,
            "Description: %s" % desc,
            "",
        ]
    with open(INDEX, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    idx_sha = hashlib.sha256(open(INDEX, "rb").read()).hexdigest()
    with open(RELEASE, "w", encoding="utf-8", newline="\n") as f:
        f.write("Origin: LoongOS LOONG repo\n")
        f.write("Label: LOONG\n")
        f.write("Suite: stable\n")
        f.write("Components: main\n")
        f.write("Architectures: aarch64\n")
        f.write("Date: %s\n" % __import__("datetime").datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"))
        f.write("SHA256: %s  Packages\n" % idx_sha)
    print("\n生成 %s（%d 个包）" % (INDEX, len(entries)))
    print("索引 SHA256:", idx_sha)
    for e in entries:
        print("  %-12s %-8s %7d B  %s" % (e[0], e[1], e[2], e[5]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
