# LOONG 软件源（LoongOS 包管理器服务端）

这是 LOONG 包管理器的**软件源**,以 Docker 容器形式部署在你的 **NAS** 上,给 LoongOS 系统提供 `loong update` / `loong install` 的软件包的下载服务。

- 客户端:LoongOS 内核里的 `loong` 命令(走 TCP/HTTP GET)。
- 服务端:本目录下的容器,一个零依赖的静态 HTTP 服务,托管 `Packages` 索引和 `.lpkg` 包。

## 目录结构

```
loong-repo/
├── Dockerfile              # 容器镜像(python:3.11-alpine + http.server)
├── docker-compose.yml      # compose 部署(默认宿主 8080:容器 8080)
├── deploy.sh               # NAS 上一键部署脚本
├── README.md
├── repo/                   # 源数据(容器内 /srv/loong-repo)
│   ├── Packages            # 包索引(Debian 风格),由 gen_packages.py 生成
│   ├── Release             # 索引校验信息
│   └── packages/*.lpkg     # 包文件(纯文本容器,内含 base64 payload)
└── tools/
    ├── make_pkg.py         # 把一个二进制文件打成 .lpkg
    └── gen_packages.py     # 扫描 packages/ 生成 Packages + Release
```

## 在 NAS 上部署(推荐 docker compose)

1. 把整个 `loong-repo/` 目录拷到 NAS,例如 `/volume1/docker/loong-repo/`。
2. 进入目录,执行一键部署(可指定端口):

   ```sh
   cd /volume1/docker/loong-repo
   sh deploy.sh 8080
   ```

   脚本会:重新生成索引 → `docker compose up -d --build` 启动容器。

   也可以手动:
   ```sh
   python3 tools/gen_packages.py
   docker compose up -d --build
   ```

3. 确认源可访问:浏览器打开 `http://<NAS_IP>:8080/Packages`,应能看到包索引。

> 群晖 DSM 用 Container Manager(套件中心装)或直接 `docker compose`;威联通用 Container Station。若 8080 被占用,`deploy.sh 9000` 换端口即可(会自动改 compose 映射)。

## 在 LoongOS 里使用

进 shell 后:

```
loong sources <NAS_IP> 8080     # 设置源地址(只需一次)
loong update                     # 拉取索引
loong list                       # 列出可用包(每包只显示最高版本)
loong show hello                 # 查看包详情
loong search sched               # 搜索
loong install hello              # 下载+SHA256校验+落盘+记账
loong status                     # 查看源/索引/统计/已装列表
```

示例:安装后 `run /home/hello.elf`,会在 EL0 跑起 Hello World。

## 发布新包

1. 打包(以 hello.elf 为例):
   ```sh
   python3 tools/make_pkg.py ../本体/build/hello.elf hello 3.0.0 /home/hello.elf \
       repo/packages/hello_3.0.0.lpkg "Hello World v3"
   ```
2. 重新生成索引并重启容器:
   ```sh
   python3 tools/gen_packages.py
   docker compose restart
   ```
3. 客户端 `loong update` 后即可看到新版本,`loong install hello` 会装**最高版本**。

`make_pkg.py` 会自动算 payload 的 SHA256 写进 `.lpkg` 头;`gen_packages.py` 会校验包的 SHA256 与 payload 一致后才写入索引,不一致的包会被跳过并报错——防止损坏的包进源。

## 包格式 `.lpkg`(纯文本)

```
LOONGPKG/1
Name: hello
Version: 2.0.0
Path: /home/hello.elf
Sha256: <payload 的 64 位 hex>
Desc: Hello World demo

<base64 payload,76 列折行>
```

客户端安装流程:解析头 → base64 解码 payload → **用索引里的 Sha256 校验** → 按 Path 落盘 → 记账。SHA256 不符会拒绝安装。

## 安全边界(诚实说明)

- 走**明文 HTTP**(教学 TCP 栈无 TLS),仅适合可信内网(你的 NAS 局域网)。不要暴露到公网。
- 容器 `read_only: true`,只提供静态文件,无写接口。
- 客户端 TCP 是教学实现(单连接、无乱序重组),已针对 SLIRP 场景修复 ACK 误判与零窗口死锁,并在 QEMU TCG 下端到端验证;真机网络环境未逐一实测。
