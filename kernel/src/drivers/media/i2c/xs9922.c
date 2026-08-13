// SPDX-License-Identifier: GPL-2.0
/*
 * xs9922 - XS9922B 4 channel AHD/CVBS to MIPI CSI-2 bridge
 *
 * Copyright (C) 2021 Rockchip Electronics Co., Ltd.
 *
 * The four analog inputs are muxed onto a single 4-lane MIPI CSI-2 link as
 * virtual channels 0..3, so one subdev feeds four rkcif streams
 * (stream_cif_mipi_id0..id3).  Pixel format is UYVY8_2X8 (YUV422), no ISP
 * needed.
 *
 * Ported to LubanCat-3 (RK3576, MIPI CSI0) from the vendor driver shipped in
 * the rpdzkj RK3576 SDK: kernel-6.1/drivers/media/i2c/xs9922/xs9922.c.
 * Deviations from that driver (see docs/xs9922b_port.md for the rationale):
 *   - no /etc/board.conf parsing, no drm_display_mode injection and no private
 *     XS9922_SET_FMT ioctl; the active mode is selected through S_FMT only
 *   - hardware bring-up moved out of the config-file kthread into
 *     xs9922_init_hw(), called synchronously from probe()
 *   - xs9922_write_array() tolerates a NULL table, 720p25 got its missing
 *     audio table back
 *   - probe()/remove() unwind completely; dead code removed
 *
 * V0.0X01.0X00 first version.
 * V0.0X01.0X01 port to LubanCat-3 (RK3576), MIPI CSI0 4-lane.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/rk-camera-module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "xs9922_reg_cfg.h"
#include "xs9922_audio_reg_cfg.h"

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x01)

#define XS9922_NAME			"xs9922"
#define XS9922_XVCLK_FREQ		27000000
#define XS9922_LANES			4
#define XS9922_CH_NUM			4
#define XS9922_ALL_CH_MASK		0x0f

/*
 * MIPI runs at 1.5Gbps/lane with a continuous clock (see 0x511b in
 * xs9922_init_cfg).  DDR, hence link frequency = bit rate / 2.
 */
#define XS9922_LINK_FREQ_1500M		(1500000000UL >> 1)
#define XS9922_LINK_FREQ_1200M		(1200000000UL >> 1)

#define XS9922_REG_VALUE_08BIT		1

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

/* chip id, expected to read back 0x99 / 0x22 */
#define XS9922_REG_CHIP_ID_HI		0x40f0
#define XS9922_REG_CHIP_ID_LO		0x40f1
#define XS9922_CHIP_ID_HI		0x99
#define XS9922_CHIP_ID_LO		0x22

/* per channel registers live in 0x1000 sized pages: ch0 0x0xxx .. ch3 0x3xxx */
#define XS9922_CH_REG(ch, reg)		(((ch) << 12) | (reg))

#define XS9922_REG_VIDEO_STATUS		0x0000	/* bit4: video signal lost */
#define XS9922_VIDEO_STATUS_LOST	BIT(4)
#define XS9922_REG_CONTRAST		0x0106
#define XS9922_REG_BRIGHTNESS		0x0107
#define XS9922_REG_SATURATION		0x0108
#define XS9922_REG_HUE			0x0109
#define XS9922_REG_CH_MIPI_EN		0x0e08	/* 1: channel drives the link */

/* MIPI reset block, bit0 of 0x5007 releases the MIPI output */
#define XS9922_REG_MIPI_RST0		0x5004
#define XS9922_REG_MIPI_RST1		0x5005
#define XS9922_REG_MIPI_RST2		0x5006
#define XS9922_REG_MIPI_RST3		0x5007

#define to_xs9922(sd) container_of(sd, struct xs9922, subdev)

struct xs9922_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	u32 field;
	struct v4l2_fract max_fps;
	u32 mipi_freq_idx;
	u32 bpp;
	const struct regval *global_reg_list;
	const struct regval *reg_list;
	const struct regval *audio_reg_list;
	u32 hdr_mode;
	u32 lanes;
	u32 vc[PAD_MAX];
};

struct xs9922 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*power_gpio;
	struct gpio_desc	*cam_gpio;

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct mutex		mutex;	/* protects cur_mode / streaming */
	bool			power_on;
	const struct xs9922_mode *cur_mode;

	u32			module_index;
	u32			cfg_num;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;

	int			streaming;
	struct task_struct	*detect_thread;	/* hotplug detect worker */
	struct input_dev	*input_dev;
	unsigned char		detect_status;
	unsigned char		last_detect_status;
	u8			is_reset;
};

static const s64 link_freq_items[] = {
	XS9922_LINK_FREQ_1500M,
	XS9922_LINK_FREQ_1200M,
};

