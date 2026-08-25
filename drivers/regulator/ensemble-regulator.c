// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Alif Semiconductor.
 * Author: Aravind Krishnan <aravind.krishnan@alifsemi.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define FLEXIO_VSEL_REG		0x0
#define FLEXIO_VSEL_MASK	BIT(0)
#define FLEXIO_N_VOLTAGES	2
#define ENSEMBLE_REG_BITS	32
#define ENSEMBLE_VAL_BITS	32
#define ENSEMBLE_REG_BASE	0

enum ensemble_flexio_reg_mode {
	FLEXIO_3_3V,
	FLEXIO_1_8V
};

static const unsigned int flexio_voltage_table[] = {
	3300000U,	/* 3.3V */
	1800000U	/* 1.8V */
};

/*
 * Ensemble regulator's information
 *
 * @desc: standard fields of regulator description.
 */
struct ensemble_reg_info {
	struct regulator_desc desc;
};

static unsigned int ensemble_flexio_map_mode(unsigned int mode)
{
	switch (mode) {
	case REGULATOR_MODE_NORMAL:
		return FLEXIO_3_3V;
	case REGULATOR_MODE_IDLE:
		return FLEXIO_1_8V;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static int ensemble_flexio_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	int val;
	int ret;

	if (mode != FLEXIO_3_3V && mode != FLEXIO_1_8V) {
		ret = -EINVAL;
		goto err_mode;
	}

	val = mode;
	ret = regmap_update_bits(rdev->regmap, rdev->desc->vsel_reg,
				 rdev->desc->vsel_mask, val);
err_mode:
	if (ret) {
		dev_err(&rdev->dev, "Failed to set Ensemble flexio voltage mode (%s) : %d\n",
			rdev->desc->name, ret);
		return ret;
	}

	return 0;
}

static unsigned int ensemble_flexio_get_mode(struct regulator_dev *rdev)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(rdev->regmap, rdev->desc->vsel_reg, &regval);
	if (ret) {
		dev_err(&rdev->dev, "Failed to get %s mode : %d\n",
			rdev->desc->name, ret);
		return REGULATOR_MODE_INVALID;
	}
	regval &= rdev->desc->vsel_mask;

	switch (regval) {
	case FLEXIO_3_3V:
		return REGULATOR_MODE_NORMAL;
	case FLEXIO_1_8V:
		return REGULATOR_MODE_IDLE;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static const struct regulator_ops ensemble_regulator_voltage_ops = {
	.list_voltage = regulator_list_voltage_table,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_mode = ensemble_flexio_set_mode,
	.get_mode = ensemble_flexio_get_mode,
};

static const struct ensemble_reg_info ensemble_flexio = {
	.desc = {
		.name = "ensemble_flexio",
		.id = 0,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = FLEXIO_N_VOLTAGES,
		.volt_table = flexio_voltage_table,
		.vsel_reg = FLEXIO_VSEL_REG,
		.vsel_mask = FLEXIO_VSEL_MASK,
		.of_map_mode = ensemble_flexio_map_mode,
		.ops = &ensemble_regulator_voltage_ops,
	}
};

static struct of_regulator_match ensemble_matches[] = {
	{ .name = "ensemble_flexio", .desc = &ensemble_flexio.desc },
};

#define ENSEMBLE_NUM_REGS	ARRAY_SIZE(ensemble_matches)

static const struct of_device_id ensemble_of_match[] = {
	{ .compatible = "alif,ensemble-regulator", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ensemble_of_match);

static const struct regmap_config ensemble_regmap_config = {
	.reg_bits = ENSEMBLE_REG_BITS,
	.val_bits = ENSEMBLE_VAL_BITS,
	.reg_base = ENSEMBLE_REG_BASE,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int ensemble_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regulator_config cfg = { };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	void __iomem *mmio_base;
	int ret, count, idx;

	count = of_regulator_match(dev, np, ensemble_matches, ENSEMBLE_NUM_REGS);
	if (count < 0) {
		dev_err(dev, "Failed to match regulators: %d\n", count);
		return count;
	}

	mmio_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmio_base)) {
		dev_err(dev, "Failed to map IO resource\n");
		return PTR_ERR(mmio_base);
	}

	regmap = devm_regmap_init_mmio(dev, mmio_base, &ensemble_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to initialize regmap\n");
		return PTR_ERR(regmap);
	}

	cfg.dev = dev;
	cfg.regmap = regmap;

	for (idx = 0; idx < ENSEMBLE_NUM_REGS; idx++) {
		if (!ensemble_matches[idx].init_data || !ensemble_matches[idx].of_node)
			continue;

		if (!(ensemble_matches[idx].desc)) {
			dev_err(dev, "Missing regulator description\n");
			return -ENODEV;
		}

		cfg.init_data = ensemble_matches[idx].init_data;
		cfg.of_node = ensemble_matches[idx].of_node;

		rdev = devm_regulator_register(dev, ensemble_matches[idx].desc, &cfg);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			dev_err(dev, "Failed to register regulator: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static struct platform_driver ensemble_regulator_driver = {
	.probe		= ensemble_regulator_probe,
	.driver		= {
		.name		= "ensemble-regulator",
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = ensemble_of_match,
	},
};

module_platform_driver(ensemble_regulator_driver);

MODULE_DESCRIPTION("Ensemble voltage regulator driver");
MODULE_AUTHOR("Aravind Krishnan <aravind.krishnan@alifsemi.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ensemble-regulator");
