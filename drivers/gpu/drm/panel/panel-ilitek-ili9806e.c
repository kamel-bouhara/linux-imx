// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2018, Bootlin
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/backlight.h>
#include <linux/media-bus-format.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>
#include <video/display_timing.h>
#include <video/videomode.h>

#define DSI_EN	11

struct ili9806e {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;
	struct videomode vm;
	struct backlight_device *backlight;
	struct regulator	*power;
	struct gpio_desc	*reset;
};

enum ili9806e_op {
	ILI9806E_SWITCH_PAGE,
	ILI9806E_COMMAND,
};

static const u32 ilitek_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
};

struct ili9806e_instr {
	enum ili9806e_op	op;

	union arg {
		struct cmd {
			u8	cmd;
			u8	data;
		} cmd;
		u8	page;
	} arg;
};

#define ILI9806E_SWITCH_PAGE_INSTR(_page)	\
	{					\
		.op = ILI9806E_SWITCH_PAGE,	\
		.arg = {			\
			.page = (_page),	\
		},				\
	}

#define ILI9806E_COMMAND_INSTR(_cmd, _data)		\
	{						\
		.op = ILI9806E_COMMAND,		\
		.arg = {				\
			.cmd = {			\
				.cmd = (_cmd),		\
				.data = (_data),	\
			},				\
		},					\
	}

