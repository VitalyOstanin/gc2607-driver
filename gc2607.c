// SPDX-License-Identifier: GPL-2.0
/*
 * GalaxyCore GC2607 CMOS image sensor driver
 *
 * Copyright (C) 2026
 *
 * A V4L2 sub-device driver for the GalaxyCore GC2607 colour sensor as found
 * in the Huawei MateBook X Pro 2024 (VGHH-XX), connected to the Intel IPU6
 * (Meteor Lake) image signal processor through an INT3472 "discrete" PMIC.
 *
 * The register sequences in gc2607-regs.h configure the native 1928x1088 mode
 * at 30 fps over 2 MIPI CSI-2 data lanes, 10-bit GRBG Bayer, with a 19.2 MHz
 * external clock.
 *
 * The driver follows the kernel's camera sensor guidelines:
 *   Documentation/driver-api/media/camera-sensor.rst
 *   Documentation/userspace-api/media/drivers/camera-sensor.rst
 *
 * Register access uses the V4L2 CCI helpers (<media/v4l2-cci.h>).  Frame rate
 * is controlled through V4L2_CID_VBLANK; exposure and gain are exposed as
 * standard controls.  Runtime PM manages sensor power.  The privacy LED is
 * handled automatically by the V4L2 core once the sub-device is registered
 * with v4l2_async_register_subdev_sensor().
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-async.h>

#include "gc2607-regs.h"

/* Chip identification: registers 0x03f0/0x03f1 read back as 0x2607. */
#define GC2607_REG_CHIP_ID		CCI_REG16(0x03f0)
#define GC2607_CHIP_ID			0x2607

/* Exposure: integration time in lines, 16-bit, big-endian (0x0202/0x0203). */
#define GC2607_REG_EXPOSURE		CCI_REG16(0x0202)
#define GC2607_EXPOSURE_MIN		4
#define GC2607_EXPOSURE_MARGIN		4	/* exposure must stay below VTS */
#define GC2607_EXPOSURE_STEP		1
#define GC2607_EXPOSURE_DEFAULT		600

/*
 * Analogue/digital gain is programmed through a calibrated look-up table
 * (carried over from the GalaxyCore reference driver).  The control value is
 * the LUT index; index 0 is unity gain.
 */
#define GC2607_REG_AGAIN_H		CCI_REG8(0x02b3)
#define GC2607_REG_AGAIN_L		CCI_REG8(0x02b4)
#define GC2607_REG_DGAIN_INT		CCI_REG8(0x020c)
#define GC2607_REG_DGAIN_FRAC		CCI_REG8(0x020d)

/*
 * Frame length (VTS).  The sensor exposes two frame-length register pairs that
 * the factory configuration keeps identical: 0x0340/0x0341 and 0x0220/0x0221.
 * Both are written together so that the frame timing and the exposure window
 * stay consistent.
 */
#define GC2607_REG_VTS_A		CCI_REG16(0x0340)
#define GC2607_REG_VTS_B		CCI_REG16(0x0220)

/* Sensor timing for the native mode (see gc2607-regs.h). */
#define GC2607_NATIVE_WIDTH		1928
#define GC2607_NATIVE_HEIGHT		1088
#define GC2607_HTS			2745	/* horizontal total size, pixels */
#define GC2607_VTS_DEFAULT		1250	/* vertical total size, lines  */
#define GC2607_VTS_MAX			0x3fff

/*
 * MIPI CSI-2 link: 2 data lanes, 10 bits per pixel.
 *   pixel_rate = link_freq * 2 (DDR) * lanes / bpp
 *   link_freq  = pixel_rate * bpp / (2 * lanes)
 *   frame rate = pixel_rate / (HTS * VTS) = 102937500 / (2745 * 1250) = 30 fps
 */
#define GC2607_LINK_FREQ		257343750LL
#define GC2607_PIXEL_RATE		102937500LL
#define GC2607_DATA_LANES		2

/* Default blanking derived from the native timing. */
#define GC2607_HBLANK_DEFAULT		(GC2607_HTS - GC2607_NATIVE_WIDTH)	/* 817 */
#define GC2607_VBLANK_MIN		(GC2607_VTS_DEFAULT - GC2607_NATIVE_HEIGHT) /* 162 */
#define GC2607_VBLANK_MAX		(GC2607_VTS_MAX - GC2607_NATIVE_HEIGHT)

#define GC2607_MBUS_CODE		MEDIA_BUS_FMT_SGRBG10_1X10