/*
 * .vc[] holds plain virtual channel indices, like nvp6188.c does: the
 * V4L2_MBUS_CSI2_CHANNEL_* flags the vendor driver used no longer exist in
 * this kernel's <media/v4l2-mediabus.h>.  rkcif does not actually read the
 * value back (rkcif_get_input_fmt() only calls get_fmt on pad 0 and falls back
 * to "vc = pad_id" because RKMODULE_GET_CHANNEL_INFO is not implemented here),
 * it is reported through fmt->reserved[0] for other BSP consumers.
 *
 * TODO: 1080p@30fps is missing.  The vendor register dump we have only covers
 * 25fps (xs9922_1080p_4lanes_25fps).  Once xs9922_1080p_4lanes_30fps[] is
 * available, add another entry here with .max_fps.denominator = 300000; no
 * structural change is needed.
 */
static const struct xs9922_mode supported_modes[] = {
	{
		/* AHD 1080p25, the default */
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_NONE,
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,
		},
		.global_reg_list = xs9922_init_cfg,
		.reg_list = xs9922_1080p_4lanes_25fps,
		.audio_reg_list = xs9922_audio_codec,
		.mipi_freq_idx = 0,
		.bpp = 8,
		.hdr_mode = NO_HDR,
		.lanes = XS9922_LANES,
		.vc[PAD0] = 0,
		.vc[PAD1] = 1,
		.vc[PAD2] = 2,
		.vc[PAD3] = 3,
	}, {
		/* AHD 720p25 */
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_NONE,
		.width = 1280,
		.height = 720,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,
		},
		.global_reg_list = xs9922_init_cfg,
		.reg_list = xs9922_720p_4lanes_25fps,
		.audio_reg_list = xs9922_audio_codec,
		.mipi_freq_idx = 0,
		.bpp = 8,
		.hdr_mode = NO_HDR,
		.lanes = XS9922_LANES,
		.vc[PAD0] = 0,
		.vc[PAD1] = 1,
		.vc[PAD2] = 2,
		.vc[PAD3] = 3,
	}, {
		/* CVBS PAL */
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_INTERLACED,
		.width = 720,
		.height = 576,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,
		},
		.global_reg_list = xs9922_init_cfg,
		.reg_list = xs9922_cvbs_720x576_25f_P,
		.audio_reg_list = xs9922_audio_codec,
		.mipi_freq_idx = 0,
		.bpp = 8,
		.hdr_mode = NO_HDR,
		.lanes = XS9922_LANES,
		.vc[PAD0] = 0,
		.vc[PAD1] = 1,
		.vc[PAD2] = 2,
		.vc[PAD3] = 3,
	}, {
		/* CVBS NTSC */
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_INTERLACED,
		.width = 720,
		.height = 480,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.global_reg_list = xs9922_init_cfg,
		.reg_list = xs9922_cvbs_720x480_30f_N,
		.audio_reg_list = xs9922_audio_codec,
		.mipi_freq_idx = 0,
		.bpp = 8,
		.hdr_mode = NO_HDR,
		.lanes = XS9922_LANES,
		.vc[PAD0] = 0,
		.vc[PAD1] = 1,
		.vc[PAD2] = 2,
		.vc[PAD3] = 3,
	},
};

/* Write up to 4 bytes to a 16bit register address */
static int xs9922_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2) {
		dev_err(&client->dev, "write reg(0x%04x) failed\n", reg);
		return -EIO;
	}

	return 0;
}

/* Read registers up to 4 at a time */
static int xs9922_read_reg(struct i2c_client *client, u16 reg,
			   unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(&client->dev, "read reg(0x%04x) failed\n", reg);
		return -EIO;
	}

	*val = be32_to_cpu(data_be);

	return 0;
}

/* NULL is accepted and means "nothing to program" */
static int xs9922_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	int ret = 0;
	u32 i;

	if (!regs)
		return 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		ret = xs9922_write_reg(client, regs[i].addr,
				       XS9922_REG_VALUE_08BIT, regs[i].val);
		if (regs[i].nDelay)
			msleep(regs[i].nDelay);
	}

	return ret;
}

static int xs9922_switch_mode(struct xs9922 *xs9922)
{
	struct i2c_client *client = xs9922->client;
	int ret;

	dev_dbg(&client->dev, "%s: %dx%d\n", __func__,
		xs9922->cur_mode->width, xs9922->cur_mode->height);

	ret = xs9922_write_array(client, xs9922->cur_mode->global_reg_list);
	ret |= xs9922_write_array(client, xs9922->cur_mode->reg_list);
	ret |= xs9922_write_array(client, xs9922->cur_mode->audio_reg_list);
	if (ret)
		dev_err(&client->dev, "failed to program mode registers\n");

	return ret;
}

/* Enable or disable the MIPI output of all four channels */
static int xs9922_set_ch_mipi_en(struct xs9922 *xs9922, bool on)
{
	unsigned int ch;
	int ret = 0;

	for (ch = 0; ch < XS9922_CH_NUM; ch++)
		ret |= xs9922_write_reg(xs9922->client,
					XS9922_CH_REG(ch, XS9922_REG_CH_MIPI_EN),
					XS9922_REG_VALUE_08BIT, on ? 0x01 : 0x00);

	return ret;
}

