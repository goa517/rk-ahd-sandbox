#!/usr/bin/env bash
# 构建并部署 camd 到开发板（板端原生构建，需要板上有 Go 工具链与 mpp/rga 开发包）
set -euo pipefail

BOARD="${BOARD:-root@192.168.8.198}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_SRC=/root/build/multi-cam-preview
REMOTE_OPT=/opt/multi-cam-preview

echo "==> 同步源码到 $BOARD:$REMOTE_SRC"
ssh "$BOARD" "mkdir -p $REMOTE_SRC"
# 板上无 rsync，用 tar 流传输；先清理旧源码再解包（等价 --delete）
tar czf - --exclude='.git' --exclude='docs' -C "$SRC_DIR" . \
  | ssh "$BOARD" "find $REMOTE_SRC -mindepth 1 -delete && tar xzf - -C $REMOTE_SRC"

echo "==> 板端构建（GOPROXY=goproxy.cn）"
ssh "$BOARD" "cd $REMOTE_SRC && \
  export GOPROXY=https://goproxy.cn,direct && \
  /usr/local/go/bin/go mod tidy && \
  CGO_ENABLED=1 /usr/local/go/bin/go build -trimpath -o camd-bin ./camd/cmd/camd"

echo "==> 安装并重启服务"
ssh "$BOARD" "mkdir -p $REMOTE_OPT && \
  install -m755 $REMOTE_SRC/camd-bin $REMOTE_OPT/camd && \
  if [ ! -f $REMOTE_OPT/config.yaml ]; then install -m644 $REMOTE_SRC/camd/config.yaml $REMOTE_OPT/config.yaml; fi && \
  install -m644 $REMOTE_SRC/scripts/camd.service /etc/systemd/system/camd.service && \
  systemctl daemon-reload && \
  systemctl enable camd >/dev/null 2>&1 || true; \
  systemctl restart camd && \
  sleep 1 && systemctl --no-pager status camd | head -8"

echo "==> 完成。访问 http://192.168.8.198:8080/"
