// SPDX-License-Identifier: GPL-2.0
//
// Xilinx ASoC SPDIF audio support
//
// Copyright (C) 2018 Xilinx, Inc.
//
// Author: Maruthi Srinivas Bayyavarapu <maruthis@xilinx.com>
//

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define XLNX_SPDIF_RATES \
	(SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
	SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | \
	SNDRV_PCM_RATE_192000)

#define XLNX_SPDIF_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)

#define XSPDIF_IRQ_STS_REG		0x20
#define XSPDIF_IRQ_STS_CH_STS_MASK	BIT(5)
#define XSPDIF_IRQ_ENABLE_REG		0x28
#define XSPDIF_SOFT_RESET_REG		0x40
#define XSPDIF_SOFT_RESET_VAL		0xA
#define XSPDIF_CONTROL_REG		0x44
#define XSPDIF_CONTROL_ENABLE_MASK	BIT(0)
#define XSPDIF_CONTROL_FIFO_FLUSH_MASK	BIT(1)
#define XSPDIF_CONTROL_CLK_CFG_MASK	GENMASK(5, 2)
#define XSPDIF_CONTROL_CLK_CFG_SHIFT	2
#define XSPDIF_CHAN_0_STS_REG		0x4C
#define XSPDIF_GLOBAL_IRQ_REG		0x1C
#define XSPDIF_GLOBAL_IRQ_ENABLE_MASK	BIT(31)
#define XSPDIF_CH_A_USER_DATA_REG_0	0x64

#define XSPDIF_MAX_CHANNELS		2
#define XSPDIF_AES_SAMPLE_WIDTH		32
#define XSPDIF_CH_STS_UPDATE_TIMEOUT	40

enum {
	CLK_DIV_BY_4,
	CLK_DIV_BY_8,
	CLK_DIV_BY_16,
	CLK_DIV_BY_24,
	CLK_DIV_BY_32,
	CLK_DIV_BY_48,
	CLK_DIV_BY_64,
};

struct spdif_dev_data {
	wait_queue_head_t chsts_q;
	void __iomem *base;
	struct clk *axi_clk;
	struct clk *axis_clk;
	struct clk *aud_clk;
	u32 mode;
	unsigned long aclk;
	bool rx_chsts_updated;
};

static irqreturn_t xlnx_spdifrx_irq_handler(int irq, void *arg)
{
	u32 val;
	struct spdif_dev_data *ctx = arg;

	val = readl(ctx->base + XSPDIF_IRQ_STS_REG);
	if (val & XSPDIF_IRQ_STS_CH_STS_MASK) {
		writel(val & XSPDIF_IRQ_STS_CH_STS_MASK,
		       ctx->base + XSPDIF_IRQ_STS_REG);
		val = readl(ctx->base +
			    XSPDIF_IRQ_ENABLE_REG);
		writel(val & ~XSPDIF_IRQ_STS_CH_STS_MASK,
		       ctx->base + XSPDIF_IRQ_ENABLE_REG);

		ctx->rx_chsts_updated = true;
		wake_up_interruptible(&ctx->chsts_q);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int xlnx_spdif_startup(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	u32 val;
	struct spdif_dev_data *ctx = dev_get_drvdata(dai->dev);

	val = readl(ctx->base + XSPDIF_CONTROL_REG);
	val |= XSPDIF_CONTROL_FIFO_FLUSH_MASK;
	writel(val, ctx->base + XSPDIF_CONTROL_REG);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		writel(XSPDIF_IRQ_STS_CH_STS_MASK,
		       ctx->base + XSPDIF_IRQ_ENABLE_REG);
		writel(XSPDIF_GLOBAL_IRQ_ENABLE_MASK,
		       ctx->base + XSPDIF_GLOBAL_IRQ_REG);
	}

	return 0;
}

static void xlnx_spdif_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct spdif_dev_data *ctx = dev_get_drvdata(dai->dev);

	writel(XSPDIF_SOFT_RESET_VAL, ctx->base + XSPDIF_SOFT_RESET_REG);
}