static const struct ili9806e_instr ili9806e_init[] = {
	ILI9806E_SWITCH_PAGE_INSTR(0x01),     // Change to Page 1
	ILI9806E_COMMAND_INSTR(0x08, 0x10),     // output SDA
	ILI9806E_COMMAND_INSTR(0x21, 0x01),     // DE = 1 Active
	ILI9806E_COMMAND_INSTR(0x30, 0x02),     // 480 X 800
	ILI9806E_COMMAND_INSTR(0x31, 0x02),     // 2-dot Inversion
	ILI9806E_COMMAND_INSTR(0x40, 0x16),     // AVDD/AVEE  
	ILI9806E_COMMAND_INSTR(0x41, 0x33),     // AVDD/AVEE 
	ILI9806E_COMMAND_INSTR(0x42, 0x02),     // VGH/VGL 
	ILI9806E_COMMAND_INSTR(0x43, 0x09),     // VGH 
	ILI9806E_COMMAND_INSTR(0x44, 0x09),     // VGL  
	ILI9806E_COMMAND_INSTR(0x50, 0x78),     // VGMP 4.5V
	ILI9806E_COMMAND_INSTR(0x51, 0x78),     // VGMN 4.5V
	ILI9806E_COMMAND_INSTR(0x52, 0x00),     //Flicker
	ILI9806E_COMMAND_INSTR(0x53, 0x5E),     //Flicker 
	ILI9806E_COMMAND_INSTR(0x60, 0x07),     // SDTI
	ILI9806E_COMMAND_INSTR(0x61, 0x00),     // CRTI
	ILI9806E_COMMAND_INSTR(0x62, 0x08),     // EQTI
	ILI9806E_COMMAND_INSTR(0x63, 0x00),     // PCTI
	ILI9806E_SWITCH_PAGE_INSTR(0x01),     // Change to Page 1
	ILI9806E_COMMAND_INSTR(0xA0, 0x00), // Gamma 0     255
	ILI9806E_COMMAND_INSTR(0xA1, 0x1B), // Gamma 4     251
	ILI9806E_COMMAND_INSTR(0xA2, 0x24), // Gamma 8     247
	ILI9806E_COMMAND_INSTR(0xA3, 0x11), // Gamma 16    239
	ILI9806E_COMMAND_INSTR(0xA4, 0x07), // Gamma 24    231
	ILI9806E_COMMAND_INSTR(0xA5, 0x0C), // Gamma 52    203
	ILI9806E_COMMAND_INSTR(0xA6, 0x08), // Gamma 80    175
	ILI9806E_COMMAND_INSTR(0xA7, 0x05), // Gamma 108   147
	ILI9806E_COMMAND_INSTR(0xA8, 0x06), // Gamma 147   108
	ILI9806E_COMMAND_INSTR(0xA9, 0x0B), // Gamma 175   80
	ILI9806E_COMMAND_INSTR(0xAA, 0x0E), // Gamma 203   52
	ILI9806E_COMMAND_INSTR(0xAB, 0x07), // Gamma 231   24
	ILI9806E_COMMAND_INSTR(0xAC, 0x0E), // Gamma 239   16
	ILI9806E_COMMAND_INSTR(0xAD, 0x12), // Gamma 247   8
	ILI9806E_COMMAND_INSTR(0xAE, 0x0C), // Gamma 251   4
	ILI9806E_COMMAND_INSTR(0xAF, 0x00), // Gamma 255   0
	ILI9806E_COMMAND_INSTR(0xC0, 0x00), // Gamma 0     255
	ILI9806E_COMMAND_INSTR(0xC1, 0x1C), // Gamma 4     251
	ILI9806E_COMMAND_INSTR(0xC2, 0x24), // Gamma 8     247
	ILI9806E_COMMAND_INSTR(0xC3, 0x11), // Gamma 16    239
	ILI9806E_COMMAND_INSTR(0xC4, 0x07), // Gamma 24    231
	ILI9806E_COMMAND_INSTR(0xC5, 0x0C), // Gamma 52    203
	ILI9806E_COMMAND_INSTR(0xC6, 0x08), // Gamma 80    175
	ILI9806E_COMMAND_INSTR(0xC7, 0x06), // Gamma 108   147
	ILI9806E_COMMAND_INSTR(0xC8, 0x07), // Gamma 147    108
	ILI9806E_COMMAND_INSTR(0xC9, 0x0A), // Gamma 175    80
	ILI9806E_COMMAND_INSTR(0xCA, 0x0E), // Gamma 203    52
	ILI9806E_COMMAND_INSTR(0xCB, 0x07), // Gamma 231    24
	ILI9806E_COMMAND_INSTR(0xCC, 0x0D), // Gamma 239    16
	ILI9806E_COMMAND_INSTR(0xCD, 0x11), // Gamma 247     8
	ILI9806E_COMMAND_INSTR(0xCE, 0x0C), // Gamma 251     4
	ILI9806E_COMMAND_INSTR(0xCF, 0x00), // Gamma 255     0
	ILI9806E_SWITCH_PAGE_INSTR(0x06),     // Change to Page 6
	ILI9806E_COMMAND_INSTR(0x00, 0x20),
	ILI9806E_COMMAND_INSTR(0x01, 0x04),
	ILI9806E_COMMAND_INSTR(0x02, 0x00),    
	ILI9806E_COMMAND_INSTR(0x03, 0x00),
	ILI9806E_COMMAND_INSTR(0x04, 0x16),
	ILI9806E_COMMAND_INSTR(0x05, 0x16),
	ILI9806E_COMMAND_INSTR(0x06, 0x88),    
	ILI9806E_COMMAND_INSTR(0x07, 0x02),
	ILI9806E_COMMAND_INSTR(0x08, 0x01),
	ILI9806E_COMMAND_INSTR(0x09, 0x00),    
	ILI9806E_COMMAND_INSTR(0x0A, 0x00),    
	ILI9806E_COMMAND_INSTR(0x0B, 0x00),  
	ILI9806E_COMMAND_INSTR(0x0C, 0x16),
	ILI9806E_COMMAND_INSTR(0x0D, 0x16),
	ILI9806E_COMMAND_INSTR(0x0E, 0x00),
	ILI9806E_COMMAND_INSTR(0x0F, 0x00),
	ILI9806E_COMMAND_INSTR(0x10, 0x50),
	ILI9806E_COMMAND_INSTR(0x11, 0x52),
	ILI9806E_COMMAND_INSTR(0x12, 0x00),
	ILI9806E_COMMAND_INSTR(0x13, 0x00),
	ILI9806E_COMMAND_INSTR(0x14, 0x00),
	ILI9806E_COMMAND_INSTR(0x15, 0x43),
	ILI9806E_COMMAND_INSTR(0x16, 0x0B),
	ILI9806E_COMMAND_INSTR(0x17, 0x00),
	ILI9806E_COMMAND_INSTR(0x18, 0x00),
	ILI9806E_COMMAND_INSTR(0x19, 0x00),
	ILI9806E_COMMAND_INSTR(0x1A, 0x00),
	ILI9806E_COMMAND_INSTR(0x1B, 0x00),
	ILI9806E_COMMAND_INSTR(0x1C, 0x00),
	ILI9806E_COMMAND_INSTR(0x1D, 0x00),
	ILI9806E_COMMAND_INSTR(0x20, 0x01),
	ILI9806E_COMMAND_INSTR(0x21, 0x23),
	ILI9806E_COMMAND_INSTR(0x22, 0x45),
	ILI9806E_COMMAND_INSTR(0x23, 0x67),
	ILI9806E_COMMAND_INSTR(0x24, 0x01),
	ILI9806E_COMMAND_INSTR(0x25, 0x23),
	ILI9806E_COMMAND_INSTR(0x26, 0x45),
	ILI9806E_COMMAND_INSTR(0x27, 0x67),
	ILI9806E_COMMAND_INSTR(0x30, 0x13),
	ILI9806E_COMMAND_INSTR(0x31, 0x11),
	ILI9806E_COMMAND_INSTR(0x32, 0x00),
	ILI9806E_COMMAND_INSTR(0x33, 0x22),
	ILI9806E_COMMAND_INSTR(0x34, 0x22),
//	ILI9806E_COMMAND_INSTR(0x35, 0x22), 
	ILI9806E_COMMAND_INSTR(0x36, 0x22),
	ILI9806E_COMMAND_INSTR(0x37, 0xAA),
	ILI9806E_COMMAND_INSTR(0x38, 0xBB),
	ILI9806E_COMMAND_INSTR(0x39, 0x66),
	ILI9806E_COMMAND_INSTR(0x3A, 0x22),
	ILI9806E_COMMAND_INSTR(0x3B, 0x22),
	ILI9806E_COMMAND_INSTR(0x3C, 0x22),
	ILI9806E_COMMAND_INSTR(0x3D, 0x22),
	ILI9806E_COMMAND_INSTR(0x3E, 0x22),
	ILI9806E_COMMAND_INSTR(0x3F, 0x22),
	ILI9806E_COMMAND_INSTR(0x40, 0x22),
	ILI9806E_SWITCH_PAGE_INSTR(0x07),     // Change to Page 7
	ILI9806E_COMMAND_INSTR(0x17, 0x22),     // Sleep-Out
	ILI9806E_COMMAND_INSTR(0x02, 0x77),     // Display On
	ILI9806E_SWITCH_PAGE_INSTR(0x00),     // Change to Page 0
	ILI9806E_COMMAND_INSTR(0x11, 0x00),     // Sleep-Out
	ILI9806E_COMMAND_INSTR(0x29, 0x00),     // Display On
};

