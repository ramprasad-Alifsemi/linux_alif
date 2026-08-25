// SPDX-License-Identifier: GPL-2.0
/**
 * dwc3-alif.c - ALIF ENSEMBLE DWC3 Specific Glue layer
 *
 * Copyright (c) 2021-2025 Alif Semiconductor
 *
 * Author: Nishit Sharma <nishit.sharma@alifsemi.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/bits.h>

#define ENSEMBLE_MAX_CLOCKS		3

#define USB_CTRL2_PHY_POR_REG		0x4903F0AC
#define PWR_CTRL_REG			0x1A609008

#define PWR_CTRL_UPHY_PWR_MASK		BIT(16)
#define PWR_CTRL_UPHY_ISO		BIT(17)
#define USB_CTRL2_PHY_POR		BIT(8)

struct dwc3_ensemble_driverdata {
	const char		*clk_names[ENSEMBLE_MAX_CLOCKS];
	int			num_clks;
	int			suspend_clk_idx;
};

struct dwc3_ensemble {
	struct device		*dev;
	const char		**clk_names;
	struct clk		*clks[ENSEMBLE_MAX_CLOCKS];
	int			num_clks;
	int			suspend_clk_idx;
	struct regulator	*vdd33;
	struct regulator	*vdd18;
};

static int dwc3_ensemble_remove_child(struct device *dev, void *unused)
{
	struct platform_device *pdev = to_platform_device(dev);

	platform_device_unregister(pdev);

	return 0;
}

static int power_control(void)
{
	void __iomem *pwr_ctrl_base, *usb_ctrl_base;
	u32 data;

	pwr_ctrl_base = ioremap(PWR_CTRL_REG, SZ_4);
	usb_ctrl_base = ioremap(USB_CTRL2_PHY_POR_REG, SZ_4);

	data = readl(pwr_ctrl_base);
	data &= ~PWR_CTRL_UPHY_PWR_MASK;
	data &= ~PWR_CTRL_UPHY_ISO;
	writel(data, pwr_ctrl_base);

	data = readl(usb_ctrl_base);
	data &= ~USB_CTRL2_PHY_POR;
	writel(data, usb_ctrl_base);

	iounmap(pwr_ctrl_base);
	iounmap(usb_ctrl_base);
	return 0;
}

static int dwc3_ensemble_probe(struct platform_device *pdev)
{
	struct dwc3_ensemble	*ensemble;
	struct device		*dev = &pdev->dev;
	struct device_node	*node = dev->of_node;
	const struct dwc3_ensemble_driverdata *driver_data;
	int	i, ret;

	ensemble = devm_kzalloc(dev, sizeof(*ensemble), GFP_KERNEL);
	if (!ensemble)
		return -ENOMEM;

	driver_data = of_device_get_match_data(dev);
	ensemble->dev = dev;
	ensemble->num_clks = driver_data->num_clks;
	ensemble->clk_names = (const char **)driver_data->clk_names;
	ensemble->suspend_clk_idx = driver_data->suspend_clk_idx;

	platform_set_drvdata(pdev, ensemble);

	for (i = 0; i < ensemble->num_clks; i++) {
		ensemble->clks[i] = devm_clk_get(dev, ensemble->clk_names[i]);
		if (IS_ERR(ensemble->clks[i])) {
			dev_err(dev, "failed to get clock: %s\n",
				ensemble->clk_names[i]);
			return PTR_ERR(ensemble->clks[i]);
		}
	}

	for (i = 0; i < ensemble->num_clks; i++) {
		ret = clk_prepare_enable(ensemble->clks[i]);
		if (ret) {
			while (i-- > 0)
				clk_disable_unprepare(ensemble->clks[i]);
			return ret;
		}
	}
	ret = power_control();
	if (ret) {
		dev_err(dev, "Failed to enable USB peripherals\n");
		return ret;
	}

	if (ensemble->suspend_clk_idx >= 0)
		clk_prepare_enable(ensemble->clks[ensemble->suspend_clk_idx]);

	ensemble->vdd33 = devm_regulator_get(dev, "vdd33");
	if (IS_ERR(ensemble->vdd33)) {
		ret = PTR_ERR(ensemble->vdd33);
		goto vdd33_err;
	}
	ret = regulator_enable(ensemble->vdd33);
	if (ret) {
		dev_err(dev, "Failed to enable VDD33 supply\n");
		goto vdd33_err;
	}

	ensemble->vdd18 = devm_regulator_get(dev, "vdd18");
	if (IS_ERR(ensemble->vdd18)) {
		ret = PTR_ERR(ensemble->vdd18);
		goto vdd18_err;
	}
	ret = regulator_enable(ensemble->vdd18);
	if (ret) {
		dev_err(dev, "Failed to enable VDD18 supply\n");
		goto vdd18_err;
	}

	if (node) {
		ret = of_platform_populate(node, NULL, NULL, dev);
		if (ret) {
			dev_err(dev, "failed to add dwc3 core\n");
			goto populate_err;
		}
	} else {
		dev_err(dev, "no device node, failed to add dwc3 core\n");
		ret = -ENODEV;
		goto populate_err;
	}
	return 0;

populate_err:
	regulator_disable(ensemble->vdd18);
vdd18_err:
	regulator_disable(ensemble->vdd33);
vdd33_err:
	for (i = ensemble->num_clks - 1; i >= 0; i--)
		clk_disable_unprepare(ensemble->clks[i]);

	if (ensemble->suspend_clk_idx >= 0)
		clk_disable_unprepare(ensemble->clks[ensemble->suspend_clk_idx]);

	return ret;
}

static void dwc3_ensemble_remove(struct platform_device *pdev)
{
	struct dwc3_ensemble	*ensemble = platform_get_drvdata(pdev);
	int i;

	device_for_each_child(&pdev->dev, NULL, dwc3_ensemble_remove_child);

	for (i = ensemble->num_clks - 1; i >= 0; i--)
		clk_disable_unprepare(ensemble->clks[i]);

	if (ensemble->suspend_clk_idx >= 0)
		clk_disable_unprepare(ensemble->clks[ensemble->suspend_clk_idx]);

	regulator_disable(ensemble->vdd33);
	regulator_disable(ensemble->vdd18);
}

static const struct dwc3_ensemble_driverdata dwc3_ensemble_drvdata = {
	.clk_names = { "usb_clk", "dwc_clk" },
	.num_clks = 2,
	.suspend_clk_idx = -1,
};

static const struct of_device_id dwc3_ensemble_match[] = {
	{
		.compatible = "alif,ensemble-usb3",
		.data = &dwc3_ensemble_drvdata,
	},
};
MODULE_DEVICE_TABLE(of, dwc3_ensemble_match);

#ifdef CONFIG_PM_SLEEP
static int dwc3_ensemble_suspend(struct device *dev)
{
	struct dwc3_ensemble *ensemble = dev_get_drvdata(dev);
	int i;

	for (i = ensemble->num_clks - 1; i >= 0; i--)
		clk_disable_unprepare(ensemble->clks[i]);

	regulator_disable(ensemble->vdd33);
	regulator_disable(ensemble->vdd18);

	return 0;
}

static int dwc3_ensemble_resume(struct device *dev)
{
	struct dwc3_ensemble *ensemble = dev_get_drvdata(dev);
	int i, ret;

	ret = regulator_enable(ensemble->vdd33);
	if (ret) {
		dev_err(dev, "Failed to enable VDD33 supply\n");
		return ret;
	}
	ret = regulator_enable(ensemble->vdd18);
	if (ret) {
		dev_err(dev, "Failed to enable VDD18 supply\n");
		return ret;
	}

	for (i = 0; i < ensemble->num_clks; i++) {
		ret = clk_prepare_enable(ensemble->clks[i]);
		if (ret) {
			while (i-- > 0)
				clk_disable_unprepare(ensemble->clks[i]);
			return ret;
		}
	}

	return 0;
}

static const struct dev_pm_ops ensemble_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_ensemble_suspend, dwc3_ensemble_resume)
};

#define DEV_PM_OPS	(&ensemble_dev_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif /* CONFIG_PM_SLEEP */

static struct platform_driver ensemble_driver = {
	.probe		= dwc3_ensemble_probe,
	.remove_new	= dwc3_ensemble_remove,
	.driver		= {
		.name	= "alif-ensemble",
		.of_match_table = dwc3_ensemble_match,
		.pm	= DEV_PM_OPS,
	},
};

module_platform_driver(ensemble_driver);

MODULE_AUTHOR("Nishit Sharma <nishit.sharma@alifsemi.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 ENSEMBLE Glue Layer");