static int xlnx_spdif_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	u32 val, clk_div, clk_cfg;
	struct spdif_dev_data *ctx = dev_get_drvdata(dai->dev);

	ctx->aclk = clk_get_rate(ctx->aud_clk);
	clk_div = DIV_ROUND_CLOSEST(ctx->aclk, XSPDIF_MAX_CHANNELS *
				    XSPDIF_AES_SAMPLE_WIDTH *
				    params_rate(params));

	switch (clk_div) {
	case 4:
		clk_cfg = CLK_DIV_BY_4;
		break;
	case 8:
		clk_cfg = CLK_DIV_BY_8;
		break;
	case 16:
		clk_cfg = CLK_DIV_BY_16;
		break;
	case 24:
		clk_cfg = CLK_DIV_BY_24;
		break;
	case 32:
		clk_cfg = CLK_DIV_BY_32;
		break;
	case 48:
		clk_cfg = CLK_DIV_BY_48;
		break;
	case 64:
		clk_cfg = CLK_DIV_BY_64;
		break;
	default:
		return -EINVAL;
	}

	val = readl(ctx->base + XSPDIF_CONTROL_REG);
	val &= ~XSPDIF_CONTROL_CLK_CFG_MASK;
	val |= clk_cfg << XSPDIF_CONTROL_CLK_CFG_SHIFT;
	writel(val, ctx->base + XSPDIF_CONTROL_REG);

	return 0;
}

static int rx_stream_detect(struct snd_soc_dai *dai)
{
	int err;
	struct spdif_dev_data *ctx = dev_get_drvdata(dai->dev);
	unsigned long jiffies = msecs_to_jiffies(XSPDIF_CH_STS_UPDATE_TIMEOUT);

	/* start capture only if stream is detected within 40ms timeout */
	err = wait_event_interruptible_timeout(ctx->chsts_q,
					       ctx->rx_chsts_updated,
					       jiffies);
	if (!err) {
		dev_err(dai->dev, "No streaming audio detected!\n");
		return -EINVAL;
	}
	ctx->rx_chsts_updated = false;

	return 0;
}

static int xlnx_spdif_trigger(struct snd_pcm_substream *substream, int cmd,
			      struct snd_soc_dai *dai)
{
	u32 val;
	int ret = 0;
	struct spdif_dev_data *ctx = dev_get_drvdata(dai->dev);

	val = readl(ctx->base + XSPDIF_CONTROL_REG);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		val |= XSPDIF_CONTROL_ENABLE_MASK;
		writel(val, ctx->base + XSPDIF_CONTROL_REG);
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			ret = rx_stream_detect(dai);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		val &= ~XSPDIF_CONTROL_ENABLE_MASK;
		writel(val, ctx->base + XSPDIF_CONTROL_REG);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct snd_soc_dai_ops xlnx_spdif_dai_ops = {
	.startup = xlnx_spdif_startup,
	.shutdown = xlnx_spdif_shutdown,
	.trigger = xlnx_spdif_trigger,
	.hw_params = xlnx_spdif_hw_params,
};

static struct snd_soc_dai_driver xlnx_spdif_tx_dai = {
	.name = "xlnx_spdif_tx",
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = XLNX_SPDIF_RATES,
		.formats = XLNX_SPDIF_FORMATS,
	},
	.ops = &xlnx_spdif_dai_ops,
};

static struct snd_soc_dai_driver xlnx_spdif_rx_dai = {
	.name = "xlnx_spdif_rx",
	.capture = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = XLNX_SPDIF_RATES,
		.formats = XLNX_SPDIF_FORMATS,
	},
	.ops = &xlnx_spdif_dai_ops,
};

static const struct snd_soc_component_driver xlnx_spdif_component = {
	.name = "xlnx-spdif",
};

static const struct of_device_id xlnx_spdif_of_match[] = {
	{ .compatible = "xlnx,spdif-2.0", },
	{},
};
MODULE_DEVICE_TABLE(of, xlnx_spdif_of_match);

