#!/bin/sh
# NAS 上一键部署 LOONG 软件源（Docker / docker compose）
# 用法: sh deploy.sh [端口]   默认 8080
set -e
cd "$(dirname "$0")"

PORT="${1:-8080}"
echo "== LOONG 软件源部署 =="
echo "端口: $PORT"

# 重新生成索引（确保包与索引一致）
python3 tools/gen_packages.py

# 端口可配置：同步 compose 的映射（简单替换默认 8080）
if [ "$PORT" != "8080" ]; then
    sed -i "s|\"8080:8080\"|\"${PORT}:8080\"|g" docker-compose.yml
fi

# 构建并后台启动
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    docker compose up -d --build
elif command -v docker-compose >/dev/null 2>&1; then
    docker-compose up -d --build
else
    echo "✗ 未找到 docker compose，请先在 NAS 上安装 Container Manager / Docker"
    exit 1
fi

echo ""
echo "== 部署完成 =="
echo "源地址: http://<NAS_IP>:${PORT}/"
echo "索引:   http://<NAS_IP>:${PORT}/Packages"
echo ""
echo "在 LoongOS 里配置并更新:"
echo "  loong sources <NAS_IP> ${PORT}"
echo "  loong update"
echo "  loong list"