static inline struct ili9806e *panel_to_ili9806e(struct drm_panel *panel)
{
	return container_of(panel, struct ili9806e, panel);
}

/*
 * The panel seems to accept some private DCS commands that map
 * directly to registers.
 *
 * It is organised by page, with each page having its own set of
 * registers, and the first page looks like it's holding the standard
 * DCS commands.
 *
 * So before any attempt at sending a command or data, we have to be
 * sure if we're in the right page or not.
 */
static int ili9806e_switch_page(struct ili9806e *ctx, u8 page)
{
	u8 buf[6] = { 0xff, 0xff, 0x98, 0x06, 0x04, page };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int ili9806e_software_reset(struct ili9806e *ctx)
{
	u8 buf[6] = { 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int ili9806e_send_cmd_data(struct ili9806e *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int ili9806e_init_sequence(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	unsigned int i, j;
	int ret;
	u8 id1_val;
	u8 id1 = 0xda;
	u16 brigthness;

	printk("%s: Enter \n", __func__);

	for (i = 0; i < ARRAY_SIZE(ili9806e_init); i++) {
		const struct ili9806e_instr *instr = &ili9806e_init[i];

		if (instr->op == ILI9806E_SWITCH_PAGE){
			ret = ili9806e_switch_page(ctx, instr->arg.page);
		}
		else if (instr->op == ILI9806E_COMMAND){
			ret = ili9806e_send_cmd_data(ctx, instr->arg.cmd.cmd,
						      instr->arg.cmd.data);
		}
		if (ret){
			printk("%s: seq[%d] cmd=0x%x data=0x%x, ret=%d\n", __func__, i, instr->arg.cmd.cmd,
					instr->arg.cmd.data, ret);
			return ret;
		}
	}

	ret = ili9806e_send_cmd_data(ctx, MIPI_DCS_SET_TEAR_ON, 0x22);
	if (ret){
		printk("%s: failed to panel tear on effect, err=%d\n", __func__, ret);
		return ret;
	}
#if 0
	//Set Maximum Return Packet Size
	ret = ili9806e_switch_page(ctx, 0);
	if (ret)
		return ret;

	ret =  mipi_dsi_set_maximum_return_packet_size(ctx->dsi, 1);
	if (ret)
		return ret;

	printk("%s: Read brigthness", __func__);
	ret = mipi_dsi_dcs_get_display_brightness(ctx->dsi, brigthness);
	if (ret)
		return ret;

	printk("%s: brigthness = %d", __func__, brigthness);

	msleep(20);

	ret = mipi_dsi_dcs_read(ctx->dsi, id1, &id1_val, sizeof(u8));
	if (ret)
		return ret;

	printk("%s: Read ID1 = %02x, err=%d\n", __func__, id1_val, ret);
#endif
	printk("%s: Exit \n", __func__);
	return 0;
}

static int ili9806e_prepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	int ret;
	printk("%s: Enter \n", __func__);

	ret = gpio_request(DSI_EN, "DE");	
	if (ret){
		printk("Failed to request reset gpio, err=%d\n", ret);
	}

	gpio_direction_output(DSI_EN, 1);

	gpiod_set_value(ctx->reset, 0);

	ret = regulator_enable(ctx->power);
	if (ret)
		return ret;
	msleep(20);

	gpiod_set_value(ctx->reset, 1);

	msleep(120); // tRT max
	gpio_free(DSI_EN);

	printk("%s: Exit \n", __func__);

	return 0;
}

static int ili9806e_enable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	unsigned int i;
	int ret;

	printk("%s: Enter\n", __func__);

	ret = ili9806e_init_sequence(panel);
	if (ret < 0) {
		printk("%s: failed with error =%d\n", __func__, ret);
		return ret;
	}

	ret = backlight_enable(ctx->backlight);
	if (ret){
		printk("%s: failed to enable backlight, err=%d\n", __func__, ret);
		return ret;
	}

	printk("%s: Exit\n", __func__);
	return 0;
}

static int ili9806e_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct device *dev = &dsi->dev;
	u16 brightness;
	int ret;

	printk("Enter func %s...\n",__func__);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	bl->props.brightness = brightness;

	printk("Exit %s normally.\n",__func__);	//new add

	return brightness & 0xff;
}

static int ili9806e_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct device *dev = &dsi->dev;
	int ret = 0;

	printk("%s: Enter\n", __func__);
	printk("New brightness: %d\n", bl->props.brightness);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);
	if (ret < 0)
		return ret;

	printk("%s: Exit\n", __func__);
	return 0;
}