/* Driver/runtime parameters. */
#define GC2607_XCLK_FREQ		19200000	/* external clock, Hz       */
#define GC2607_REG_ADDR_BITS		16		/* CCI register address bits */
#define GC2607_BOOT_DELAY_US		10000		/* settle after reset release */
#define GC2607_AUTOSUSPEND_DELAY_MS	1000

/* Gain look-up table: maps LUT index -> {again_h, again_l, dgain_int, dgain_frac}. */
struct gc2607_gain_lut {
	u8 again_h;
	u8 again_l;
	u8 dgain_int;
	u8 dgain_frac;
};

static const struct gc2607_gain_lut gc2607_gain_table[] = {
	{ 0x00, 0x00, 0x00, 0x40 },	/* index 0  - unity */
	{ 0x05, 0x00, 0x00, 0x4b },	/* index 1  */
	{ 0x00, 0x01, 0x00, 0x59 },	/* index 2  */
	{ 0x05, 0x01, 0x00, 0x6a },	/* index 3  */
	{ 0x00, 0x02, 0x00, 0x80 },	/* index 4  */
	{ 0x05, 0x02, 0x00, 0x97 },	/* index 5  */
	{ 0x00, 0x03, 0x00, 0xb3 },	/* index 6  */
	{ 0x05, 0x03, 0x00, 0xd4 },	/* index 7  */
	{ 0x00, 0x04, 0x01, 0x00 },	/* index 8  */
	{ 0x05, 0x04, 0x01, 0x2f },	/* index 9  */
	{ 0x00, 0x05, 0x01, 0x66 },	/* index 10 */
	{ 0x05, 0x05, 0x01, 0xa8 },	/* index 11 */
	{ 0x00, 0x06, 0x02, 0x00 },	/* index 12 */
	{ 0x05, 0x06, 0x02, 0x5e },	/* index 13 */
	{ 0x09, 0x26, 0x02, 0xcc },	/* index 14 */
	{ 0x0c, 0xb6, 0x03, 0x50 },	/* index 15 */
	{ 0x10, 0x06, 0x04, 0x00 },	/* index 16 - highest */
};

#define GC2607_GAIN_MIN		0
#define GC2607_GAIN_MAX		(ARRAY_SIZE(gc2607_gain_table) - 1)
#define GC2607_GAIN_STEP	1
#define GC2607_GAIN_DEFAULT	0

/*
 * Regulator supplies.  On this INT3472-based platform only the analogue
 * supply ("avdd") is switchable through the PMIC; the interface and digital
 * core rails are always on and are not described as regulators in firmware,
 * so only "avdd" is requested.
 */
static const char * const gc2607_supply_names[] = {
	"avdd",
};

#define GC2607_NUM_SUPPLIES	ARRAY_SIZE(gc2607_supply_names)

static const s64 gc2607_link_freq_menu[] = {
	GC2607_LINK_FREQ,
};

struct gc2607 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[GC2607_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
};

static inline struct gc2607 *to_gc2607(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc2607, sd);
}

/* --------------------------------------------------------------------------
 * Power management
 * --------------------------------------------------------------------------
 */
static int gc2607_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc2607 *gc2607 = to_gc2607(sd);
	int ret;

	ret = regulator_bulk_enable(GC2607_NUM_SUPPLIES, gc2607->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(gc2607->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		goto disable_regulators;
	}

	/* Release the sensor from reset (the descriptor encodes active-low). */
	gpiod_set_value_cansleep(gc2607->reset_gpio, 0);

	/* Wait for the sensor's internal boot before any register access. */
	usleep_range(GC2607_BOOT_DELAY_US, GC2607_BOOT_DELAY_US + 1000);

	return 0;

disable_regulators:
	regulator_bulk_disable(GC2607_NUM_SUPPLIES, gc2607->supplies);
	return ret;
}

static int gc2607_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc2607 *gc2607 = to_gc2607(sd);

	gpiod_set_value_cansleep(gc2607->reset_gpio, 1);
	clk_disable_unprepare(gc2607->xclk);
	regulator_bulk_disable(GC2607_NUM_SUPPLIES, gc2607->supplies);

	return 0;
}

/* --------------------------------------------------------------------------
 * V4L2 controls
 * --------------------------------------------------------------------------
 */
static int gc2607_set_gain(struct gc2607 *gc2607, u32 index)
{
	const struct gc2607_gain_lut *lut = &gc2607_gain_table[index];
	int ret = 0;

	cci_write(gc2607->regmap, GC2607_REG_AGAIN_H, lut->again_h, &ret);
	cci_write(gc2607->regmap, GC2607_REG_AGAIN_L, lut->again_l, &ret);
	cci_write(gc2607->regmap, GC2607_REG_DGAIN_INT, lut->dgain_int, &ret);
	cci_write(gc2607->regmap, GC2607_REG_DGAIN_FRAC, lut->dgain_frac, &ret);

	return ret;
}

