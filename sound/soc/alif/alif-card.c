// SPDX-License-Identifier: GPL-2.0
/*
 * PDM sound card driver for Alif PDM module
 * Copyright (C) 2021-2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>

SND_SOC_DAILINK_DEFS(capture, DAILINK_COMP_ARRAY(COMP_CPU("alifpcm")),
		     DAILINK_COMP_ARRAY(COMP_CODEC("alifpcm", "alifpcm")),
		     DAILINK_COMP_ARRAY(COMP_PLATFORM("alifpcm")));

static struct snd_soc_dai_link alif_dai_link = {
	.name = "alifpcm",
	.stream_name = "alifpcm",
	.capture_only = 1,
	SND_SOC_DAILINK_REG(capture),
};

static struct snd_soc_card sndcard = {
	.name = "alifpcm",
	.owner = THIS_MODULE,
	.dai_link = &alif_dai_link,
	.num_links = 1,
};

#ifdef CONFIG_OF
static const struct of_device_id alif_pcm_dt_ids[] = {
	{ .compatible = "alif,alif-pcm-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, alif_pcm_dt_ids);
#endif

static int alif_pcm_card_probe(struct platform_device *pdev)
{
	struct device *dev;
	int err;

	dev = &pdev->dev;

	sndcard.dev = dev;

	err = devm_snd_soc_register_card(dev, &sndcard);

	if (err)
		dev_err(dev, "failed to register card: %d\n", err);

	return err;
}

static struct platform_driver alif_pcm_card_driver = {
	.driver		= {
		.name	= "alif_pcm_card",
		.of_match_table = alif_pcm_dt_ids,
	},
	.probe		= alif_pcm_card_probe,
};
module_platform_driver(alif_pcm_card_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Alif PDM sound card driver");
MODULE_AUTHOR("Aravind Krishnan <aravind.krishnan@alifsemi.com>");