static const struct backlight_ops ili9806e_bl_ops = {
	.update_status = ili9806e_bl_update_status,
	.get_brightness = ili9806e_bl_get_brightness,
};

static int ili9806e_disable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	backlight_disable(ctx->backlight);
	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int ili9806e_unprepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);

	regulator_disable(ctx->power);

	//gpiod_set_value(ctx->reset, 1);

	return 0;
}

static const struct display_timing ili9806e_default_timing = {
	.pixelclock = { 29534400, 29534400, 29534400 },//Hz : htotal*vtotal*60 = 29 534 400 Hz
	.hactive = { 480, 480, 480 },
	.hfront_porch = { 50, 50, 50 },
	.hsync_len = { 10, 10, 10 },
	.hback_porch = { 46, 46, 46 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 15, 15, 15 },
	.vsync_len = { 10, 10, 10 },
	.vback_porch = { 15, 15, 15 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW |
		 DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH |
		 DISPLAY_FLAGS_PIXDATA_POSEDGE, //see p.184
};

static int ili9806e_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct drm_display_mode *mode;
	u32 *bus_flags = &panel->connector->display_info.bus_flags;
	int ret;

	printk("%s: Enter\n", __func__);

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		printk("Failed to create display mode!\n");
		return 0;
	}

	drm_display_mode_from_videomode(&ctx->vm, mode);

	mode->width_mm = 52;
	mode->height_mm = 86;
	connector->display_info.bpc = 8;
	connector->display_info.width_mm = 52;
	connector->display_info.height_mm = 86;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	*bus_flags |= DRM_BUS_FLAG_DE_LOW | DRM_BUS_FLAG_PIXDATA_NEGEDGE;

	ret = drm_display_info_set_bus_formats(&(connector->display_info),
			ilitek_bus_formats, ARRAY_SIZE(ilitek_bus_formats));
	if(ret)
		return ret;

	drm_mode_probed_add(connector, mode);
	printk("%s: Exit \n", __func__);
	return 0;
}