static int gc2607_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc2607 *gc2607 =
		container_of(ctrl->handler, struct gc2607, ctrl_handler);
	struct device *dev = gc2607->sd.dev;
	u32 vts;
	int ret;

	/*
	 * When the vertical blanking changes the frame length changes too, so
	 * the maximum exposure (which must stay below the frame length) has to
	 * be adjusted.  This must happen regardless of the power state.
	 */
	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = GC2607_NATIVE_HEIGHT + ctrl->val - GC2607_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(gc2607->exposure,
					 GC2607_EXPOSURE_MIN, max,
					 GC2607_EXPOSURE_STEP,
					 min_t(s64, gc2607->exposure->val, max));
	}

	/*
	 * Only touch hardware while the sensor is powered.  A non-positive
	 * return means no reference was taken (0: suspended; <0: runtime PM
	 * disabled) -- skip the register writes and, crucially, do not call
	 * pm_runtime_put() below for a reference we never acquired.
	 */
	if (pm_runtime_get_if_in_use(dev) <= 0)
		return 0;

	/*
	 * cci_write() takes the error accumulator as an in/out parameter and
	 * becomes a no-op once it holds a non-zero value, so it must start at 0
	 * for the EXPOSURE and VBLANK branches below.
	 */
	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(gc2607->regmap, GC2607_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc2607_set_gain(gc2607, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		vts = GC2607_NATIVE_HEIGHT + ctrl->val;
		cci_write(gc2607->regmap, GC2607_REG_VTS_A, vts, &ret);
		cci_write(gc2607->regmap, GC2607_REG_VTS_B, vts, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops gc2607_ctrl_ops = {
	.s_ctrl = gc2607_s_ctrl,
};

static int gc2607_init_controls(struct gc2607 *gc2607)
{
	struct v4l2_ctrl_handler *hdl = &gc2607->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	int ret;

	/* 6 device controls + up to 2 fwnode properties (orientation, rotation). */
	ret = v4l2_ctrl_handler_init(hdl, 8);
	if (ret)
		return ret;

	gc2607->link_freq =
		v4l2_ctrl_new_int_menu(hdl, &gc2607_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(gc2607_link_freq_menu) - 1, 0,
				       gc2607_link_freq_menu);
	if (gc2607->link_freq)
		gc2607->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc2607->pixel_rate =
		v4l2_ctrl_new_std(hdl, &gc2607_ctrl_ops, V4L2_CID_PIXEL_RATE,
				  GC2607_PIXEL_RATE, GC2607_PIXEL_RATE, 1,
				  GC2607_PIXEL_RATE);
	if (gc2607->pixel_rate)
		gc2607->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Horizontal blanking is fixed for the single supported mode. */
	gc2607->hblank =
		v4l2_ctrl_new_std(hdl, &gc2607_ctrl_ops, V4L2_CID_HBLANK,
				  GC2607_HBLANK_DEFAULT, GC2607_HBLANK_DEFAULT,
				  1, GC2607_HBLANK_DEFAULT);
	if (gc2607->hblank)
		gc2607->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Vertical blanking controls the frame rate. */
	gc2607->vblank =
		v4l2_ctrl_new_std(hdl, &gc2607_ctrl_ops, V4L2_CID_VBLANK,
				  GC2607_VBLANK_MIN, GC2607_VBLANK_MAX, 1,
				  GC2607_VBLANK_MIN);

	gc2607->exposure =
		v4l2_ctrl_new_std(hdl, &gc2607_ctrl_ops, V4L2_CID_EXPOSURE,
				  GC2607_EXPOSURE_MIN,
				  GC2607_VTS_DEFAULT - GC2607_EXPOSURE_MARGIN,
				  GC2607_EXPOSURE_STEP, GC2607_EXPOSURE_DEFAULT);

	gc2607->gain =
		v4l2_ctrl_new_std(hdl, &gc2607_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				  GC2607_GAIN_MIN, GC2607_GAIN_MAX,
				  GC2607_GAIN_STEP, GC2607_GAIN_DEFAULT);

	/* Orientation and rotation from firmware (DisCo for Imaging). */
	ret = v4l2_fwnode_device_parse(gc2607->sd.dev, &props);
	if (ret)
		goto err;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &gc2607_ctrl_ops, &props);
	if (ret)
		goto err;

	if (hdl->error) {
		ret = hdl->error;
		dev_err(gc2607->sd.dev, "control init failed: %d\n", ret);
		goto err;
	}

	gc2607->sd.ctrl_handler = hdl;
	return 0;

err:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

/* --------------------------------------------------------------------------
 * V4L2 sub-device pad operations
 * --------------------------------------------------------------------------
 */
static void gc2607_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = GC2607_NATIVE_WIDTH;
	fmt->height = GC2607_NATIVE_HEIGHT;
	fmt->code = GC2607_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int gc2607_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	gc2607_fill_format(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static int gc2607_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = GC2607_MBUS_CODE;
	return 0;
}

static int gc2607_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != GC2607_MBUS_CODE)
		return -EINVAL;

	fse->min_width = GC2607_NATIVE_WIDTH;
	fse->max_width = GC2607_NATIVE_WIDTH;
	fse->min_height = GC2607_NATIVE_HEIGHT;
	fse->max_height = GC2607_NATIVE_HEIGHT;
	return 0;
}

static int gc2607_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	/* Only one mode is supported; force it. */
	gc2607_fill_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;
	return 0;
}

