# XS9922B（4 路 AHD → MIPI）驱动移植说明

> **已上板验证通过（2026-08-13）**：4 路 AHD 1080p25 全部出图，rkcif 直出 NV12，
> 可缩放至 720p，已接入 `apps/multi-cam-preview`（camd 6 路全部在线）。
> 上板过程中的关键结论见文末 [bring-up 检查清单](#上板-bring-up-检查清单) 与 §7。

## 1. 硬件链路

```
4× AHD 摄像头 ──► XS9922B 解码板 ──FPC──► MIPI CSI 转接板 ──FPC24 J14──► 底板 MIPI CSI0
                  (4 路 → 1 条 4-lane MIPI，VC0~VC3)      (供 1.8V/5V，已做 3.3V→1.8V 电平转换)
```

CSI0 是底板 5 个 MIPI CSI 接口中唯一的 4-lane 口（其余 4 个拆成 2-lane），引脚定义见
`references/lubancat3/EBF410529V1R0_SCH_20250919.pdf` 第 11 页。

| 底板信号 | RK3576 引脚 | 转接板 FPC | 说明 |
|---|---|---|---|
| CAM0_MCLK | — | 21 `CAM_MCLK` | **NC**，解码板用自带 27MHz 晶振 |
| CAM0_PWDN | GPIO1_D2 | 20 `CAM_PDN_L` | 工作期间必须持续拉高 |
| CAM0_RSTN | GPIO1_D3 | 19 `CAM_REST` | 工作期间必须持续拉高 |
| I2C3_SDA/SCL_M0 | — | — | 控制通道，XS9922B 默认从地址 0x30 |
| MIPI CSI0 D0~D3 + CLK | csi2_dcphy0 | — | 1.5Gbps/lane × 4，连续时钟 |
| VDD_5V / VDD_3V3 | — | 23/24/25 (2V8/1V2) NC | CAM0 各路电源在 DT 里都是 always-on 固定电源 |

数据通路（YUV422 不经 ISP）：

```
xs9922 subdev ──► csi2_dcphy0 ──► mipi0_csi2 ──► rkcif_mipi_lvds (+rkcif_mmu)
                                                   └─► stream_cif_mipi_id0..id3 = 4 路画面
```

4 路画面走同一条 MIPI link 的 4 个虚拟通道。驱动没实现 `RKMODULE_GET_CHANNEL_INFO`，
`rkcif_get_input_fmt()`（`cif/capture.c:1024`）会回落到 `csi_info->vc = pad_id`，正好把
VC0~VC3 依次映射到 `stream_cif_mipi_id0..id3`，与 nvp6188 的做法一致。

## 2. 交付物

```
kernel/
├── README.md                                # 构建/部署一页速查
├── src/                                     # 镜像内核树目录结构，rsync 直接叠加
│   ├── drivers/media/i2c/xs9922.c           # 驱动（1459 行，厂商版 1773 行）
│   ├── drivers/media/i2c/xs9922_reg_cfg.h   # 寄存器表（取 SDK 版，含 720p/CVBS）
│   ├── drivers/media/i2c/xs9922_audio_reg_cfg.h
│   └── arch/arm64/boot/dts/rockchip/overlay/
│       └── rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay.dts
└── scripts/{prepare_tree,apply_to_tree,build,deploy}.sh
```

内核树内被幂等修改的 4 个文件（由 `apply_to_tree.sh` 插入，每处 1~13 行）：
`drivers/media/i2c/{Kconfig,Makefile}`、`arch/arm64/boot/dts/rockchip/overlay/Makefile`、
`arch/arm64/boot/dts/rockchip/uEnv/rk3576/uEnvLubanCat3.txt`。

部署形态是**内建内核 Image + dtbo**（`CONFIG_VIDEO_XS9922=y`）。最初按模块（=m）增量安装，
上板后发现模块形态在此平台上**不可行**，原因见 §8。

## 3. 驱动移植：与厂商版的差异

基线：`references/rk3576-linux-SDK-20260616/kernel-6.1/drivers/media/i2c/xs9922/xs9922.c`
（rpdzkj RK3576 SDK，内核 6.1.118）。目标：鲁班猫官方内核 6.1.99
（`c9df0f9`，与板端 `uname -r = 6.1.99-rk3576` 同源）。

### 3.1 删除的平台专有逻辑

| 厂商实现 | 处理 |
|---|---|
| `read_config()`：内核里 `filp_open("/etc/board.conf")` 解析 `camode=`/`outiming=` | 删除。它同时是唯一的初始化路径，初始化序列改写为 `xs9922_init_hw()`，在 probe 里同步调用 |
| 填充 `drm_display_mode`、`hdmiCusMode`、`board_config_buf` | 删除（与显示子系统无关） |
| 私有 ioctl `XS9922_SET_FMT` | 删除（标准 `VIDIOC_SUBDEV_S_FMT` 已够） |
| `dumpChxReg()`、`read_xs9922_audio_reg()`、`#if 0` 块、`__CLOSE_SENSOR__` 宏 | 删除/按 `== 0` 分支展平 |
| `include/ni_type.h` | 删除。实际用到的 `NI_40F0_DEVICE_ID_1`、`NI_VIDEO_STATUS_CH*` 等宏都定义在 xs9922.c 自身，驱动拍平成 nvp6188 那样的单文件 + 两个表头 |
| `pm_runtime`、`regulator`、`rk-preisp.h` | 删除。`rkcif_sensor_set_power()`（`cif/capture.c:8345`）只调 `core, s_power` 且忽略返回值，sensor 侧不走 pm_runtime；CAM0 各路电源是 always-on 固定电源 |

### 3.2 修掉的缺陷

| 位置 | 问题 | 处理 |
|---|---|---|
| `xs9922_write_array()` | 无 NULL 保护，而 720p25 模式漏填 `audio_reg_list` → `switch_mode()` 空指针 | 入口 `if (!regs) return 0;`，并给 720p25 补上音频表 |
| probe 尾部 | `sysfs_create_group` / `input_register_device` 失败直接 return，subdev / media entity / ctrl handler 全泄漏 | 补全 6 级 goto unwind；`remove()` 补 `sysfs_remove_group` |
| `xs9922_enum_frame_interval()` | 任何 index 都返回 0 且总是回 mode 0 → 用户态枚举不终止 | 按 `cfg_num` 边界返回 `-EINVAL`，并上报 `reserved[0] = hdr_mode`（gc4653 写法） |
| `xs9922_enum_mbus_code()` | 同样不检查 index | `if (code->index) return -EINVAL;` |
| `xs9922_set_fmt()` | `V4L2_SUBDEV_FORMAT_TRY` 也会写 `xs9922_mipi_reset_new`（TRY 产生硬件副作用） | MIPI reset 移进 ACTIVE 分支 |
| `xs9922_set_fmt()`/`get_fmt()` | `colorspace` 被赋成了 `mode->bus_fmt` | 改为 `V4L2_COLORSPACE_SMPTE170M` |
| `g_volatile_ctrl` | 从未被调用（无 `VOLATILE` 标志），且把 `s32*`/`int*` 传给 `u32*` 参数（`-Wpointer-sign`） | 整个回调删除 |
| 热插拔上报 | 用 `last < current` 判断插入/拔出，同时一插一拔时判断错误 | 改为逐位比较新值（`xs9922_report_hotplug()`） |
| `devm_gpiod_get()` | 无返回值检查 | 统一 `IS_ERR_OR_NULL` + `dev_warn` |
| `__xs9922_power_on()` | 不设 `power_on` 标志 → probe 后热插拔线程读寄存器被跳过 | 置 `power_on = true`；`err_clk` 路径补上拉低 power_gpio |
| `V4L2_MBUS_CSI2_CHANNEL_0..3` | 6.1.99 的 `<media/v4l2-mediabus.h>` 已删除这些宏（SDK 的 6.1.118 也没有，只是那些驱动没开） | `vc[]` 直接写 0~3，同 nvp6188 |

### 3.3 保留的功能

- 4 个模式：AHD 1080p25（默认）、AHD 720p25、CVBS PAL 720×576、CVBS NTSC 720×480
- 热插拔检测线程（100ms/1000ms 轮询 + 2 拍去抖）→ `hotplug_status` sysfs +
  `xs9922_input_event` input 设备（`EV_MSC`/`MSC_RAW`，值为 4 位通道掩码）+
  `RKMODULE_GET_VC_HOTPLUG_INFO` / `GET_VICAP_RST_INFO`（见 §5 的 cif-monitor 说明）
- 音频寄存器表（转接板未引出 I2S，写入无害）
- 图像控制：BRIGHTNESS / CONTRAST / SATURATION / HUE（0~255，默认 0x80，一次写 4 个通道）
- sysfs：`hotplug_status`（只读，通道掩码）、`cam_power`（只写，控制 `camera-gpios`）

### 3.4 初始化时序

`xs9922_init_hw()` 在 probe 里 `check_chip_id()` 之后同步执行，顺序遵循
`references/xs9922b/readme.txt`：

```
cam_gpio 拉高 → xs9922_init_cfg(391) → 分辨率表(113) → xs9922_mipi_reset_new(29) → 音频表(78)
```

共约 611 次 I2C 单字节写，寄存器表里所有 `nDelay` 均为 0，probe 耗时是纯 I2C 传输时间
（400kHz 下约 60~80ms），可以同步做。厂商 readme 要求「MIPI 接收端先就绪」，probe 阶段
无法保证，因此 `set_fmt()` 与 stream-on 都会**重复一遍 MIPI reset 序列**，等 rkcif 配好
D-PHY 后再同步一次帧头。

`s_power()` 是空实现：转接板要求 CAM_REST / CAM_PDN_L 全程拉高，掉电重上会丢掉 4 路
AHD 锁定，所以驱动 probe 时上电一次并保持，只在 probe 失败 / remove 时
`xs9922_hw_teardown()` 拉低。

## 4. 设备树 overlay

`rk3576-lubancat-3-cam0-xs9922b-1920x1080-25fps-overlay.dts`，6 个 fragment：

| fragment | target | 作用 |
|---|---|---|
| 0 | `&i2c3` | okay + **新建** `xs9922-0@30`（基础 dtsi 里没有这个节点） |
| 1 | `&csi2_dcphy0` | okay + 新增 `ports/port@0/endpoint@9`（`endpoint@1..@8` 已被自带 sensor 占用） |
| 2 | `&mipi0_csi2` | okay |
| 3 | `&rkcif` | okay |
| 4 | `&rkcif_mipi_lvds` | okay |
| 5 | `&rkcif_mmu` | okay |

**明确不开** `&rkcif_mipi_lvds_sditf` / `&rkisp*` / `&rkvpss*`：AHD 出来就是 UYVY8_2X8，
rkcif 可直接写出 NV12/NV16/UYVY，单实例 ISP 留给现有 2 路 GC4653。

关键属性：

- `clocks = <&ext_cam0_27m_clk>` / `clock-names = "xvclk"`：MCLK 实际 NC，但驱动
  `devm_clk_get(dev, "xvclk")` 是强制的；基础 dtsi 里现成的这个 fixed-clock 正好 27MHz，
  等于 `XS9922_XVCLK_FREQ`，`clk_set_rate()` 返回 0 且 `clk_get_rate()` 校验通过
- `power-gpios = <&gpio1 RK_PD2 GPIO_ACTIVE_HIGH>`、`reset-gpios = <&gpio1 RK_PD3 GPIO_ACTIVE_HIGH>`：
  配合 `__xs9922_power_on()` 的「power 拉高 → 25ms → reset 拉低 5ms 再拉高 → 100ms」，
  上电后两脚持续为高，满足转接板手册要求
- 没有 `*-supply`：CAM0 的 5V/3V3/1V8 在 `rk3576-lubancat-3-csi.dtsi` 里都是
  `regulator-fixed` + `always-on` + `boot-on`

## 5. `rockchip,cif-monitor` 在本内核里是死属性

SDK 的 `rp-camera-dphy0-mipi-xs9922b.dtsi` 给 `rkcif_mipi_lvds1` 写了
`rockchip,cif-monitor = <1 2 5 1000 5>`，本 overlay **没有照抄**，原因：

- 属性名只在 `drivers/media/platform/rockchip/cif/dev.h:37`（`OF_CIF_MONITOR_PARA`）声明，
  6.1.99 里**没有任何代码解析它**
- `rkcif_init_reset_monitor()`（`cif/dev.c:2440`）的参数全部来自 Kconfig：
  `CONFIG_ROCKCHIP_CIF_USE_MONITOR` / `_MODE` / `_KEEP_TIME` / `_CYCLE` / `_START_FRAME` / `_ERR_CNT`
- `lubancat_linux_rk3576_defconfig` 只有 `CONFIG_VIDEO_ROCKCHIP_CIF=y`，没开 USE_MONITOR
  → `monitor_mode = RKCIF_MONITOR_MODE_IDLE`，复位看门狗根本不跑

**后果**：驱动里的 `RKMODULE_GET_VICAP_RST_INFO`（`cif/capture.c:11956` 只在
`monitor_mode == RKCIF_MONITOR_MODE_HOTPLUG` 分支里查询）当前是**惰性的**——AHD 掉线后
rkcif 不会自动复位管线。`hotplug_status` sysfs 与 input 事件仍然正常工作，应用层可据此
自行停流/重开流（`apps/multi-cam-preview` 已有采集超时重建管线的机制）。

如果将来确实需要内核自动复位：要 `CONFIG_ROCKCHIP_CIF_USE_MONITOR=y` +
`CONFIG_ROCKCHIP_CIF_MONITOR_MODE=0x3`（HOTPLUG）重编内核——rkcif 是 `=y` 内建，
这就不再是「模块增量安装」，需要整体换内核，不在本期范围内。

## 6. 已完成的离线验证

| 项 | 结果 |
|---|---|
| 交叉编译 `xs9922.ko` | 通过，`make W=1` 下 xs9922.c/`*.h` **零告警零错误** |
| vermagic | `6.1.99-rk3576 SMP mod_unload aarch64`，release 段与板端 `uname -r` 一致（注意实测**没有** `preempt` 字段，该 defconfig 不是 `CONFIG_PREEMPT`） |
| overlay 编译 | `.dtbo` 生成；`__fixups__` 含 `i2c3 / ext_cam0_27m_clk / gpio1 / csi2_dcphy0 / mipi0_csi2 / rkcif / rkcif_mipi_lvds / rkcif_mmu`，`__local_fixups__` 含两个 `remote-endpoint` 本地交叉引用 |
| `fdtoverlay` 离线合并（等价 U-Boot 的 dtoverlay 应用） | 成功；`i2c@2ac60000`(=i2c3) 下出现 `xs9922-0@30` status=okay；`endpoint@9` 与 xs9922 的 `port/endpoint` phandle 双向对应；`clocks` 指向 `external-camera-27m-clock`；gpio 指向 `gpio@2ae10000`(=gpio1) 偏移 0x1a/0x1b(=D2/D3) |
| 通路 status 核对 | `csi2-dcphy0`/`mipi0-csi2`/`rkcif@27c10000`/`iommu@27c10800`/`rkcif-mipi-lvds` = okay；`rkcif-mipi-lvds-sditf`/`isp@27c00000`/`vpss@27c30000` 仍为 disabled；`es8388@11` 未被破坏 |
| 回归：现有 gc4653 overlay | cam0/cam1/cam3 三个 overlay 单独编译 + 合并均正常 |
| 回归：与板端实际配置共存 | `cam1-gc4653 + cam3-gc4653 + cam0-xs9922b` 三个 dtbo 依次叠加成功，GC4653 的 ISP/VPSS 通路与本 overlay 的 CIF 直出通路互不干扰 |
| 符号闭合 | 通过。`SYMCHECK=1 kernel/scripts/build.sh` 先 `make vmlinux` 编出 `vmlinux.o` + `Module.symvers`，此时 modpost **零未定义符号**；`nm -u xs9922.ko` 的 61 个未定义符号全部能在 `Module.symvers` 中找到 |

代码走查（对照厂商版逐条确认）：4 个模式的 `audio_reg_list` 均非 NULL；probe 各失败点
都有对应 unwind；`xs9922_hw_teardown()` 只在 probe 失败/remove 调用，不在 stream 生命周期
里拉低 power/reset；init 顺序与 `references/xs9922b/readme.txt` 一致。

## 6.1 上板验证结果（2026-08-13）

| 项 | 结果 |
|---|---|
| probe / chip id | `detected chip id 0x9922`，4 路热插拔全在线（`hotplug_status = 0xf`） |
| media 拓扑 | `m00_b_xs9922 3-0031` → `rockchip-csi2-dphy0` → `mipi0-csi2` → rkcif，链路 ENABLED |
| 4 路抓帧 | index0..3 各 1080p25 NV12 出图正常，画面清晰色彩正确 |
| 4 路同采 5s | 各 125 帧无丢失，MIPI 带宽充足 |
| rkcif 缩放 | S_FMT 1280x720 直接生效（内置 scaler），camd 按 720p 采集 |
| camd 集成 | ahd0~3 上线，与 2 路 GC4653 共存（6 路同在线） |
| 回归 | GC4653 两路 ISP 通路不受影响；脚本构建的 Image 冷启动全链路自恢复 |

## 7. 假设与遗留项

1. ~~**I2C 地址 0x30**~~ **已确认为 0x31**（模组 strap 地址，厂家确认）。注意一个重大坑：
   **XS9922B 只响应完整的 16 位寄存器地址事务**，`i2cdetect`（quick-write / receive-byte）
   和 `i2cget`（单字节寄存器协议）全部 NACK——芯片活着也扫不到。正确的验证命令：
   `i2ctransfer -y 3 w2@0x31 0x40 0xf0 r1` → 返回 `0x99`。
2. **转接板把 CAM0_PWDN/CAM0_RSTN 透传到 CAM_PDN_L/CAM_REST**：已验证（模组脚实测 1.8V）。
3. ~~**1080p@30fps 待补**~~ **已完成（2026-08-14，驱动 V0.01.02）**：拿到《XS9922B芯片用户
   寄存器手册》后确认高清解码本身自动跟踪制式（`0xN10c` bit0=0 自动模式，`0xN10d` 手动值
   无效），主机侧唯一与帧率相关的是 `0xNE12`（MIPI_FREE_RUN_STD）。驱动在热插拔线程中对
   每个锁定通道读 `0xN001`（HD 制式回读：1080p25=0x?4、1080p30=0x?5），按检测结果回写
   `0xNE12`（5→1080p25、6→1080p30），25/30fps 摄像头可混插，无需单独的 30fps 寄存器表。
   `g_frame_interval` 按通道实测帧率上报；新增 `video_std` sysfs 查看每通道制式
   （如 `ch0: ASTD 1920x1080@25 (0x44)`）。已上板验证：4 路 1080p25 识别正确；
   free-run 强制 1080p30（`0x0e10=1, 0x0e12=6`）实测 30.00fps 出流。**接 30fps 摄像头后
   camd 侧 `source_fps`/`fps` 需相应改为 30**（当前 YAML 仍是 25）。
4. **MIPI 速率**：1.5Gbps/lane × 4lane 连续时钟（`0x511b = 0x78`）。RK3576 D-PHY 支持
   2.5Gbps/lane，余量充足；`V4L2_CID_LINK_FREQ` 报 750MHz（1.5G >> 1）。
5. **不接入 ISP**：4 路 YUV422 由 rkcif 直接写出，不占用单实例 ISP，与现有 2 路 GC4653 的
   ISP 通路互不影响。
6. **cif-monitor 惰性**：见 §5，掉线自动复位需要重编内核，本期不做（应用层 camd 已有
   采集超时重建机制）。
7. ~~**本期不改 `apps/multi-cam-preview`**~~ 已完成：camd 的 `ahd` 类型复用 raw 的
   V4L2 采集管线，4 路以 720p NV12 上线。

## 8. 为什么驱动必须内建（=y），模块形态不可行

最初按「模块 + dtbo 增量安装」设计（`CONFIG_VIDEO_XS9922=m`），上板后发现 sensor 实体
永远进不了 media 拓扑，根因是三个因素叠加：

1. **dphy 驱动的 GKI 检查**：`phy-rockchip-csi2-dphy.c` 的
   `rockchip_csi2_dphy_fwnode_parse()` 只在 sensor 驱动**已绑定**时
   （`bus_find_device_by_fwnode(i2c_bus_type) && dev->driver`）才把端点加入异步匹配
   等待列表，否则直接跳过。`csi2-dcphy0` 内建（~4s probe），而模块形态的 xs9922 要等
   udev 加载（~8s）→ parse 时端点被永久跳过。
2. **dphy 不可重触补偿**：`csi2_dcphy0` 的别名是 `csi2dcphy0`，而驱动用
   `of_alias_get_id(node, "csi2dphy")` 查不到 → 回退 index 0 → `rockchip,hw` phandle
   与 index 不符 → 重绑返回 `-EINVAL`，无法通过手动 rebind 重建通知器。
3. **sensor 单独 rebind 无法自愈**：异步匹配依赖 subdev notifier 的 parent 挂载，
   sensor 后注册时 dphy 通知器里没有等待项，匹配链断掉。

内建后时序变成：i2c 驱动（`module_i2c_driver` → late_initcall）先于 dphy 的
subsys_initcall 完成 probe 绑定 → dphy parse 时 GKI 检查通过 → sensor 注册即匹配
→ `dphy0 matches m00_b_xs9922 3-0031` → 拓扑建立。

部署细节：`build.sh` 产出完整 `Image`；`deploy.sh` 装为 `/boot/Image-<krel>-builtin`
并切换 `/boot/Image` 符号链接（原厂 `Image-<krel>` 保留可回退）。**/boot 分区只有
124MB**，放不下两份 42MB Image，多余备份放 `/root`（根分区）。

## 上板 bring-up 检查清单

```bash
# 0. 部署（先看 kernel/README.md）：内建 Image + dtbo + 启用 uEnv 行
kernel/scripts/deploy.sh
ssh root@192.168.8.198 reboot

# 1. I2C 通信验证（重要：i2cdetect / i2cget 探测不到这颗芯片！它只响应
#    完整的 16 位寄存器地址事务，必须用 i2ctransfer）
i2ctransfer -y 3 w2@0x31 0x40 0xf0 r1    # 期望 0x99
i2ctransfer -y 3 w2@0x31 0x40 0xf1 r1    # 期望 0x22

# 2. 驱动 probe（chip id 校验 + 4 路热插拔）
dmesg | grep -i xs9922
# 期望：detected chip id 0x9922 / dphy0 matches m00_b_xs9922 3-0031 / status 0xf

# 3. media 拓扑：m00_b_xs9922 3-0031 → dcphy0 → mipi0 → rkcif
media-ctl -p -d /dev/media0 | grep -A8 xs9922

# 4. 四路 CIF 节点（板端 udev 只生成 by-path，没有 by-name）
ls -l /dev/v4l/by-path/ | grep rkcif-mipi-lvds-video-index   # index0..3 对应 VC0~3

# 5. 抓第 0 路 50 帧（rkcif 直出，无需 ISP / 无需 rkaiq；支持 720p 缩放）
v4l2-ctl -d /dev/v4l/by-path/platform-rkcif-mipi-lvds-video-index0 \
    --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
    --stream-mmap=16 --stream-count=50 --stream-to=/tmp/ch0.nv12
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 1920x1080 -i /tmp/ch0.nv12 -frames:v 1 /tmp/ch0.jpg

# 6. 热插拔上报（拔掉一路 AHD 再看）
cat /sys/bus/i2c/devices/3-0031/hotplug_status    # 4 位掩码，bit0~3 对应 CH0~CH3

# 7. 切 720p25 / CVBS（默认是 1080p25）
media-ctl -d /dev/media0 --set-v4l2 "'m00_b_xs9922 3-0031':0[fmt:UYVY8_2X8/1280x720]"
```

排查要点：

- `i2ctransfer` 读不到 0x99 → 先量模组 FPC 座 pin22(1V8)/pin20(PDN_L)/pin19(REST) 电平，
  再示波器看 pin17/18 是否有波形；**26pin FPC 必须是反向连接**（转接板 pin N ↔ 模组
  pin 27−N，转接板原理图按翻折设计，用同向排线则 1V8 落在 D0P、I2C 落在 D1N/GND）
- `dmesg` 里 `xs9922: unexpected chip id` → 地址对但通信异常
- media 拓扑里 sensor 实体缺失 → 先检查 dmesg 有没有 `dphy0 matches m00_b_xs9922`；
  没有则说明驱动被编成模块了（必须内建，见 §8）
- 只有 id0 出图、id1~id3 无帧 → 检查 `0x?e08` 各通道 MIPI 使能与摄像头是否真的插上
  （`hotplug_status` 的对应 bit）
