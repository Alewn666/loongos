# LOONG 软件源 —— 极简静态 HTTP 服务
# 托管 Packages / Release 索引与 packages/*.lpkg 包，供 LoongOS 的 `loong update/install` 拉取。
# 用 python 标准库 http.server，零第三方依赖，镜像小、好审计。
FROM python:3.11-alpine

LABEL org.opencontainers.image.title="LOONG Repo" \
      org.opencontainers.image.description="Static HTTP package repository for LoongOS"

WORKDIR /srv/loong-repo
COPY repo/ /srv/loong-repo/

# 只读静态服务：Python 自带 http.server，绑定 0.0.0.0:8080
EXPOSE 8080

# -d 指定目录，-b 绑定地址；目录索引关闭（我们只提供明确文件）
CMD ["python3", "-m", "http.server", "8080", "--bind", "0.0.0.0", "--directory", "/srv/loong-repo"]
