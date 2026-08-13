# 鲁班猫3 (RK3576) 开发板状态参考

> 本文档记录开发板当前已完成的配置与验证状态。

## 开发板基本信息

- 型号：鲁班猫3，SoC RK3576（8 核，内存 3.8GB）
- 系统：厂家 Ubuntu 22.04 镜像，内核 6.1.99-rk3576（Rockchip BSP）
- 访问：`ssh root@192.168.8.198`（免密）

## 摄像头配置（U-Boot overlay）

启动链路：`U-Boot → /boot/boot.scr(源 boot.cmd) → env import /boot/uEnv/uEnv.txt → 加载 /Image、/initrd、/rk-kernel.dtb → dtfile 应用 dtoverlay → booti`

`/boot/uEnv/uEnv.txt` 中 `enable_uboot_overlays=1`，`#overlay_start` 与 `#overlay_end` 之间未注释的 `dtoverlay=` 行由 U-Boot 在启动前动态叠加到基础 DTB（内存合并，不改磁盘）。当前启用：

```
dtoverlay=/dtb/overlay/rk3576-lubancat-3-cam1-gc4653-2560x1440-30fps-overlay.dtbo
dtoverlay=/dtb/overlay/rk3576-lubancat-3-cam3-gc4653-2560x1440-30fps-overlay.dtbo
```

即 CAM1、CAM3 各接一路 GC4653（RAW10，SGRBG10，2560x1440@30fps）。

## V4L2 设备拓扑

数据通路：`GC4653 → MIPI CSI2/D-PHY → rkcif → rkisp → rkvpss`。关键节点：

| 功能 | CAM1 | CAM3 |
|---|---|---|
| rkcif RAW 采集（id0 直通） | /dev/video0 (media0) | /dev/video11 (media1) |
| ISP mainpath（NV12 等 YUV 输出） | /dev/video22 (media2) | /dev/video31 (media3) |
| ISP selfpath 等 | /dev/video23-27,30 | /dev/video32-36,39 |
| 3A 统计（供 rkaiq） | /dev/video28,29 | /dev/video37,38 |
| rkvpss 缩放 | /dev/video41-44 (media4) | /dev/video45-48 (media5) |

> **注意：上表是仅启用 CAM1/CAM3 overlay 时的编号。** 启用 CAM0（xs9922b）overlay 后，
> `rkcif_mipi_lvds` 即使传感器未接也会注册 11 个 stream 节点（video0-10），其余节点整体偏移
> （如 ISP mainpath 变为 CAM1=/dev/video33、CAM3=/dev/video42，media 号同样后移）。
> 应用层应使用 udev 生成的稳定符号链接，如
> `/dev/v4l/by-path/platform-rkisp-vir1-video-index0`（CAM1 ISP mainpath）、
> `...-rkisp-vir3-video-index0`（CAM3）、`...-rkcif-mipi-lvds-video-index0..3`（XS9922B 4 路）。

- 传感器实体：CAM1 为 `m01_b_gc4653 4-0029`（I2C4 @0x29），media 链路已默认 ENABLED
- GC4653 是 RAW sensor，rkcif 直通节点输出 SGRBG10 RAW，需经 ISP 才能得到 YUV
- `rkaiq_3A_server` 已在运行，ISP 输出色彩/曝光正常

## CAM0 / XS9922B（4 路 AHD，已上板验证通过 2026-08-13）

CAM0 是底板 5 个 MIPI CSI 口中唯一的 4-lane 口，接 **XS9922B 4 路 AHD 转 MIPI 转接板**（经一块自研 MIPI CSI 转接板，供 1.8V/5V 并做 3.3V→1.8V 电平转换）。4 路已全部出图并接入 camd 预览。

- 驱动：`kernel/src/drivers/media/i2c/xs9922.c`，**内建于内核**（`CONFIG_VIDEO_XS9922=y`，必须内建，原因见 xs9922b_port.md §8）；当前 `/boot/Image` 指向 `Image-6.1.99-rk3576-builtin`，原厂 Image 保留可回退
- overlay：`rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay.dtbo`（uEnv 已启用）
- 控制通道：**I2C3 @0x31**（模组 strap 地址；注意该芯片只响应 16 位寄存器地址事务，`i2cdetect`/`i2cget` 扫不到，验证用 `i2ctransfer -y 3 w2@0x31 0x40 0xf0 r1` → 0x99）；CAM0_PWDN = GPIO1_D2、CAM0_RSTN = GPIO1_D3，probe 成功后持续拉高
- 数据通路：`XS9922B → csi2_dcphy0 → mipi0_csi2 → rkcif`，4 路走 VC0~VC3 → `/dev/v4l/by-path/platform-rkcif-mipi-lvds-video-index0..3`。AHD 出来就是 UYVY8_2X8，**rkcif 直出 NV12，不经 ISP/VPSS**，不占用单实例 ISP，与 CAM1/CAM3 的 GC4653 ISP 通路互不影响；rkcif 内置 scaler 可直接出 720p
- 默认模式 1080p@25fps；另支持 720p25 / CVBS PAL / CVBS NTSC。1080p@30fps 寄存器表待厂商提供
- 热插拔状态：`cat /sys/bus/i2c/devices/3-0031/hotplug_status`（4 位掩码）；注意本内核 `CONFIG_ROCKCHIP_CIF_USE_MONITOR` 未开，掉线后 rkcif **不会**自动复位管线，camd 侧靠采集超时重建管线
- 硬件要点：转接板到 XS9922B 模组的 **26pin FPC 必须反向连接**（转接板 pin N ↔ 模组 pin 27−N）；/boot 分区仅 124MB，放不下两份 42MB Image