static const struct drm_panel_funcs ili9806e_funcs = {
	.prepare	= ili9806e_prepare,
	.unprepare	= ili9806e_unprepare,
	.enable		= ili9806e_enable,
	.disable	= ili9806e_disable,
	.get_modes	= ili9806e_get_modes,
};

static int ili9806e_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *np;
	struct ili9806e *ctx;
	struct backlight_properties bl_props;
	int ret;

	printk("%s: Enter\n", __func__);
	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dsi = dsi;

    	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 2;

	ctx->power = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(ctx->power)) {
		dev_err(&dsi->dev, "Couldn't get our power regulator\n");
		return PTR_ERR(ctx->power);
	}
/*
	ctx->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}
*/

	videomode_from_timing(&ili9806e_default_timing, &ctx->vm);
/*
	np = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (np) {
		ctx->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}
*/
	memset(&bl_props, 0, sizeof(bl_props));
	bl_props.type = BACKLIGHT_RAW;
	bl_props.brightness = 255;
	bl_props.max_brightness = 255;

	ctx->backlight = devm_backlight_device_register(
				dev, dev_name(dev),
				dev, dsi,
				&ili9806e_bl_ops, &bl_props);
	if (IS_ERR(ctx->backlight)) {
		dev_err(&dsi->dev, "Failed to register backlight (%d)\n", ret);
		return ret;
	}

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &ili9806e_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	printk("%s: Exit\n", __func__);
	return mipi_dsi_attach(dsi);
}

static int ili9806e_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ili9806e *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id ili9806e_of_match[] = {
	{ .compatible = "ilitek, ili9806e" },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9806e_of_match);

static struct mipi_dsi_driver ili9806e_dsi_driver = {
	.probe		= ili9806e_dsi_probe,
	.remove		= ili9806e_dsi_remove,
	.driver = {
		.name		= "ili9806e-dsi",
		.of_match_table	= ili9806e_of_match,
	},
};
module_mipi_dsi_driver(ili9806e_dsi_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Ilitek ILI9806E Controller Driver");
MODULE_LICENSE("GPL v2");
