# kernel — 内核外挂源码与构建脚本

给鲁班猫3（RK3576，内核 `6.1.99-rk3576`）增加 **XS9922B 4 路 AHD → MIPI CSI-2** 支持。移植细节、硬件映射和上板 bring-up 步骤见 [docs/xs9922b_port.md](../docs/xs9922b_port.md)。

部署形态：**内建内核 Image + 设备树 overlay**（`CONFIG_VIDEO_XS9922=y`）。
最初设计为模块增量安装，上板后证明模块形态在本平台不可行（dphy 驱动的 GKI 检查 +
别名索引问题，见 docs/xs9922b_port.md §8），Image 以 `Image-<krel>-builtin` 安装、
符号链接切换，原厂 Image 保留可回退。

## 目录

```
kernel/
├── src/                     # 镜像内核树的目录结构，rsync 直接叠加上去
│   ├── drivers/media/i2c/xs9922.c              # 驱动
│   ├── drivers/media/i2c/xs9922_reg_cfg.h      # 视频寄存器表
│   ├── drivers/media/i2c/xs9922_audio_reg_cfg.h
│   └── arch/arm64/boot/dts/rockchip/overlay/
│       └── rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay.dts
├── scripts/
│   ├── prepare_tree.sh      # 从本地 repo 镜像检出 6.1.99 工作树（离线）
│   ├── apply_to_tree.sh     # rsync src/ + 幂等插入 Kconfig/Makefile/uEnv 引用
│   ├── build.sh             # 交叉编译 .ko + .dtbo，离线 fdtoverlay 合并校验
│   └── deploy.sh            # scp 到板子 + depmod + 改 uEnv.txt
└── out/                     # 构建产物（gitignore）
```

内核工作树落在 `references/lubancat3/kernel-6.1/`（gitignore），源自 `references/lubancat3/.repo/` 里的 repo 镜像，revision `c9df0f9` = 板端运行的 6.1.99。

## 构建

```bash
kernel/scripts/prepare_tree.sh      # 首次：检出内核工作树（~1 分钟）
kernel/scripts/apply_to_tree.sh     # 叠加源码 + 插入构建引用（幂等，改完 src/ 就再跑一次）
kernel/scripts/build.sh             # 编译 + 校验
```

产物：

| 文件 | 说明 |
|---|---|
| `out/Image` | 内建 xs9922 的完整内核镜像（release `6.1.99-rk3576`） |
| `out/rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay.dtbo` | CAM0 overlay |
| `out/build.log` | xs9922.o 的 `make W=1` 日志 |
| `out/merged.dts` | 基础 dtb + overlay 的离线合并结果，用于人工核对 |

`build.sh` 的环境变量：

| 变量 | 默认 | 作用 |
|---|---|---|
| `JOBS` | `nproc` | 并行度 |
| `RECONFIG=1` | — | 重新跑 defconfig（平时复用已有 `.config`） |
| `KERNEL_TREE` | `references/lubancat3/kernel-6.1` | 内核工作树位置 |
| `CROSS_COMPILE` | `aarch64-linux-gnu-` | 工具链前缀 |

> `build.sh` 里 `export LOCALVERSION=` 不能删。`scripts/setlocalversion` 判的是变量「有没有被设置」（`${LOCALVERSION+set}`），不设置时会因为工作树 dirty 给版本号追加 `+`，编出 `6.1.99-rk3576+`，vermagic 与板端不符，insmod 直接被拒。

`apply_to_tree.sh` 在内核树里只碰这 4 个文件，每处 1~13 行，且插入前先 grep：

- `drivers/media/i2c/Kconfig` — `config VIDEO_XS9922`
- `drivers/media/i2c/Makefile` — `obj-$(CONFIG_VIDEO_XS9922) += xs9922.o`
- `arch/arm64/boot/dts/rockchip/overlay/Makefile` — 加 dtbo 目标
- `arch/arm64/boot/dts/rockchip/uEnv/rk3576/uEnvLubanCat3.txt` — 加一行**注释掉的** `dtoverlay=`

`CONFIG_VIDEO_XS9922=y` 由 `build.sh` 用 `scripts/config --enable` 打在生成的 `.config` 上，
**不改官方 defconfig 文件**。

## 部署

```bash
kernel/scripts/deploy.sh            # 默认 BOARD=root@192.168.8.198
ssh root@192.168.8.198 reboot       # 新内核 + overlay 必须重启才生效；或 REBOOT=1 让脚本代劳
```

`deploy.sh` 做的事，任何一步失败就停：

1. `Image` → `/boot/Image-<krel>-builtin`，`/boot/Image` 符号链接切换过去
   （原厂 `Image-<krel>` 保留；回退：`ln -sf Image-<krel> /boot/Image`），
   并清理 `/lib/modules/<krel>/extra/xs9922.ko` 历史模块
2. `.dtbo` → `/boot/dtb/overlay/`
3. `/boot/uEnv/uEnv.txt`：先 `cp -n` 备份成 `uEnv.txt.bak.<时间戳>`，再在
   `#overlay_start`/`#overlay_end` 之间启用对应 `dtoverlay=` 行（已启用则跳过，
   有注释行则取消注释，都没有则插入），最后打印当前生效的全部 overlay

注意：**/boot 分区只有 124MB**，放不下两份 42MB Image，多余备份请放 `/root`。

重启后按 [docs/xs9922b_port.md](../docs/xs9922b_port.md) 的 bring-up 检查清单逐项确认。

## 当前状态

**已上板验证通过（2026-08-13）**：4 路 AHD 1080p25 全部出图，已接入 camd 预览。
**2026-08-14（驱动 V0.01.02）**：1080p30 支持落地——运行时制式自适应（热插拔线程读
`0xN001` 回写 `0xNE12`），25/30fps 摄像头可混插；free-run 1080p30 实测 30.00fps。
遗留待办：

- I2C 验证注意：该芯片只响应 16 位寄存器地址事务，`i2cdetect`/`i2cget` 扫不到，
  用 `i2ctransfer -y 3 w2@0x31 0x40 0xf0 r1`（应返回 0x99）；驱动绑定后需加 `-f`