部署与上板 bring-up 步骤见 [kernel/README.md](../kernel/README.md) 与 [docs/xs9922b_port.md](xs9922b_port.md)。

## 环境依赖安装

```bash
apt update && apt install -y \
    v4l-utils \
    pkg-config \
    librockchip-mpp-dev \
    librga-dev \
    libdrm-dev
```

- `v4l-utils`：v4l2-ctl、media-ctl 工具
- `pkg-config`：ffmpeg configure 依赖检测所需
- `librockchip-mpp-dev`、`librga-dev`、`libdrm-dev`：ffmpeg-rockchip 编译依赖，厂家镜像通常已预装，版本：mpp 1.5.0 / rga 2.2.0 / drm 2.4.114

## ffmpeg-rockchip 构建（已完成）

源码：`/root/ffmpeg-rockchip`（github.com/nyanmisaka/ffmpeg-rockchip master）。

- 依赖见上文"环境依赖安装"
- 配置命令：
  ```bash
  ./configure --prefix=/usr/local --enable-version3 --enable-libdrm --enable-rkmpp --enable-rkrga
  make -j8 && make install   # 8 核约 2 分钟
  ```
- 注意：rkmpp 是 version3 许可，`--enable-version3` 必须
- 安装至 `/usr/local/bin/ffmpeg`
- 可用硬件能力：`h264_rkmpp` / `hevc_rkmpp` / `mjpeg_rkmpp` 编码器；`scale_rkrga` / `vpp_rkrga` / `overlay_rkrga` 滤镜；v4l2 indev

## 已验证的抓帧流程

ISP mainpath 抓帧 → JPEG，端到端验证通过（图像色彩曝光正常）：

```bash
# 方式一：v4l2-ctl 抓 NV12 + ffmpeg 转 JPEG
v4l2-ctl -d /dev/video22 --set-fmt-video=width=2560,height=1440,pixelformat=NV12 --stream-mmap=3 --stream-skip=5 --stream-count=1 --stream-to=/tmp/f.nv12
/usr/local/bin/ffmpeg -y -f rawvideo -pix_fmt nv12 -s 2560x1440 -i /tmp/f.nv12 -q:v 2 out.jpg

# 方式二：ffmpeg 直接从 v4l2 节点抓帧（一步）
/usr/local/bin/ffmpeg -y -f v4l2 -input_format nv12 -video_size 2560x1440 -i /dev/video22 -frames:v 1 -q:v 2 out.jpg

# 方式三：MPP 硬件 JPEG 编码（快，适合批量；不响应 -q:v，画质低于软编）
/usr/local/bin/ffmpeg -y -f v4l2 -input_format nv12 -video_size 2560x1440 -i /dev/video22 -frames:v 1 -c:v mjpeg_rkmpp out.jpg
```

## 已验证的视频流硬编录制（H.265）

**注意：ffmpeg 的 v4l2 indev 直接读 ISP 节点连续流会出错**（仅出 1 帧后 EOF，输入被识别为 0.03fps）；单帧抓取正常，连续流必须用 v4l2-ctl 管道方案。已验证 5 秒 150 帧 @30fps：

```bash
v4l2-ctl -d /dev/video22 --set-fmt-video=width=2560,height=1440,pixelformat=NV12 --stream-mmap=16 --stream-count=150 --stream-to=- 2> /dev/null | /usr/local/bin/ffmpeg -y -f rawvideo -pix_fmt nv12 -s 2560x1440 -r 30 -i - -c:v hevc_rkmpp -b:v 8M -g 60 out.mp4
```

- 时长控制：`--stream-count=150`（30fps × 5 秒）；长时间录制用 `--stream-count=0`（无限）+ ffmpeg `-t` 或 `timeout`
- 实测码率约 7.8Mbps（-b:v 8M），画质正常
- CAM3 换 `/dev/video31`；H.264 换 `-c:v h264_rkmpp`
- **`--stream-mmap` 必须 ≥16**：缓冲过少（如 4）时管道消费抖动会让 vb2 队列耗尽，rkisp 驱动停止出帧（不丢帧、无 dmesg 报错），v4l2-ctl 永久阻塞在 `dqbuf`，ffmpeg 随之饿死——表现为编码到第几十帧（如 54 帧）卡死
