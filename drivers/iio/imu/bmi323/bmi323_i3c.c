// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Alif Semiconductor
 *
 * Author: Aravind Krishnan M <aravind.krishnan@alifsemi.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include "bmi323.h"

enum hwid {
	BMI323_ID = 0x0000,
};

/*
 * From BMI323 datasheet section 4: Notes on the Serial Interface Support.
 * Each I3C register read operation requires to read two dummy bytes before
 * the actual payload.
 */
static int bmi323_regmap_i3c_read(void *context, const void *reg_buf, size_t reg_size,
				  void *val_buf, size_t val_size)
{
	struct device *dev = context;
	struct i3c_device *i3c = dev_to_i3cdev(dev);
	struct i3c_priv_xfer xfers[2];
	u8 buff[BMI323_MAX_READ_SIZE + BMI323_I3C_DUMMY];
	int ret;

	if (val_size > BMI323_MAX_READ_SIZE)
		return -EINVAL;

	xfers[0] = (struct i3c_priv_xfer) {
		.rnw = false,
		.len = reg_size,
		.data.out = reg_buf,
	};

	xfers[1] = (struct i3c_priv_xfer) {
		.rnw = true,
		.len = val_size + BMI323_I3C_DUMMY,
		.data.in = buff,
	};

	ret = i3c_device_do_priv_xfers(i3c, xfers, ARRAY_SIZE(xfers));
	if (ret) {
		dev_err(dev, "I3C read failed: %d\n", ret);
		return ret;
	}

	memcpy(val_buf, buff + BMI323_I3C_DUMMY, val_size);

	return 0;
}

static int bmi323_regmap_i3c_write(void *context, const void *data,
				   size_t count)
{
	struct device *dev = context;
	struct i3c_device *i3c = dev_to_i3cdev(dev);
	struct i3c_priv_xfer xfers[] = {
		{
			.rnw = false,
			.len = count,
			.data.out = data,
		},
	};

	return i3c_device_do_priv_xfers(i3c, xfers, 1);
}

static const struct regmap_bus bmi323_regmap_bus = {
	.read = bmi323_regmap_i3c_read,
	.write = bmi323_regmap_i3c_write,
};

static const struct regmap_config bmi323_i3c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = BMI323_CFG_RES_REG,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int bmi323_i3c_probe(struct i3c_device *i3cdev)
{
	struct device *dev = &i3cdev->dev;
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init(dev, &bmi323_regmap_bus, dev, &bmi323_i3c_regmap_config);

	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err_probe(dev, ret, "Failed to initialize I3C regmap\n");
		return ret;
	}

	ret = bmi323_core_probe(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to probe BMI323 core\n");

	return 0;
}

static const struct i3c_device_id bmi323_i3c_ids[] = {
	I3C_DEVICE(0x03B8, 0x1043, (void *)BMI323_ID),
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i3c, bmi323_i3c_ids);

static const struct of_device_id bmi323_of_match[] = {
	{ .compatible = "bosch,bmi323" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, bmi323_of_match);

static struct i3c_driver bmi323_driver = {
	.driver = {
		.name = "bmi323_i3c",
		.of_match_table = bmi323_of_match,
	},
	.probe = bmi323_i3c_probe,
	.id_table = bmi323_i3c_ids,
};
module_i3c_driver(bmi323_driver);

MODULE_DESCRIPTION("Bosch BMI323 IMU I3C driver");
MODULE_AUTHOR("Aravind Krishnan <aravind.krishnan@alifsemi.com>");
MODULE_LICENSE("GPL");
