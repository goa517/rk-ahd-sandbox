

XS9922_openMipi_stream ：0，1，2，3通道MIPI数据开关，1=OPEN，0=close，，可以根据实际需要进行配置。

xs9922_init_cfg：9922B初始化，MIPI 频率1.5G，4lane，主控端应该配置成1.5G或更高，如果主控端最大1.2G，则9922B需要调整为1.2G.

xs9922_1080p_4lanes_25fps：AHD1080P@25帧 0，1，2，3通道初始化 

xs9922_720p_4lanes_25fps：AHD720P@25帧 0，1，2，3通道初始化 

xs9922_mipi_reset_new：mipi复位，可以从新获取帧头信息。

9922B MIPI与主控MIPI配置注意：
1，MIPI有连续模式和非连续模式，配置需要两边匹配。不能用连续模式的配置输出接主控MIPI端的非连续模式。
2，非连续模式带宽比较受限，建议尽量修改为使用连续模式。
3，9922B没特别说明，配置的是连续模式

初始化建议：
主控的MIPI先初始化完后再到9922B初始化。只要针对一些主控（如NXP,TI）：主控MIPI未初始化前，MIPI接口不允许有数据。

初始化顺序：

xs9922_init_cfg---根据接入的AHD摄像头像素选择：xs9922_1080p_4lanes_25fps（或者xs9922_720p_4lanes_25fps）---xs9922_mipi_reset_new