static int gc2607_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = GC2607_NATIVE_WIDTH;
		sel->r.height = GC2607_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_pad_ops gc2607_pad_ops = {
	.enum_mbus_code = gc2607_enum_mbus_code,
	.enum_frame_size = gc2607_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = gc2607_set_fmt,
	.get_selection = gc2607_get_selection,
};

/* --------------------------------------------------------------------------
 * V4L2 sub-device video operations
 * --------------------------------------------------------------------------
 */
static int gc2607_start_streaming(struct gc2607 *gc2607)
{
	struct device *dev = gc2607->sd.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(gc2607->regmap,
				  gc2607_init_1928x1088_30fps,
				  ARRAY_SIZE(gc2607_init_1928x1088_30fps), NULL);
	if (ret) {
		dev_err(dev, "failed to write init registers: %d\n", ret);
		goto err;
	}

	ret = __v4l2_ctrl_handler_setup(&gc2607->ctrl_handler);
	if (ret) {
		dev_err(dev, "failed to apply controls: %d\n", ret);
		goto err;
	}

	ret = cci_multi_reg_write(gc2607->regmap, gc2607_stream_on,
				  ARRAY_SIZE(gc2607_stream_on), NULL);
	if (ret) {
		dev_err(dev, "failed to write stream-on registers: %d\n", ret);
		goto err;
	}

	return 0;

err:
	pm_runtime_put(dev);
	return ret;
}

static int gc2607_stop_streaming(struct gc2607 *gc2607)
{
	struct device *dev = gc2607->sd.dev;
	int ret;

	ret = cci_multi_reg_write(gc2607->regmap, gc2607_stream_off,
				  ARRAY_SIZE(gc2607_stream_off), NULL);
	if (ret)
		dev_err(dev, "failed to write stream-off registers: %d\n", ret);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int gc2607_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gc2607 *gc2607 = to_gc2607(sd);
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable)
		ret = gc2607_start_streaming(gc2607);
	else
		ret = gc2607_stop_streaming(gc2607);

	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct v4l2_subdev_video_ops gc2607_video_ops = {
	.s_stream = gc2607_s_stream,
};

