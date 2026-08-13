#!/usr/bin/env bash
# 把 kernel/src/ 叠加到内核工作树，并幂等地插入 Kconfig / Makefile / uEnv 引用行。
#
# 可重复执行：每处插入前先 grep 标记，已存在则跳过。除下面列出的 4 个文件外，
# 不修改内核树的任何既有文件（尤其不改 defconfig，配置项由 build.sh 用
# scripts/config 打到生成的 .config 上）。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$REPO_ROOT/kernel/src"
TREE="${KERNEL_TREE:-$REPO_ROOT/references/lubancat3/kernel-6.1}"

OVERLAY_NAME="rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay"

[ -f "$TREE/Makefile" ] || { echo "ERROR: 内核树不存在，请先跑 prepare_tree.sh：$TREE" >&2; exit 1; }

# ---------------------------------------------------------------- 工具函数

# insert <file> <锚点整行内容> before|after  < 待插入文本
#
# 锚点按「整行完全相等」匹配（不是正则），避免 $( ) . 之类字符还要转义；锚点经
# 环境变量传入而非 awk -v，否则 awk 会吃掉行尾续行符那样的反斜杠。待插入文本走
# stdin + 临时文件，同样不经 awk 转义处理，行尾的 \ 能原样保留。
insert() {
    local file="$1" anchor="$2" where="$3" blk
    blk="$(mktemp)"; cat > "$blk"
    ANCHOR="$anchor" awk -v where="$where" -v blk="$blk" '
        BEGIN { anchor = ENVIRON["ANCHOR"] }
        function emit() { while ((getline l < blk) > 0) print l; close(blk) }
        !done && $0 == anchor {
            if (where == "before") { emit(); print } else { print; emit() }
            done = 1; next
        }
        { print }
        END { if (!done) exit 3 }
    ' "$file" > "$file.tmp" || { rm -f "$blk" "$file.tmp"; echo "ERROR: 在 $file 中找不到锚点行：$anchor" >&2; exit 1; }
    mv "$file.tmp" "$file"; rm -f "$blk"
}

# ---------------------------------------------------------------- 1. 源码

echo "== rsync $SRC/ → $TREE/"
rsync -a --checksum \
      "$SRC/drivers/" "$TREE/drivers/"
rsync -a --checksum \
      "$SRC/arch/" "$TREE/arch/"

# ---------------------------------------------------------------- 2. Kconfig

KCONFIG="$TREE/drivers/media/i2c/Kconfig"
if grep -q 'VIDEO_XS9922' "$KCONFIG"; then
    echo "== Kconfig: 已存在 VIDEO_XS9922，跳过"
else
    echo "== Kconfig: 在 VIDEO_NVP6324 之前插入 VIDEO_XS9922"
    insert "$KCONFIG" 'config VIDEO_NVP6324' before <<'EOF'
config VIDEO_XS9922
	tristate "Xinshi xs9922 driver support"
	depends on I2C && VIDEO_DEV
	select MEDIA_CONTROLLER
	select VIDEO_V4L2_SUBDEV_API
	help
	  Support for the Xinshi XS9922B 4 channel AHD/CVBS to MIPI CSI-2
	  bridge.  The four analog inputs are exposed as CSI-2 virtual
	  channels 0..3 on a single 4-lane link.

	  To compile this driver as a module, choose M here: the
	  module will be called xs9922.

EOF
fi

# ---------------------------------------------------------------- 3. Makefile

MK="$TREE/drivers/media/i2c/Makefile"
if grep -q 'CONFIG_VIDEO_XS9922' "$MK"; then
    echo "== drivers/media/i2c/Makefile: 已存在，跳过"
else
    echo "== drivers/media/i2c/Makefile: 追加 xs9922.o（保持字母序，wm8775 之后）"
    insert "$MK" 'obj-$(CONFIG_VIDEO_WM8775) += wm8775.o' after \
        <<< 'obj-$(CONFIG_VIDEO_XS9922) += xs9922.o'
fi

# ---------------------------------------------------------------- 4. overlay Makefile

OMK="$TREE/arch/arm64/boot/dts/rockchip/overlay/Makefile"
if grep -q "$OVERLAY_NAME" "$OMK"; then
    echo "== overlay/Makefile: 已存在，跳过"
else
    echo "== overlay/Makefile: 在 rk3576 段的 cam4-gc4653 之后追加 dtbo"
    insert "$OMK" '	rk3576-lubancat-3-cam4-gc4653-2560x1440-30fps-overlay.dtbo \' after \
        <<< "	$OVERLAY_NAME.dtbo \\"
fi

# ---------------------------------------------------------------- 5. uEnv 模板

UENV="$TREE/arch/arm64/boot/dts/rockchip/uEnv/rk3576/uEnvLubanCat3.txt"
if grep -q "$OVERLAY_NAME" "$UENV"; then
    echo "== uEnvLubanCat3.txt: 已存在，跳过"
else
    echo "== uEnvLubanCat3.txt: 在 CAM0 段追加（默认注释掉）"
    insert "$UENV" '#dtoverlay=/dtb/overlay/rk3576-lubancat-3-cam0-gc4653-2560x1440-30fps-overlay.dtbo' after \
        <<< "#dtoverlay=/dtb/overlay/$OVERLAY_NAME.dtbo"
fi

echo "== OK"
echo "   下一步：kernel/scripts/build.sh"
