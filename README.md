# RK35xx 平台 AHD 摄像头调试沙盒

## 项目简介

当前项目用于 RK35xx 平台 AHD 多路聚合摄像头（XS9922B 方案）和 MIPI CSI 直连摄像头的调试和验证。

使用的开发板为鲁班猫3 RK3576 金手指核心版+鲁班猫官方底板，底板原理图位于 `references/lubancat3/EBF410529V1R0_SCH_20250919.pdf`。开发板安装了鲁班猫提供的 Ubuntu 22.04 无桌面操作系统，Linux SDK 位于 `references/lubancat3/.repo`。当前开发环境为 [devcontainer](.devcontainer/devcontainer.json)。

可以 `ssh root@192.168.8.198` 免密连接到开发板进行调试。参考 [鲁班猫3 (RK3576) 开发板状态参考](./docs/board_setup.md)

## 目录结构

```
rk-ahd-sandbox/
├── .devcontainer/                # 开发容器配置
├── apps/
│   └── multi-cam-preview/        # 多路摄像头 WebRTC 预览（Go 采集编码 + Web 前端）
├── kernel/                       # 内核外挂源码与构建脚本（XS9922B 驱动 + CAM0 overlay）
│   ├── src/                      # 镜像内核树结构，rsync 叠加到工作树
│   └── scripts/                  # prepare_tree / apply_to_tree / build / deploy
├── docs/                         # 项目文档
├── references/                   # 参考资料
│   ├── lubancat3/                # 鲁班猫3 开发板参考资料和 Linux SDK
│   └── xs9922b/                  # XS9922B 转接板参考资料和驱动参考代码
├── resources/                    # 资源文件
│   ├── firmwares/                # 开发板固件压缩包
│   └── sdk/                      # SDK 压缩包
├── scripts/                      # 调试脚本
```

## 当前进度

- 2 路 GC4653（CAM1/CAM3，RAW → ISP → NV12）已验证，`apps/multi-cam-preview` 可 WebRTC 预览
- XS9922B 4 路 AHD → MIPI CSI0：**已上板验证通过（2026-08-13）**，4 路 1080p25 全部出图，
  rkcif 直出 NV12（可缩放 720p），已接入 camd 预览（6 路同在线）。驱动内建于内核 Image
  → [kernel/README.md](./kernel/README.md)、[docs/xs9922b_port.md](./docs/xs9922b_port.md)