static const struct v4l2_subdev_ops gc2607_subdev_ops = {
	.video = &gc2607_video_ops,
	.pad = &gc2607_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc2607_internal_ops = {
	.init_state = gc2607_init_state,
};

/* --------------------------------------------------------------------------
 * Probe / remove
 * --------------------------------------------------------------------------
 */
static int gc2607_identify(struct gc2607 *gc2607)
{
	struct device *dev = gc2607->sd.dev;
	u64 id;
	int ret = 0;

	cci_read(gc2607->regmap, GC2607_REG_CHIP_ID, &id, &ret);
	if (ret) {
		dev_err(dev, "failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (id != GC2607_CHIP_ID) {
		dev_err(dev, "wrong chip ID 0x%04llx (expected 0x%04x)\n",
			id, GC2607_CHIP_ID);
		return -ENODEV;
	}

	dev_dbg(dev, "GC2607 detected (chip ID 0x%04llx)\n", id);
	return 0;
}

/*
 * Validate the CSI-2 endpoint description from firmware: the number of data
 * lanes and the advertised link frequency must match what the driver drives.
 */
static int gc2607_check_hwcfg(struct device *dev)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct fwnode_handle *ep;
	unsigned int i;
	int ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != GC2607_DATA_LANES) {
		ret = dev_err_probe(dev, -EINVAL,
				    "%u data lanes configured, expected %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes,
				    GC2607_DATA_LANES);
		goto out;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		ret = dev_err_probe(dev, -EINVAL,
				    "no link frequencies defined\n");
		goto out;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == GC2607_LINK_FREQ)
			break;

	if (i == bus_cfg.nr_of_link_frequencies)
		ret = dev_err_probe(dev, -EINVAL,
				    "link frequency %lld not supported\n",
				    GC2607_LINK_FREQ);

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int gc2607_get_resources(struct gc2607 *gc2607, struct device *dev)
{
	unsigned long xclk_rate;
	unsigned int i;
	int ret;

	gc2607->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(gc2607->xclk))
		return dev_err_probe(dev, PTR_ERR(gc2607->xclk),
				     "failed to get clock\n");

	/*
	 * On ACPI platforms devm_v4l2_sensor_clk_get() registers a fixed clock
	 * from the firmware "clock-frequency" property, so the reported rate
	 * reflects what the platform supplies.  Validate it when known; a rate of
	 * 0 means the provider does not report one, in which case trust firmware.
	 */
	xclk_rate = clk_get_rate(gc2607->xclk);
	if (xclk_rate && xclk_rate != GC2607_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "external clock %lu Hz, expected %u Hz\n",
				     xclk_rate, GC2607_XCLK_FREQ);

	for (i = 0; i < GC2607_NUM_SUPPLIES; i++)
		gc2607->supplies[i].supply = gc2607_supply_names[i];

	ret = devm_regulator_bulk_get(dev, GC2607_NUM_SUPPLIES,
				      gc2607->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* Reset is asserted (held in reset) until power-on releases it. */
	gc2607->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(gc2607->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(gc2607->reset_gpio),
				     "failed to get reset GPIO\n");

	return 0;
}

static int gc2607_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc2607 *gc2607;
	int ret;

	ret = gc2607_check_hwcfg(dev);
	if (ret)
		return ret;

	gc2607 = devm_kzalloc(dev, sizeof(*gc2607), GFP_KERNEL);
	if (!gc2607)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&gc2607->sd, client, &gc2607_subdev_ops);
	gc2607->sd.internal_ops = &gc2607_internal_ops;

	gc2607->regmap = devm_cci_regmap_init_i2c(client, GC2607_REG_ADDR_BITS);
	if (IS_ERR(gc2607->regmap))
		return dev_err_probe(dev, PTR_ERR(gc2607->regmap),
				     "failed to init CCI regmap\n");

	ret = gc2607_get_resources(gc2607, dev);
	if (ret)
		return ret;

	/* Power on to probe the chip ID. */
	ret = gc2607_power_on(dev);
	if (ret)
		return ret;

	ret = gc2607_identify(gc2607);
	if (ret)
		goto power_off;

	ret = gc2607_init_controls(gc2607);
	if (ret)
		goto power_off;

	gc2607->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gc2607->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	gc2607->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&gc2607->sd.entity, 1, &gc2607->pad);
	if (ret) {
		dev_err(dev, "failed to init media entity: %d\n", ret);
		goto free_ctrls;
	}

	gc2607->sd.state_lock = gc2607->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&gc2607->sd);
	if (ret) {
		dev_err(dev, "failed to finalize subdev: %d\n", ret);
		goto media_cleanup;
	}

	/*
	 * Enable runtime PM with autosuspend.  The device was powered on above
	 * for identification, so mark it active and let autosuspend power it
	 * down shortly after probe.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, GC2607_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&gc2607->sd);
	if (ret) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto pm_disable;
	}

	return 0;

pm_disable:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&gc2607->sd);
media_cleanup:
	media_entity_cleanup(&gc2607->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&gc2607->ctrl_handler);
power_off:
	gc2607_power_off(dev);
	return ret;
}

static void gc2607_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = dev_get_drvdata(&client->dev);
	struct gc2607 *gc2607 = to_gc2607(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&gc2607->ctrl_handler);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		gc2607_power_off(dev);
	pm_runtime_set_suspended(dev);
}

static const struct dev_pm_ops gc2607_pm_ops = {
	SET_RUNTIME_PM_OPS(gc2607_power_off, gc2607_power_on, NULL)
};

static const struct acpi_device_id gc2607_acpi_ids[] = {
	{ "GCTI2607" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, gc2607_acpi_ids);

static struct i2c_driver gc2607_i2c_driver = {
	.driver = {
		.name = "gc2607",
		.pm = &gc2607_pm_ops,
		.acpi_match_table = gc2607_acpi_ids,
	},
	.probe = gc2607_probe,
	.remove = gc2607_remove,
};
module_i2c_driver(gc2607_i2c_driver);

MODULE_DESCRIPTION("GalaxyCore GC2607 sensor driver");
MODULE_LICENSE("GPL");