/* Hold (on == false) or release (on == true) the MIPI D-PHY output */
static int xs9922_mipi_output(struct xs9922 *xs9922, bool on)
{
	struct i2c_client *client = xs9922->client;
	int ret;

	ret = xs9922_write_reg(client, XS9922_REG_MIPI_RST0,
			       XS9922_REG_VALUE_08BIT, 0x00);
	ret |= xs9922_write_reg(client, XS9922_REG_MIPI_RST1,
				XS9922_REG_VALUE_08BIT, 0x00);
	ret |= xs9922_write_reg(client, XS9922_REG_MIPI_RST2,
				XS9922_REG_VALUE_08BIT, 0x00);
	ret |= xs9922_write_reg(client, XS9922_REG_MIPI_RST3,
				XS9922_REG_VALUE_08BIT, on ? 0x01 : 0x00);

	return ret;
}

/* detect_status bit n: channel n has a locked video signal */
static int xs9922_auto_detect_hotplug(struct xs9922 *xs9922)
{
	struct i2c_client *client = xs9922->client;
	unsigned char status = 0;
	unsigned int ch;
	int ret = 0;
	u32 val;

	for (ch = 0; ch < XS9922_CH_NUM; ch++) {
		ret = xs9922_read_reg(client,
				      XS9922_CH_REG(ch, XS9922_REG_VIDEO_STATUS),
				      XS9922_REG_VALUE_08BIT, &val);
		if (ret)
			return ret;

		if (!(val & XS9922_VIDEO_STATUS_LOST))
			status |= BIT(ch);
	}

	xs9922->detect_status = status;

	return 0;
}

static ssize_t hotplug_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct xs9922 *xs9922 = to_xs9922(sd);

	return sysfs_emit(buf, "%d\n", xs9922->detect_status);
}

/* Cuts/restores the AHD front-end supply of the adapter board */
static ssize_t cam_power_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct xs9922 *xs9922 = to_xs9922(sd);
	bool enable;
	int ret;

	if (IS_ERR_OR_NULL(xs9922->cam_gpio))
		return -ENODEV;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(xs9922->cam_gpio, enable);

	return count;
}

static DEVICE_ATTR_RO(hotplug_status);
static DEVICE_ATTR_WO(cam_power);

static struct attribute *dev_attrs[] = {
	&dev_attr_hotplug_status.attr,
	&dev_attr_cam_power.attr,
	NULL,
};

static const struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};

static int xs9922_get_reso_dist(const struct xs9922_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct xs9922_mode *
xs9922_find_best_fit(struct xs9922 *xs9922, struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;
	int dist;

	for (i = 0; i < xs9922->cfg_num; i++) {
		dist = xs9922_get_reso_dist(&supported_modes[i], framefmt);
		if ((cur_best_fit_dist == -1 || dist <= cur_best_fit_dist) &&
		    supported_modes[i].bus_fmt == framefmt->code) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int xs9922_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2_DPHY;
	cfg->bus.mipi_csi2.num_data_lanes = XS9922_LANES;

	return 0;
}

static int xs9922_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct xs9922 *xs9922 = to_xs9922(sd);
	const struct xs9922_mode *mode;
	u64 pixel_rate;

	mutex_lock(&xs9922->mutex);

	mode = xs9922_find_best_fit(xs9922, fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = mode->field;
	fmt->format.colorspace = V4L2_COLORSPACE_SMPTE170M;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&xs9922->mutex);
		return -ENOTTY;
#endif
	} else {
		xs9922->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(xs9922->link_freq, mode->mipi_freq_idx);
		/* pixel rate = link frequency * 2 * lanes / bits per sample */
		pixel_rate = (u32)link_freq_items[mode->mipi_freq_idx] /
			     mode->bpp * 2 * XS9922_LANES;
		__v4l2_ctrl_s_ctrl_int64(xs9922->pixel_rate, pixel_rate);
		dev_dbg(&xs9922->client->dev,
			"mipi_freq_idx %d, pixel_rate %lld\n",
			mode->mipi_freq_idx, pixel_rate);

		xs9922_switch_mode(xs9922);
		/* the receiver only re-syncs on frame headers after a MIPI reset */
		xs9922_write_array(xs9922->client, xs9922_mipi_reset_new);
	}

	mutex_unlock(&xs9922->mutex);

	return 0;
}

static int xs9922_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct xs9922 *xs9922 = to_xs9922(sd);
	struct i2c_client *client = xs9922->client;
	const struct xs9922_mode *mode = xs9922->cur_mode;

	mutex_lock(&xs9922->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
#else
		mutex_unlock(&xs9922->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = mode->field;
		fmt->format.colorspace = V4L2_COLORSPACE_SMPTE170M;
		/* rkcif reads the virtual channel of this pad from reserved[0] */
		if (fmt->pad < PAD_MAX)
			fmt->reserved[0] = mode->vc[fmt->pad];
		else
			fmt->reserved[0] = mode->vc[PAD0];
	}
	mutex_unlock(&xs9922->mutex);

	dev_dbg(&client->dev, "%s: %x %dx%d\n", __func__, fmt->format.code,
		fmt->format.width, fmt->format.height);

	return 0;
}

