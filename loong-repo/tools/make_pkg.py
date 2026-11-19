#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一个二进制文件打包成 LOONG .lpkg 纯文本容器。

用法:
  python3 make_pkg.py <源文件> <包名> <版本> <安装路径> <输出.lpkg> ["描述"]

例:
  python3 make_pkg.py ../../本体/build/hello.elf hello 1.2.0 /home/hello.elf \
      repo/packages/hello_1.2.0.lpkg "Hello World 演示程序"
"""
import sys, os, base64, hashlib

def main():
    if len(sys.argv) < 6:
        print(__doc__); return 1
    src, name, ver, path, out = sys.argv[1:6]
    desc = sys.argv[6] if len(sys.argv) > 6 else ""
    data = open(src, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    b64 = base64.b64encode(data).decode("ascii")
    # 76 列折行，与内核 b64in 通道一致
    lines = [b64[i:i+76] for i in range(0, len(b64), 76)]
    head = "\n".join([
        "LOONGPKG/1",
        "Name: %s" % name,
        "Version: %s" % ver,
        "Path: %s" % path,
        "Sha256: %s" % sha,
        "Desc: %s" % desc,
        "",
    ])
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        # head 已以空串元素结尾 => 末尾一个 \n；再补一个 \n 形成空行分隔 payload
        f.write(head + "\n" + "\n".join(lines) + "\n")
    print("已生成 %s" % out)
    print("  包 %s %s -> %s" % (name, ver, path))
    print("  payload %d B / sha256 %s" % (len(data), sha[:16] + "..."))
    return 0

if __name__ == "__main__":
    sys.exit(main())
