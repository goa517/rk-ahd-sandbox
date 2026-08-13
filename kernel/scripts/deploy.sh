#!/usr/bin/env bash
# 把 kernel/out/ 里的内建内核 Image 与 CAM0 overlay 部署到开发板。
#
#   Image → /boot/Image-<krel>-builtin，并把 /boot/Image 符号链接切过去
#           （原厂 Image-<krel> 文件保留，随时可指回回退）
#   .dtbo → /boot/dtb/overlay/
#   /boot/uEnv/uEnv.txt 的 #overlay_start..#overlay_end 之间启用对应 dtoverlay= 行
#
# 注意：驱动必须内建（=y），不能走模块。csi2-dcphy0 的 fwnode parse 有
# GKI 检查，只在 sensor 驱动已绑定时才把端点加入异步匹配列表；模块加载
# 太晚会导致 xs9922 永远进不了 media 拓扑。详见 docs/xs9922b_port.md。
set -euo pipefail

BOARD="${BOARD:-root@192.168.8.198}"
OUT="$(cd "$(dirname "$0")/.." && pwd)/out"

OVERLAY_NAME="rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay"
IMAGE="$OUT/Image"
DTBO="$OUT/$OVERLAY_NAME.dtbo"
DTOVERLAY_LINE="dtoverlay=/dtb/overlay/$OVERLAY_NAME.dtbo"

for f in "$IMAGE" "$DTBO"; do
    [ -f "$f" ] || { echo "ERROR: 缺少 $f，请先跑 kernel/scripts/build.sh" >&2; exit 1; }
done

# ---------------------------------------------------------------- 版本校验

BOARD_KREL="$(ssh "$BOARD" 'uname -r')"
NEW_IMAGE="/boot/Image-$BOARD_KREL-builtin"

echo "==> 板端 uname -r：$BOARD_KREL"
# build.sh 已校验 Image 的 release 段，这里只防止部署到版本不符的板子
# （板端 uname -r 就是当前运行的内核版本，新 Image 与它同 release 才能保证
#  /lib/modules 匹配、驱动行为一致）

# ---------------------------------------------------------------- 内核 Image

echo "==> 安装 Image 到 $NEW_IMAGE 并切换 /boot/Image 链接"
scp -q "$IMAGE" "$BOARD:$NEW_IMAGE"
ssh "$BOARD" "set -e
    md5sum $NEW_IMAGE
    # 原厂 Image 保留为 Image-<krel>，不覆盖；回退方法：
    #   ln -sf Image-$BOARD_KREL /boot/Image
    ln -sf $(basename $NEW_IMAGE) /boot/Image
    # 清理模块形态的历史安装（extra 里的 .ko 会遮蔽内建驱动）
    rm -f /lib/modules/$BOARD_KREL/extra/xs9922.ko
    depmod -a
    ls -l /boot/Image"

# ---------------------------------------------------------------- 设备树

echo "==> 安装 $OVERLAY_NAME.dtbo 到 /boot/dtb/overlay/"
scp -q "$DTBO" "$BOARD:/boot/dtb/overlay/"

echo "==> 更新 /boot/uEnv/uEnv.txt"
# 三种情况：已启用 / 有注释行（取消注释）/ 完全没有（插到 #overlay_end 之前）
ssh "$BOARD" "set -e
    U=/boot/uEnv/uEnv.txt
    grep -q '^#overlay_end' \$U || { echo 'ERROR: uEnv.txt 里找不到 #overlay_end'; exit 1; }
    cp -n \$U \$U.bak.\$(date +%Y%m%d%H%M%S)
    if grep -q '^${DTOVERLAY_LINE//\//\\/}\$' \$U; then
        echo '   已启用，跳过'
    elif grep -q '^#${DTOVERLAY_LINE//\//\\/}\$' \$U; then
        sed -i 's|^#${DTOVERLAY_LINE}\$|${DTOVERLAY_LINE}|' \$U
        echo '   取消注释完成'
    else
        sed -i '/^#overlay_end/i ${DTOVERLAY_LINE}' \$U
        echo '   已插入新行'
    fi
    echo '   --- 当前生效的 overlay ---'
    sed -n '/^#overlay_start/,/^#overlay_end/p' \$U | grep '^dtoverlay=' || true"

# ---------------------------------------------------------------- 收尾

if [ "${REBOOT:-0}" = 1 ]; then
    echo "==> 重启开发板"
    ssh "$BOARD" 'nohup sh -c "sleep 1; reboot" >/dev/null 2>&1 &' || true
    echo "   等板子起来后按 docs/xs9922b_port.md 的 bring-up 检查清单逐项确认。"
else
    echo "==> 完成。新内核 + overlay 需要重启才生效："
    echo "     ssh $BOARD reboot"
    echo "   重启后按 docs/xs9922b_port.md 的 bring-up 检查清单逐项确认。"
fi