static int xs9922_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	/* every mode outputs the same media bus code */
	if (code->index)
		return -EINVAL;

	code->code = supported_modes[0].bus_fmt;

	return 0;
}

static int xs9922_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct xs9922 *xs9922 = to_xs9922(sd);

	if (fse->index >= xs9922->cfg_num)
		return -EINVAL;

	if (fse->code != supported_modes[fse->index].bus_fmt)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = supported_modes[fse->index].height;

	return 0;
}

static int xs9922_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct xs9922 *xs9922 = to_xs9922(sd);

	if (fie->index >= xs9922->cfg_num)
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	fie->reserved[0] = supported_modes[fie->index].hdr_mode;

	return 0;
}

static int xs9922_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct xs9922 *xs9922 = to_xs9922(sd);

	mutex_lock(&xs9922->mutex);
	fi->interval = xs9922->cur_mode->max_fps;
	mutex_unlock(&xs9922->mutex);

	return 0;
}

static void xs9922_get_module_inf(struct xs9922 *xs9922,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, XS9922_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, xs9922->module_name,
		sizeof(inf->base.module));
	strscpy(inf->base.lens, xs9922->len_name, sizeof(inf->base.lens));
}

static void xs9922_get_vc_hotplug_inf(struct xs9922 *xs9922,
				      struct rkmodule_vc_hotplug_info *inf)
{
	memset(inf, 0, sizeof(*inf));
	inf->detect_status = xs9922->detect_status;
}

static void xs9922_get_vicap_rst_inf(struct xs9922 *xs9922,
				     struct rkmodule_vicap_reset_info *rst_info)
{
	rst_info->is_reset = xs9922->is_reset;
	rst_info->src = RKCIF_RESET_SRC_ERR_HOTPLUG;
}

static void xs9922_set_vicap_rst_inf(struct xs9922 *xs9922,
				     struct rkmodule_vicap_reset_info rst_info)
{
	xs9922->is_reset = rst_info.is_reset;
}

static long xs9922_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct xs9922 *xs9922 = to_xs9922(sd);
	long ret = 0;
	u32 stream;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		xs9922_get_module_inf(xs9922, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_GET_VC_HOTPLUG_INFO:
		xs9922_get_vc_hotplug_inf(xs9922,
					  (struct rkmodule_vc_hotplug_info *)arg);
		break;
	case RKMODULE_GET_VICAP_RST_INFO:
		xs9922_get_vicap_rst_inf(xs9922,
					 (struct rkmodule_vicap_reset_info *)arg);
		break;
	case RKMODULE_SET_VICAP_RST_INFO:
		xs9922_set_vicap_rst_inf(xs9922,
					 *(struct rkmodule_vicap_reset_info *)arg);
		break;
	case RKMODULE_GET_START_STREAM_SEQ:
		*(int *)arg = RKMODULE_START_STREAM_FRONT;
		break;
	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);
		if (stream) {
			ret = xs9922_mipi_output(xs9922, true);
			ret |= xs9922_set_ch_mipi_en(xs9922, true);
		} else {
			ret = xs9922_set_ch_mipi_en(xs9922, false);
			ret |= xs9922_mipi_output(xs9922, false);
		}
		dev_dbg(&xs9922->client->dev, "quick stream %s\n",
			stream ? "on" : "off");
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long xs9922_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_vc_hotplug_info *vc_hp_inf;
	struct rkmodule_vicap_reset_info *vicap_rst_inf;
	struct rkmodule_inf *inf;
	long ret;
	u32 stream;
	int *seq;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;

		ret = xs9922_ioctl(sd, cmd, inf);
		if (!ret) {
			if (copy_to_user(up, inf, sizeof(*inf)))
				ret = -EFAULT;
		}
		kfree(inf);
		break;
	case RKMODULE_GET_VC_HOTPLUG_INFO:
		vc_hp_inf = kzalloc(sizeof(*vc_hp_inf), GFP_KERNEL);
		if (!vc_hp_inf)
			return -ENOMEM;

		ret = xs9922_ioctl(sd, cmd, vc_hp_inf);
		if (!ret) {
			if (copy_to_user(up, vc_hp_inf, sizeof(*vc_hp_inf)))
				ret = -EFAULT;
		}
		kfree(vc_hp_inf);
		break;
	case RKMODULE_GET_VICAP_RST_INFO:
		vicap_rst_inf = kzalloc(sizeof(*vicap_rst_inf), GFP_KERNEL);
		if (!vicap_rst_inf)
			return -ENOMEM;

		ret = xs9922_ioctl(sd, cmd, vicap_rst_inf);
		if (!ret) {
			if (copy_to_user(up, vicap_rst_inf, sizeof(*vicap_rst_inf)))
				ret = -EFAULT;
		}
		kfree(vicap_rst_inf);
		break;
	case RKMODULE_SET_VICAP_RST_INFO:
		vicap_rst_inf = kzalloc(sizeof(*vicap_rst_inf), GFP_KERNEL);
		if (!vicap_rst_inf)
			return -ENOMEM;

		if (copy_from_user(vicap_rst_inf, up, sizeof(*vicap_rst_inf)))
			ret = -EFAULT;
		else
			ret = xs9922_ioctl(sd, cmd, vicap_rst_inf);
		kfree(vicap_rst_inf);
		break;
	case RKMODULE_GET_START_STREAM_SEQ:
		seq = kzalloc(sizeof(*seq), GFP_KERNEL);
		if (!seq)
			return -ENOMEM;

		ret = xs9922_ioctl(sd, cmd, seq);
		if (!ret) {
			if (copy_to_user(up, seq, sizeof(*seq)))
				ret = -EFAULT;
		}
		kfree(seq);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(stream)))
			ret = -EFAULT;
		else
			ret = xs9922_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static void xs9922_report_hotplug(struct xs9922 *xs9922)
{
	struct i2c_client *client = xs9922->client;
	unsigned char bits;
	unsigned int ch;

	bits = xs9922->last_detect_status ^ xs9922->detect_status;
	for (ch = 0; ch < XS9922_CH_NUM; ch++) {
		if (!(bits & BIT(ch)))
			continue;
		dev_info(&client->dev, "channel %u %s\n", ch,
			 (xs9922->detect_status & BIT(ch)) ? "plugged in"
							  : "plugged out");
	}
}