static int xlnx_spdif_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;
	struct snd_soc_dai_driver *dai_drv;
	struct spdif_dev_data *ctx;

	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->axi_clk = devm_clk_get(dev, "s_axi_aclk");
	if (IS_ERR(ctx->axi_clk)) {
		ret = PTR_ERR(ctx->axi_clk);
		dev_err(dev, "failed to get s_axi_aclk(%d)\n", ret);
		return ret;
	}
	ret = clk_prepare_enable(ctx->axi_clk);
	if (ret) {
		dev_err(dev, "failed to enable s_axi_aclk(%d)\n", ret);
		return ret;
	}

	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base)) {
		ret = PTR_ERR(ctx->base);
		goto axi_clk_err;
	}
	ret = of_property_read_u32(node, "xlnx,spdif-mode", &ctx->mode);
	if (ret < 0) {
		dev_err(dev, "cannot get SPDIF mode\n");
		goto axi_clk_err;
	}
	if (ctx->mode) {
		ctx->axis_clk = devm_clk_get(dev, "s_axis_aclk");
		if (IS_ERR(ctx->axis_clk)) {
			ret = PTR_ERR(ctx->axis_clk);
			dev_err(dev, "failed to get s_axis_aclk(%d)\n", ret);
			goto axi_clk_err;
		}
		dai_drv = &xlnx_spdif_tx_dai;
	} else {
		ctx->axis_clk = devm_clk_get(dev, "m_axis_aclk");
		if (IS_ERR(ctx->axis_clk)) {
			ret = PTR_ERR(ctx->axis_clk);
			dev_err(dev, "failed to get m_axis_aclk(%d)\n", ret);
			goto axi_clk_err;
		}

		res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
		if (!res) {
			dev_err(dev, "No IRQ resource found\n");
			ret = -ENODEV;
			goto axi_clk_err;
		}
		ret = devm_request_irq(dev, res->start,
				       xlnx_spdifrx_irq_handler,
				       0, "XLNX_SPDIF_RX", ctx);
		if (ret) {
			dev_err(dev, "spdif rx irq request failed\n");
			ret = -ENODEV;
			goto axi_clk_err;
		}

		init_waitqueue_head(&ctx->chsts_q);
		dai_drv = &xlnx_spdif_rx_dai;
	}

	ctx->aud_clk = devm_clk_get(dev, "aud_clk_i");
	if (IS_ERR(ctx->aud_clk)) {
		ret = PTR_ERR(ctx->aud_clk);
		dev_err(dev, "failed to get aud_aclk(%d)\n", ret);
		goto axi_clk_err;
	}

	ret = clk_prepare_enable(ctx->axis_clk);
	if (ret) {
		dev_err(dev, "failed to enable axis_aclk(%d)\n", ret);
		goto axi_clk_err;
	}

	ret = clk_prepare_enable(ctx->aud_clk);
	if (ret) {
		dev_err(dev, "failed to enable aud_aclk(%d)\n", ret);
		goto axis_clk_err;
	}

	dev_set_drvdata(dev, ctx);

	ret = devm_snd_soc_register_component(dev, &xlnx_spdif_component,
					      dai_drv, 1);
	if (ret) {
		dev_err(dev, "SPDIF component registration failed\n");
		goto clk_err;
	}

	writel(XSPDIF_SOFT_RESET_VAL, ctx->base + XSPDIF_SOFT_RESET_REG);
	dev_info(dev, "%s DAI registered\n", dai_drv->name);
	return 0;

clk_err:
	clk_disable_unprepare(ctx->aud_clk);
axis_clk_err:
	clk_disable_unprepare(ctx->axis_clk);
axi_clk_err:
	clk_disable_unprepare(ctx->axi_clk);

	return ret;
}

static int xlnx_spdif_remove(struct platform_device *pdev)
{
	struct spdif_dev_data *ctx = dev_get_drvdata(&pdev->dev);

	clk_disable_unprepare(ctx->aud_clk);
	clk_disable_unprepare(ctx->axis_clk);
	clk_disable_unprepare(ctx->axi_clk);
	return 0;
}

static struct platform_driver xlnx_spdif_driver = {
	.driver = {
		.name = "xlnx-spdif",
		.of_match_table = xlnx_spdif_of_match,
	},
	.probe = xlnx_spdif_probe,
	.remove = xlnx_spdif_remove,
};
module_platform_driver(xlnx_spdif_driver);

MODULE_AUTHOR("Maruthi Srinivas Bayyavarapu <maruthis@xilinx.com>");
MODULE_DESCRIPTION("XILINX SPDIF driver");
MODULE_LICENSE("GPL v2");
