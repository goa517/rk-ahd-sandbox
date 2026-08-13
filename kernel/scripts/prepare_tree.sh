#!/usr/bin/env bash
# 从本地 repo 镜像检出鲁班猫 kernel-6.1 工作树（无需联网）。
#
# 镜像位于 references/lubancat3/.repo/projects/kernel-6.1.git（gitdir，objects 符号链接到
# ../../project-objects/kernel.git/objects）。检出的 revision 与板端运行内核一致：
#   6.1.99-rk3576 / c9df0f900d445bb911149632e27b591958939e1b
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MIRROR="$REPO_ROOT/references/lubancat3/.repo/projects/kernel-6.1.git"
TREE="${KERNEL_TREE:-$REPO_ROOT/references/lubancat3/kernel-6.1}"
REV="c9df0f900d445bb911149632e27b591958939e1b"

[ -d "$MIRROR" ] || { echo "ERROR: 找不到内核镜像 $MIRROR" >&2; exit 1; }

if [ -d "$TREE/.git" ]; then
    echo "== 工作树已存在：$TREE"
else
    echo "== 克隆 $MIRROR → $TREE"
    # 本地路径克隆默认走 hardlink，磁盘占用小且克隆后自包含（不依赖 alternates）
    git clone --quiet --no-checkout "$MIRROR" "$TREE"
fi

cd "$TREE"
CUR="$(git rev-parse HEAD 2>/dev/null || echo none)"
if [ "$CUR" != "$REV" ] || [ ! -f Makefile ]; then
    echo "== 检出 $REV"
    git checkout --quiet --detach "$REV"
fi

# 校验：必须是 6.1.99，否则 vermagic 与板端不符，.ko 无法加载
V=$(sed -n 's/^VERSION *= *//p;'  Makefile | head -1)
P=$(sed -n 's/^PATCHLEVEL *= *//p' Makefile | head -1)
S=$(sed -n 's/^SUBLEVEL *= *//p'   Makefile | head -1)
echo "== 内核版本：$V.$P.$S  (期望 6.1.99)"
[ "$V.$P.$S" = "6.1.99" ] || { echo "ERROR: 内核版本不是 6.1.99" >&2; exit 1; }

echo "== OK: $TREE"