static int detect_thread_function(void *data)
{
	struct xs9922 *xs9922 = (struct xs9922 *)data;
	struct i2c_client *client = xs9922->client;
	int need_reset_wait = -1;

	if (xs9922->power_on) {
		xs9922_auto_detect_hotplug(xs9922);
		xs9922->last_detect_status = xs9922->detect_status;
		xs9922->is_reset = 0;
	}

	while (!kthread_should_stop()) {
		if (xs9922->power_on) {
			xs9922_auto_detect_hotplug(xs9922);

			if (xs9922->last_detect_status != xs9922->detect_status) {
				if (need_reset_wait < 0) {
					xs9922_report_hotplug(xs9922);
					/* debounce: report after two more polls */
					need_reset_wait = 2;
				}
				if (--need_reset_wait == 0) {
					need_reset_wait = -1;
					xs9922->is_reset = 1;
					xs9922->last_detect_status =
						xs9922->detect_status;
					if (xs9922->input_dev) {
						input_event(xs9922->input_dev,
							    EV_MSC, MSC_RAW,
							    xs9922->detect_status);
						input_sync(xs9922->input_dev);
					}
					dev_info(&client->dev,
						 "hotplug settled, status 0x%x, vicap reset requested\n",
						 xs9922->detect_status);
				}
			} else {
				/* bounced back, no reset needed */
				need_reset_wait = -1;
			}
		}

		set_current_state(TASK_INTERRUPTIBLE);
		if (xs9922->detect_status == XS9922_ALL_CH_MASK)
			schedule_timeout(msecs_to_jiffies(100));
		else
			schedule_timeout(msecs_to_jiffies(1000));
	}

	return 0;
}

static int detect_thread_start(struct xs9922 *xs9922)
{
	struct i2c_client *client = xs9922->client;
	int ret;

	xs9922->detect_thread = kthread_create(detect_thread_function,
					       xs9922, "xs9922_kthread");
	if (IS_ERR(xs9922->detect_thread)) {
		ret = PTR_ERR(xs9922->detect_thread);
		xs9922->detect_thread = NULL;
		dev_err(&client->dev, "failed to create detect thread: %d\n", ret);
		return ret;
	}
	wake_up_process(xs9922->detect_thread);

	return 0;
}

static void detect_thread_stop(struct xs9922 *xs9922)
{
	if (xs9922->detect_thread)
		kthread_stop(xs9922->detect_thread);
	xs9922->detect_thread = NULL;
}

static int __xs9922_start_stream(struct xs9922 *xs9922)
{
	struct i2c_client *client = xs9922->client;
	int ret;

	/* re-sync the receiver on the frame headers */
	ret = xs9922_write_array(client, xs9922_mipi_reset_new);
	ret |= xs9922_write_array(client, xs9922_audio_codec);
	ret |= xs9922_set_ch_mipi_en(xs9922, true);

	/* the bridge needs ~200ms before the first complete frame comes out */
	usleep_range(200 * 1000, 400 * 1000);

	dev_dbg(&client->dev, "%s: ret %d\n", __func__, ret);

	return ret;
}

static int __xs9922_stop_stream(struct xs9922 *xs9922)
{
	return xs9922_set_ch_mipi_en(xs9922, false);
}

static int xs9922_stream(struct v4l2_subdev *sd, int on)
{
	struct xs9922 *xs9922 = to_xs9922(sd);
	struct i2c_client *client = xs9922->client;
	int ret = 0;

	dev_dbg(&client->dev, "%s: %d, %dx%d\n", __func__, on,
		xs9922->cur_mode->width, xs9922->cur_mode->height);

	mutex_lock(&xs9922->mutex);
	on = !!on;
	if (xs9922->streaming == on)
		goto unlock;

	if (on)
		ret = __xs9922_start_stream(xs9922);
	else
		ret = __xs9922_stop_stream(xs9922);

	if (!ret)
		xs9922->streaming = on;

unlock:
	mutex_unlock(&xs9922->mutex);

	return ret;
}

