#!/usr/bin/env bash
# 交叉编译内建 xs9922 的内核 Image 与 CAM0 overlay dtbo，并做离线校验。
#
# 前置：kernel/scripts/prepare_tree.sh && kernel/scripts/apply_to_tree.sh
# 产物：kernel/out/{Image,*.dtbo,build.log,merged.dts}
#
# 关键点：必须 export LOCALVERSION=（置为空串而非不设置），否则 scripts/setlocalversion
# 会因为工作树 dirty 给内核版本号追加 "+"，得到 6.1.99-rk3576+，vermagic 与板端
# 6.1.99-rk3576 不符，insmod 会被拒。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TREE="${KERNEL_TREE:-$REPO_ROOT/references/lubancat3/kernel-6.1}"
OUT="$REPO_ROOT/kernel/out"

OVERLAY_NAME="rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay"
DTS_DIR="arch/arm64/boot/dts/rockchip"
OVERLAY_DIR="$DTS_DIR/overlay"
BASE_DTB="$DTS_DIR/rk3576-lubancat-3.dtb"
# 顶层 Makefile 的 %.dtb/%.dtbo 规则会自动加上 arch/$ARCH/boot/dts/ 前缀，
# 所以 make 目标要写成 rockchip/... 这样的相对路径，而不是完整路径
DT_TARGETS="rockchip/overlay/$OVERLAY_NAME.dtbo rockchip/rk3576-lubancat-3.dtb"
EXPECT_KREL="6.1.99-rk3576"
JOBS="${JOBS:-$(nproc)}"

export ARCH=arm64
export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
export LOCALVERSION=

[ -f "$TREE/drivers/media/i2c/xs9922.c" ] || { echo "ERROR: 内核树里没有 xs9922.c，请先跑 apply_to_tree.sh" >&2; exit 1; }

mkdir -p "$OUT"
cd "$TREE"

# ------------------------------------------------------------------ 配置

if [ ! -f .config ] || [ "${RECONFIG:-0}" = 1 ]; then
    echo "== make lubancat_linux_rk3576_defconfig"
    make -s lubancat_linux_rk3576_defconfig
fi
# 只改生成的 .config，不动官方 defconfig 文件。
# 注意：必须是 =y 内建，不能 =m。模块加载太晚（~8s），csi2-dcphy0 的
# fwnode parse（phy-rockchip-csi2-dphy.c 的 GKI 检查）在 ~4s 时就会因为
# sensor 驱动未绑定而永久跳过 xs9922 端点，异步匹配链断裂、sensor 永远
# 进不了 media 拓扑。内建后 i2c 驱动 late_initcall 先于 dphy probe 完成
# 绑定，链路才能建起来。详见 docs/xs9922b_port.md。
grep -q '^CONFIG_VIDEO_XS9922=y$' .config || {
    echo "== scripts/config --enable CONFIG_VIDEO_XS9922"
    ./scripts/config --enable CONFIG_VIDEO_XS9922
    make -s olddefconfig
}
grep -q '^CONFIG_VIDEO_XS9922=y$' .config \
    || { echo "ERROR: CONFIG_VIDEO_XS9922 没有变成 y（依赖 I2C && VIDEO_DEV 是否满足？）" >&2; exit 1; }

# ------------------------------------------------------------------ 内核镜像（驱动内建）

# 先单独用 W=1 编 xs9922.o 做告警检查（整树 W=1 会让 BSP 代码大量 -Werror）
echo "== make W=1 drivers/media/i2c/xs9922.o（告警检查）"
rm -f drivers/media/i2c/xs9922.o
make W=1 -j"$JOBS" drivers/media/i2c/xs9922.o 2>&1 | tee "$OUT/build.log"
if grep -nE 'xs9922[^ ]*\.[ch]:[0-9]+:[0-9]+: (warning|error):' "$OUT/build.log"; then
    echo "ERROR: xs9922 有编译告警/错误（见上）" >&2
    exit 1
fi
echo "   [ok]   xs9922 W=1 零告警"

echo "== make Image -j$JOBS"
make -s -j"$JOBS" Image

cp arch/arm64/boot/Image "$OUT/Image"

# 内核 release 必须与板端 uname -r 一致（vmlinux 内嵌版本串）。
# 注意：set -o pipefail 下不能用 grep -m1/-q 提前退出（上游 SIGPIPE 会判失败），
# awk/sed 必须读完整个输入
KREL="$(strings vmlinux | awk '/^Linux version/ && !k {k=$3} END {print k}')"
echo "== kernel release: $KREL"
[ "$KREL" = "$EXPECT_KREL" ] \
    || { echo "ERROR: release 不是 $EXPECT_KREL（检查 LOCALVERSION= 是否导出）" >&2; exit 1; }
[ "$(nm vmlinux | grep -c xs9922_probe)" -gt 0 ] \
    || { echo "ERROR: vmlinux 里没有 xs9922 符号" >&2; exit 1; }
echo "   [ok]   xs9922 已内建进 vmlinux"

# ------------------------------------------------------------------ 设备树

echo "== make $OVERLAY_NAME.dtbo + 基础 dtb"
make -s -j"$JOBS" $DT_TARGETS
cp "$OVERLAY_DIR/$OVERLAY_NAME.dtbo" "$OUT/"

# 离线复现 U-Boot 的 dtoverlay 应用过程
echo "== fdtoverlay 合并校验"
fdtoverlay -i "$BASE_DTB" -o "$OUT/merged.dtb" "$OUT/$OVERLAY_NAME.dtbo"
dtc -q -I dtb -O dts "$OUT/merged.dtb" > "$OUT/merged.dts"

check() { # <描述> <grep 模式> ...
    local desc="$1"; shift
    if "$@" > /dev/null; then echo "   [ok]   $desc"; else echo "   [FAIL] $desc"; FAILED=1; fi
}
FAILED=0
check "i2c3 下出现 xs9922-0@31"      grep -q 'xs9922-0@31' "$OUT/merged.dts"
check "endpoint@9 挂上 dcphy0"        grep -q 'endpoint@9' "$OUT/merged.dts"
check "es8388@11 未被破坏"            grep -q 'es8388@11' "$OUT/merged.dts"
[ "$FAILED" = 0 ] || { echo "ERROR: 合并结果不符合预期，详见 $OUT/merged.dts" >&2; exit 1; }

echo "== OK，产物在 $OUT/"
ls -l "$OUT"
