# multi-cam-preview

RK3576 多路摄像头 WebRTC 预览应用（远程驾驶前期验证）。板端零拷贝视频管线：
V4L2 dma-buf 采集 → mpp H.265 硬编 → Pion WebRTC 发送，全程无 CPU 像素处理。

- 可行性评估与软硬件限制：见 [docs/feasibility.md](docs/feasibility.md)
- 板上环境配置：见 [../../docs/board_setup.md](../../docs/board_setup.md)

## 架构

```
GC4653 ──ISP──▶ /dev/video22,31 ──V4L2 dma-buf──▶ camd (Go+CGo)
                                                   ├─ mpp H.265 (rkvenc 硬编)
                                                   ├─ HTTP 控制/信令 API
                                                   └─ 内嵌前端静态托管
浏览器 (Chrome 136+) ◀──H.265 over WebRTC──┘
```

- 每路通道一条 PeerConnection（WHEP 风格 SDP 交换，ICE-lite，LAN 免 STUN）
- 编码帧经 Hub 广播给 0..N 个观众；新观众请求 IDR 后起播；慢观众丢帧不阻塞编码
- 码率/GOP 运行时动态生效；分辨率/帧率变更重建该路采集+编码（观众连接保持）

## 部署

```bash
cd apps/multi-cam-preview
./scripts/deploy.sh        # 同步源码 → 板端构建 → 安装 systemd 服务并重启
./scripts/board_verify.sh  # 节点探测 / 抓帧冒烟 / 服务状态
```

前提：板端已装 Go（/usr/local/go）、librockchip-mpp-dev、librga-dev（见 board_setup.md）。

部署后访问 `http://192.168.3.173:8080/`。

## 配置

`camd/config.yaml`（部署到板上 `/opt/multi-cam-preview/config.yaml`，改后 `systemctl restart camd`）：

```yaml
listen: ":8080"
channels:
  - id: cam1
    name: "CAM1 · GC4653"
    type: raw            # raw=ISP 通路 / ahd=XS9922B（后续）/ stitch=环视（后续）
    device: /dev/video22
    enabled: true
    width: 1280
    height: 720
    fps: 30              # 编码帧率，<= source_fps，超出部分按帧抽取
    bitrate_kbps: 2048
    gop: 60
    source_fps: 30       # sensor 采集帧率
    format: NV12         # NV12 / UYVY
```

CSI2/CSI4 扩展：接入摄像头并启用对应 overlay 后，将 `csi2`/`csi4` 的 `device`
改为实际 ISP mainpath 节点并置 `enabled: true` 即可。

## HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/channels` | 全部通道状态（online/参数/观众数/错误） |
| PUT | `/api/channels/{id}` | 调整参数，body 为部分 JSON：`width/height/fps/bitrate_kbps/gop`（null 字段不变） |
| POST | `/api/whep/{id}` | WHEP 风格 SDP 交换：body 为 offer（`application/sdp`），响应为 answer |
| GET | `/ws/stats` | SSE 统计推送（2s 周期：每路实际 fps/kbps、编码负载估算） |

参数约束：宽高 320×240~2560×1440 偶数；fps 1~source_fps；码率 128~20480 kbps；GOP 1~300。

## 前端

- 12 列网格自由编排：拖拽换位、右下角手柄调整跨度（1~12 列 × 1~4 行），布局存 localStorage
- 预设布局：3×3 / 2×2 / 一大 N 小
- 双击或 ⤢ 按钮单路放大全屏（纯前端缩放，不触发板端规格变化），Esc 退出
- 右侧参数面板：分辨率/帧率/码率/GOP，应用即时生效或短暂重建
- 顶栏：连接状态、在线通道数、编码负载估算、RTT 中位数

## 板上验证结果（2026-08-08）

- 2 路 GC4653（/dev/video22、/dev/video31）同时 1280×720@30 编码：实测 30.0fps、2.05Mbps（CBR 2048k），编码负载估算 11%
- 码率动态调整 2048→4096 kbps：mpp 运行时生效，无需重建
- 分辨率 720p→1080p 重建：新管线 1920×1080@30 实测 4.1Mbps（CBR 4096k）
- WHEP 信令：offer→answer 正常（ICE-lite，host candidate，H265/90000 PT96）

> 浏览器端画面验证需在上位机 Chrome 136+ 打开 `http://192.168.3.173:8080/` 确认
> （`chrome://gpu` 需支持 HEVC 硬解）。

## 已知限制与后续阶段

- **rkaiq_3A 崩溃会导致 ISP 停帧**（已观测：画面剧烈变化时 3A 曝光断言 "can't find the latest
  effecting exposure" 失败退出，ISP 无 params 停止出帧，dmesg 报 `waiting on params stream on event
  timeout`）。缓解：板上已为 `rkaiq_3A.service` 配置 `Restart=always` drop-in
  （`/etc/systemd/system/rkaiq_3A.service.d/override.conf`），camd 采集超时自动重启管线，
  恢复过程约 10~20s；camd.service 已声明 `Wants/After=rkaiq_3A.service`
- ffmpeg v4l2 indev 读 ISP 连续流会 EOF 卡死——本应用自研采集已规避；采集缓冲固定 16（vb2 队列耗尽会导致 rkisp 静默停帧）
- 单 ISP 硬件时分复用：4 路 RAW 满规格（2560×1440@30）吞吐不足，需降 mode/降帧率
- 双核 rkvenc 总吞吐约 4K@60：8 路全 1080p30 不可行，默认低分辨率 + API 按需提升
- 后续阶段：XS9922B 驱动移植 + 4 路 AHD 接入（待厂商 30fps 寄存器配置）；RGA 简单鸟瞰环视拼接流

## 目录结构

```
├── docs/feasibility.md      # 可行性评估
├── camd/
│   ├── cmd/camd/main.go     # 入口
│   ├── internal/capture/    # V4L2 采集（MPLANE + EXPBUF dma-buf）
│   ├── internal/encode/     # mpp H.265 编码封装（CGo）
│   ├── internal/pipeline/   # 通道编排、Hub 广播、参数重建
│   ├── internal/webrtcsrv/  # Pion WebRTC（WHEP 信令、RTCP PLI→IDR）
│   ├── internal/api/        # REST + SSE + 静态托管
│   └── config.yaml          # 通道清单
├── web/                     # 纯静态前端（embed 进 daemon）
└── scripts/                 # deploy.sh / board_verify.sh / camd.service
```