/*
 * The bridge is powered up once in probe() and stays up: the adapter board
 * requires CAM_REST / CAM_PDN_L to be held high while running, and cycling
 * them would drop the AHD lock of all four channels.  There is therefore no
 * runtime power management - s_power() only exists because rkcif calls it.
 */
static int xs9922_power(struct v4l2_subdev *sd, int on)
{
	struct xs9922 *xs9922 = to_xs9922(sd);

	dev_dbg(&xs9922->client->dev, "%s: on %d (no-op)\n", __func__, on);

	return 0;
}

static int __xs9922_power_on(struct xs9922 *xs9922)
{
	struct device *dev = &xs9922->client->dev;
	int ret;

	if (!IS_ERR_OR_NULL(xs9922->pins_default)) {
		ret = pinctrl_select_state(xs9922->pinctrl,
					   xs9922->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins. ret=%d\n", ret);
	}

	if (!IS_ERR_OR_NULL(xs9922->power_gpio)) {
		gpiod_set_value_cansleep(xs9922->power_gpio, 1);
		usleep_range(25 * 1000, 30 * 1000);
	}

	ret = clk_set_rate(xs9922->xvclk, XS9922_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate\n");
	if (clk_get_rate(xs9922->xvclk) != XS9922_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched\n");
	ret = clk_prepare_enable(xs9922->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		goto err_clk;
	}

	if (!IS_ERR_OR_NULL(xs9922->reset_gpio)) {
		gpiod_set_value_cansleep(xs9922->reset_gpio, 0);
		usleep_range(5 * 1000, 10 * 1000);
		gpiod_set_value_cansleep(xs9922->reset_gpio, 1);
		usleep_range(100 * 1000, 120 * 1000);
	}

	/* the chip needs another ~100ms before it answers on I2C */
	usleep_range(100 * 1000, 120 * 1000);

	xs9922->power_on = true;

	return 0;

err_clk:
	if (!IS_ERR_OR_NULL(xs9922->reset_gpio))
		gpiod_set_value_cansleep(xs9922->reset_gpio, 0);

	if (!IS_ERR_OR_NULL(xs9922->power_gpio))
		gpiod_set_value_cansleep(xs9922->power_gpio, 0);

	if (!IS_ERR_OR_NULL(xs9922->pins_sleep))
		pinctrl_select_state(xs9922->pinctrl, xs9922->pins_sleep);

	return ret;
}

/*
 * Release the hardware.  Only called when the driver gives up the device
 * (probe failure or remove) - never while the link is up, see xs9922_power().
 */
static void xs9922_hw_teardown(struct xs9922 *xs9922)
{
	if (!xs9922->power_on)
		return;

	if (!IS_ERR_OR_NULL(xs9922->cam_gpio))
		gpiod_set_value_cansleep(xs9922->cam_gpio, 0);
	if (!IS_ERR_OR_NULL(xs9922->reset_gpio))
		gpiod_set_value_cansleep(xs9922->reset_gpio, 0);
	if (!IS_ERR_OR_NULL(xs9922->power_gpio))
		gpiod_set_value_cansleep(xs9922->power_gpio, 0);

	clk_disable_unprepare(xs9922->xvclk);

	if (!IS_ERR_OR_NULL(xs9922->pins_sleep))
		pinctrl_select_state(xs9922->pinctrl, xs9922->pins_sleep);

	xs9922->power_on = false;
}

/* Picture controls are applied to all four channels at once */
static int xs9922_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xs9922 *xs9922 = container_of(ctrl->handler,
					     struct xs9922, ctrl_handler);
	struct i2c_client *client = xs9922->client;
	unsigned int ch;
	int ret = 0;
	u16 reg;
	u8 val;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		/* 0..255 (v4l2) -> -128..127 (chip) */
		reg = XS9922_REG_BRIGHTNESS;
		val = ctrl->val - 128;
		break;
	case V4L2_CID_CONTRAST:
		reg = XS9922_REG_CONTRAST;
		val = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		reg = XS9922_REG_SATURATION;
		val = ctrl->val;
		break;
	case V4L2_CID_HUE:
		/* 0..255 (v4l2) -> -128..127 (chip) */
		reg = XS9922_REG_HUE;
		val = ctrl->val - 128;
		break;
	default:
		dev_warn(&client->dev, "unhandled control 0x%x\n", ctrl->id);
		return -EINVAL;
	}

	for (ch = 0; ch < XS9922_CH_NUM; ch++)
		ret |= xs9922_write_reg(client, XS9922_CH_REG(ch, reg),
					XS9922_REG_VALUE_08BIT, val);

	return ret;
}

static const struct v4l2_ctrl_ops xs9922_ctrl_ops = {
	.s_ctrl = xs9922_s_ctrl,
};

static int xs9922_initialize_controls(struct xs9922 *xs9922)
{
	const struct xs9922_mode *mode = xs9922->cur_mode;
	struct v4l2_ctrl_handler *handler;
	u64 pixel_rate;
	int ret;

	handler = &xs9922->ctrl_handler;
	ret = v4l2_ctrl_handler_init(handler, 6);
	if (ret)
		return ret;
	handler->lock = &xs9922->mutex;

	xs9922->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(link_freq_items) - 1,
						   0, link_freq_items);
	__v4l2_ctrl_s_ctrl(xs9922->link_freq, mode->mipi_freq_idx);

	/* pixel rate = link frequency * 2 * lanes / bits per sample */
	pixel_rate = (u32)link_freq_items[mode->mipi_freq_idx] /
		     mode->bpp * 2 * XS9922_LANES;
	xs9922->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
					       V4L2_CID_PIXEL_RATE,
					       0, pixel_rate, 1, pixel_rate);

	v4l2_ctrl_new_std(handler, &xs9922_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, 0xff, 1, 0x80);
	v4l2_ctrl_new_std(handler, &xs9922_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, 0xff, 1, 0x80);
	v4l2_ctrl_new_std(handler, &xs9922_ctrl_ops,
			  V4L2_CID_SATURATION, 0, 0xff, 1, 0x80);
	v4l2_ctrl_new_std(handler, &xs9922_ctrl_ops,
			  V4L2_CID_HUE, 0, 0xff, 1, 0x80);

	if (handler->error) {
		ret = handler->error;
		dev_err(&xs9922->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	xs9922->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int xs9922_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct xs9922 *xs9922 = to_xs9922(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->state, 0);
	const struct xs9922_mode *def_mode = xs9922->cur_mode;

	mutex_lock(&xs9922->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = def_mode->field;
	mutex_unlock(&xs9922->mutex);
	/* No crop or compose */

	return 0;
}

static const struct v4l2_subdev_internal_ops xs9922_internal_ops = {
	.open = xs9922_open,
};
#endif

static const struct v4l2_subdev_video_ops xs9922_video_ops = {
	.s_stream = xs9922_stream,
	.g_frame_interval = xs9922_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops xs9922_subdev_pad_ops = {
	.enum_mbus_code = xs9922_enum_mbus_code,
	.enum_frame_size = xs9922_enum_frame_sizes,
	.enum_frame_interval = xs9922_enum_frame_interval,
	.get_fmt = xs9922_get_fmt,
	.set_fmt = xs9922_set_fmt,
	.get_mbus_config = xs9922_g_mbus_config,
};

static const struct v4l2_subdev_core_ops xs9922_core_ops = {
	.s_power = xs9922_power,
	.ioctl = xs9922_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = xs9922_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_ops xs9922_subdev_ops = {
	.core = &xs9922_core_ops,
	.video = &xs9922_video_ops,
	.pad   = &xs9922_subdev_pad_ops,
};

static int xs9922_check_chip_id(struct xs9922 *xs9922)
{
	struct device *dev = &xs9922->client->dev;
	u32 id_hi = 0;
	u32 id_lo = 0;
	int ret;

	ret = xs9922_read_reg(xs9922->client, XS9922_REG_CHIP_ID_HI,
			      XS9922_REG_VALUE_08BIT, &id_hi);
	ret |= xs9922_read_reg(xs9922->client, XS9922_REG_CHIP_ID_LO,
			       XS9922_REG_VALUE_08BIT, &id_lo);
	if (ret)
		return -EIO;

	if (id_hi != XS9922_CHIP_ID_HI || id_lo != XS9922_CHIP_ID_LO) {
		dev_err(dev, "unexpected chip id 0x%02x%02x, expected 0x%02x%02x\n",
			id_hi, id_lo, XS9922_CHIP_ID_HI, XS9922_CHIP_ID_LO);
		return -ENODEV;
	}

	dev_info(dev, "detected chip id 0x%02x%02x\n", id_hi, id_lo);

	return 0;
}

/*
 * Bring-up order is mandated by the vendor (references/xs9922b/readme.txt):
 * global init -> resolution table -> MIPI reset.  That note also asks for the
 * MIPI receiver to be up first, which cannot be guaranteed from probe(); what
 * makes it work anyway is that the MIPI reset sequence is repeated from
 * set_fmt() and from stream-on, once rkcif has configured the D-PHY.
 */
static int xs9922_init_hw(struct xs9922 *xs9922)
{
	int ret;

	/* keeps the AHD front end of the adapter board supplied */
	if (!IS_ERR_OR_NULL(xs9922->cam_gpio))
		gpiod_set_value_cansleep(xs9922->cam_gpio, 1);

	ret = xs9922_switch_mode(xs9922);
	ret |= xs9922_write_array(xs9922->client, xs9922_mipi_reset_new);

	return ret;
}

static int xs9922_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct v4l2_subdev *sd;
	struct xs9922 *xs9922;
	char facing[2];
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff);

	xs9922 = devm_kzalloc(dev, sizeof(*xs9922), GFP_KERNEL);
	if (!xs9922)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &xs9922->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &xs9922->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &xs9922->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &xs9922->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	xs9922->client = client;
	xs9922->cur_mode = &supported_modes[0];
	xs9922->cfg_num = ARRAY_SIZE(supported_modes);

	xs9922->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(xs9922->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	/*
	 * All three GPIOs are optional: on the LubanCat-3 CSI adapter board
	 * they may be pulled up on the board instead of being routed to the
	 * SoC.  Missing ones simply turn the corresponding steps into no-ops.
	 */
	xs9922->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(xs9922->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	xs9922->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(xs9922->power_gpio))
		dev_warn(dev, "Failed to get power-gpios\n");

	xs9922->cam_gpio = devm_gpiod_get(dev, "camera", GPIOD_OUT_LOW);
	if (IS_ERR(xs9922->cam_gpio))
		dev_warn(dev, "Failed to get camera-gpios\n");

	xs9922->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(xs9922->pinctrl)) {
		xs9922->pins_default =
			pinctrl_lookup_state(xs9922->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(xs9922->pins_default))
			dev_info(dev, "could not get default pinstate\n");

		xs9922->pins_sleep =
			pinctrl_lookup_state(xs9922->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(xs9922->pins_sleep))
			dev_info(dev, "could not get sleep pinstate\n");
	} else {
		dev_info(dev, "no pinctrl\n");
	}

	mutex_init(&xs9922->mutex);

	sd = &xs9922->subdev;
	v4l2_i2c_subdev_init(sd, client, &xs9922_subdev_ops);
	ret = xs9922_initialize_controls(xs9922);
	if (ret) {
		dev_err(dev, "Failed to initialize controls\n");
		goto err_destroy_mutex;
	}

	ret = __xs9922_power_on(xs9922);
	if (ret) {
		dev_err(dev, "Failed to power on\n");
		goto err_free_handler;
	}

	ret = xs9922_check_chip_id(xs9922);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &xs9922_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif

#if defined(CONFIG_MEDIA_CONTROLLER)
	xs9922->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &xs9922->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(xs9922->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 xs9922->module_index, facing, XS9922_NAME, dev_name(sd->dev));

	ret = sysfs_create_group(&dev->kobj, &dev_attr_grp);
	if (ret) {
		dev_err(dev, "Failed to create sysfs group\n");
		goto err_clean_entity;
	}

	xs9922->input_dev = devm_input_allocate_device(dev);
	if (!xs9922->input_dev) {
		dev_err(dev, "Failed to allocate input device\n");
		ret = -ENOMEM;
		goto err_sysfs;
	}
	xs9922->input_dev->name = "xs9922_input_event";
	set_bit(EV_MSC, xs9922->input_dev->evbit);
	set_bit(MSC_RAW, xs9922->input_dev->mscbit);

	/* devm managed: unregistered automatically when the device goes away */
	ret = input_register_device(xs9922->input_dev);
	if (ret) {
		dev_err(dev, "Failed to register input device\n");
		goto err_sysfs;
	}

	/* ~600 register writes, takes a few hundred ms on the I2C bus */
	ret = xs9922_init_hw(xs9922);
	if (ret) {
		dev_err(dev, "Failed to initialize hardware\n");
		goto err_sysfs;
	}

	ret = detect_thread_start(xs9922);
	if (ret)
		goto err_sysfs;

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_thread;
	}

	return 0;

err_thread:
	detect_thread_stop(xs9922);
err_sysfs:
	sysfs_remove_group(&dev->kobj, &dev_attr_grp);
err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	xs9922_hw_teardown(xs9922);
err_free_handler:
	v4l2_ctrl_handler_free(&xs9922->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&xs9922->mutex);

	return ret;
}

static void xs9922_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct xs9922 *xs9922 = to_xs9922(sd);

	detect_thread_stop(xs9922);
	v4l2_async_unregister_subdev(sd);
	sysfs_remove_group(&client->dev.kobj, &dev_attr_grp);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&xs9922->ctrl_handler);
	xs9922_hw_teardown(xs9922);
	mutex_destroy(&xs9922->mutex);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id xs9922_of_match[] = {
	{ .compatible = "xs9922" },
	{},
};
MODULE_DEVICE_TABLE(of, xs9922_of_match);
#endif

static const struct i2c_device_id xs9922_match_id[] = {
	{ "xs9922", 0 },
	{ },
};

static struct i2c_driver xs9922_i2c_driver = {
	.driver = {
		.name = XS9922_NAME,
		.of_match_table = of_match_ptr(xs9922_of_match),
	},
	.probe		= &xs9922_probe,
	.remove		= &xs9922_remove,
	.id_table	= xs9922_match_id,
};

module_i2c_driver(xs9922_i2c_driver);

MODULE_AUTHOR("hardy <yangjianzhong@percherry.com>");
MODULE_DESCRIPTION("XS9922B 4ch AHD to MIPI CSI-2 bridge driver");
MODULE_LICENSE("GPL v2");
