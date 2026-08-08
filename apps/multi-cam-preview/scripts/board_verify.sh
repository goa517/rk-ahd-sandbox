#!/usr/bin/env bash
# 板上验证：节点探测、单路抓帧、camd 服务状态、编码负载观测
set -euo pipefail

BOARD="${BOARD:-root@192.168.3.173}"

ssh "$BOARD" bash -s <<'EOF'
set -e
echo "===== 摄像头节点 ====="
for n in /dev/video22 /dev/video31; do
  if [ -e "$n" ]; then echo "[OK] $n 存在"; else echo "[--] $n 不存在"; fi
done

echo
echo "===== 单路抓帧冒烟测试（NV12 1280x720，各 30 帧） ====="
for n in /dev/video22 /dev/video31; do
  if [ -e "$n" ]; then
    if v4l2-ctl -d "$n" --set-fmt-video=width=1280,height=720,pixelformat=NV12 \
        --stream-mmap=16 --stream-count=30 --stream-to=/dev/null 2>/dev/null; then
      echo "[OK] $n 30 帧采集成功"
    else
      echo "[FAIL] $n 采集失败"
    fi
  fi
done

echo
echo "===== camd 服务状态 ====="
systemctl --no-pager status camd 2>/dev/null | head -6 || echo "camd 服务未安装"

echo
echo "===== HTTP 接口冒烟 ====="
curl -s --max-time 3 http://127.0.0.1:8080/api/channels | head -c 600 || echo "接口无响应"
echo

echo
echo "===== rkvenc 内核计数（编码活动观测） ====="
dmesg | grep -iE "rkvenc" | tail -3 || true
ls /sys/kernel/debug/mpp_service/ 2>/dev/null || echo "mpp debugfs 未挂载"
EOF
