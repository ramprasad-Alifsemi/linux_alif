// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pinctrl driver for Alif Ensemble SoCs
 *
 * Copyright (C) 2024 Alif Semiconductor
 * Author: Harith George <harith.g@alifsemi.com>
 */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include "core.h"
#include "pinconf.h"
#include "pinctrl-utils.h"
#include "pinmux.h"

/* Number of io pads that can be configured */
#define ENSEMBLE_NUM_IOS		120

#define PIN_SIZE			8

/* Read Enable */
#define ENSEMBLE_PINCONF_REN		BIT(0)
/* Schmitt Trigger */
#define ENSEMBLE_PINCONF_SCHMITT	BIT(1)
/* Slew Rate [1 = Fast, 0 = Slow] */
#define ENSEMBLE_PINCONF_SR		BIT(2)
/* Driver Disabled State Control
 * P2 P1
 *  0  0	Z (Normal operation)
 *  0  1	Weak 1 (Pull up)
 *  1  0	Weak 0 (Pull down)
 *  1  1	Repeater (Bus keeper)
 */
#define ENSEMBLE_DSC_SHIFT		3
#define ENSEMBLE_DSC_MASK		0x3
#define ENSEMBLE_PINCONF_DSC(x)		(((x) & ENSEMBLE_DSC_MASK) << ENSEMBLE_DSC_SHIFT)
#define ENSEMBLE_PINCONF_DSC_Z		ENSEMBLE_PINCONF_DSC(0)
#define ENSEMBLE_PINCONF_DSC_PU		ENSEMBLE_PINCONF_DSC(1)
#define ENSEMBLE_PINCONF_DSC_PD		ENSEMBLE_PINCONF_DSC(2)
#define ENSEMBLE_PINCONF_DSC_REP	ENSEMBLE_PINCONF_DSC(3)
/* Output Drive Strength
 * E2 E1
 *  0  0	2 mA
 *  0  1	4 mA
 *  1  0	8 mA
 *  1  1	12 mA
 */
#define ENSEMBLE_ODS_SHIFT		5
#define ENSEMBLE_ODS_MASK		0x3
#define ENSEMBLE_PINCONF_DRIVE_STRENGTH(x)	(((x) & ENSEMBLE_ODS_MASK) << ENSEMBLE_ODS_SHIFT)
#define ENSEMBLE_PINCONF_DS_2MA		ENSEMBLE_PINCONF_DRIVE_STRENGTH(0)
#define ENSEMBLE_PINCONF_DS_4MA		ENSEMBLE_PINCONF_DRIVE_STRENGTH(1)
#define ENSEMBLE_PINCONF_DS_8MA		ENSEMBLE_PINCONF_DRIVE_STRENGTH(2)
#define ENSEMBLE_PINCONF_DS_12MA	ENSEMBLE_PINCONF_DRIVE_STRENGTH(3)

/* DRV [1 = Push Pull, 0 = Open Drain] */
#define ENSEMBLE_PINCONF_DRV		BIT(7)
#define ENSEMBLE_NO_PAD_CTL		0x00000000

struct ensemble_pin {
	unsigned long pin_no;
	unsigned long mux;
	unsigned long padctrl;
};

struct ensemble_pinctrl {
	struct device		*dev;
	struct pinctrl_dev	*pctl;
	void __iomem		*pinmux_base;
	unsigned int group_index;
	/* Mutex for protecting pinmux operations */
	struct mutex mutex;
};

static const struct pinctrl_pin_desc ensemble_pins[] = {
	/* Port 0 */
	PINCTRL_PIN(0, "P0_0"),
	PINCTRL_PIN(1, "P0_1"),
	PINCTRL_PIN(2, "P0_2"),
	PINCTRL_PIN(3, "P0_3"),
	PINCTRL_PIN(4, "P0_4"),
	PINCTRL_PIN(5, "P0_5"),
	PINCTRL_PIN(6, "P0_6"),
	PINCTRL_PIN(7, "P0_7"),
	/* Port 1 */
	PINCTRL_PIN(8, "P1_0"),
	PINCTRL_PIN(9, "P1_1"),
	PINCTRL_PIN(10, "P1_2"),
	PINCTRL_PIN(11, "P1_3"),
	PINCTRL_PIN(12, "P1_4"),
	PINCTRL_PIN(13, "P1_5"),
	PINCTRL_PIN(14, "P1_6"),
	PINCTRL_PIN(15, "P1_7"),
	/* Port 2 */
	PINCTRL_PIN(16, "P2_0"),
	PINCTRL_PIN(17, "P2_1"),
	PINCTRL_PIN(18, "P2_2"),
	PINCTRL_PIN(19, "P2_3"),
	PINCTRL_PIN(20, "P2_4"),
	PINCTRL_PIN(21, "P2_5"),
	PINCTRL_PIN(22, "P2_6"),
	PINCTRL_PIN(23, "P2_7"),
	/* Port 3 */
	PINCTRL_PIN(24, "P3_0"),
	PINCTRL_PIN(25, "P3_1"),
	PINCTRL_PIN(26, "P3_2"),
	PINCTRL_PIN(27, "P3_3"),
	PINCTRL_PIN(28, "P3_4"),
	PINCTRL_PIN(29, "P3_5"),
	PINCTRL_PIN(30, "P3_6"),
	PINCTRL_PIN(31, "P3_7"),
	/* Port 4 */
	PINCTRL_PIN(32, "P4_0"),
	PINCTRL_PIN(33, "P4_1"),
	PINCTRL_PIN(34, "P4_2"),
	PINCTRL_PIN(35, "P4_3"),
	PINCTRL_PIN(36, "P4_4"),
	PINCTRL_PIN(37, "P4_5"),
	PINCTRL_PIN(38, "P4_6"),
	PINCTRL_PIN(39, "P4_7"),
	/* Port 5 */
	PINCTRL_PIN(40, "P5_0"),
	PINCTRL_PIN(41, "P5_1"),
	PINCTRL_PIN(42, "P5_2"),
	PINCTRL_PIN(43, "P5_3"),
	PINCTRL_PIN(44, "P5_4"),
	PINCTRL_PIN(45, "P5_5"),
	PINCTRL_PIN(46, "P5_6"),
	PINCTRL_PIN(47, "P5_7"),
	/* Port 6 */
	PINCTRL_PIN(48, "P6_0"),
	PINCTRL_PIN(49, "P6_1"),
	PINCTRL_PIN(50, "P6_2"),
	PINCTRL_PIN(51, "P6_3"),
	PINCTRL_PIN(52, "P6_4"),
	PINCTRL_PIN(53, "P6_5"),
	PINCTRL_PIN(54, "P6_6"),
	PINCTRL_PIN(55, "P6_7"),
	/* Port 7 */
	PINCTRL_PIN(56, "P7_0"),
	PINCTRL_PIN(57, "P7_1"),
	PINCTRL_PIN(58, "P7_2"),
	PINCTRL_PIN(59, "P7_3"),
	PINCTRL_PIN(60, "P7_4"),
	PINCTRL_PIN(61, "P7_5"),
	PINCTRL_PIN(62, "P7_6"),
	PINCTRL_PIN(63, "P7_7"),
	/* Port 8 */
	PINCTRL_PIN(64, "P8_0"),
	PINCTRL_PIN(65, "P8_1"),
	PINCTRL_PIN(66, "P8_2"),
	PINCTRL_PIN(67, "P8_3"),
	PINCTRL_PIN(68, "P8_4"),
	PINCTRL_PIN(69, "P8_5"),
	PINCTRL_PIN(70, "P8_6"),
	PINCTRL_PIN(71, "P8_7"),
	/* Port 9 */
	PINCTRL_PIN(72, "P9_0"),
	PINCTRL_PIN(73, "P9_1"),
	PINCTRL_PIN(74, "P9_2"),
	PINCTRL_PIN(75, "P9_3"),
	PINCTRL_PIN(76, "P9_4"),
	PINCTRL_PIN(77, "P9_5"),
	PINCTRL_PIN(78, "P9_6"),
	PINCTRL_PIN(79, "P9_7"),
	/* Port 10 */
	PINCTRL_PIN(80, "P10_0"),
	PINCTRL_PIN(81, "P10_1"),
	PINCTRL_PIN(82, "P10_2"),
	PINCTRL_PIN(83, "P10_3"),
	PINCTRL_PIN(84, "P10_4"),
	PINCTRL_PIN(85, "P10_5"),
	PINCTRL_PIN(86, "P10_6"),
	PINCTRL_PIN(87, "P10_7"),
	/* Port 11 */
	PINCTRL_PIN(88, "P11_0"),
	PINCTRL_PIN(89, "P11_1"),
	PINCTRL_PIN(90, "P11_2"),
	PINCTRL_PIN(91, "P11_3"),
	PINCTRL_PIN(92, "P11_4"),
	PINCTRL_PIN(93, "P11_5"),
	PINCTRL_PIN(94, "P11_6"),
	PINCTRL_PIN(95, "P11_7"),
	/* Port 12 */
	PINCTRL_PIN(96, "P12_0"),
	PINCTRL_PIN(97, "P12_1"),
	PINCTRL_PIN(98, "P12_2"),
	PINCTRL_PIN(99, "P12_3"),
	PINCTRL_PIN(100, "P12_4"),
	PINCTRL_PIN(101, "P12_5"),
	PINCTRL_PIN(102, "P12_6"),
	PINCTRL_PIN(103, "P12_7"),
	/* Port 13 */
	PINCTRL_PIN(104, "P13_0"),
	PINCTRL_PIN(105, "P13_1"),
	PINCTRL_PIN(106, "P13_2"),
	PINCTRL_PIN(107, "P13_3"),
	PINCTRL_PIN(108, "P13_4"),
	PINCTRL_PIN(109, "P13_5"),
	PINCTRL_PIN(110, "P13_6"),
	PINCTRL_PIN(111, "P13_7"),
	/* Port 14 */
	PINCTRL_PIN(112, "P14_0"),
	PINCTRL_PIN(113, "P14_1"),
	PINCTRL_PIN(114, "P14_2"),
	PINCTRL_PIN(115, "P14_3"),
	PINCTRL_PIN(116, "P14_4"),
	PINCTRL_PIN(117, "P14_5"),
	PINCTRL_PIN(118, "P14_6"),
	PINCTRL_PIN(119, "P14_7"),
};

static inline const struct group_desc *
ensemble_pinctrl_find_group_by_name(struct pinctrl_dev *pctldev, const char *name)
{
	const struct group_desc *grp = NULL;
	int i;

	for (i = 0; i < pctldev->num_groups; i++) {
		grp = pinctrl_generic_get_group(pctldev, i);
		if (grp && !strcmp(grp->grp.name, name))
			break;
	}
	return grp;
}

static void ensemble_pin_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
				  unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static int ensemble_dt_node_to_map(struct pinctrl_dev *pctldev, struct device_node *np,
				   struct pinctrl_map **map, unsigned int *num_maps)
{
	struct ensemble_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	unsigned long *configs;
	unsigned int num_configs;
	const struct group_desc *grp;
	struct pinctrl_map *new_map;
	struct device_node *parent;
	struct ensemble_pin *pin;
	int map_num = 1;
	int i, j, ret;

	/*
	 * first find the group of this node and check if we need create
	 * config maps for pins
	 */
	grp = ensemble_pinctrl_find_group_by_name(pctldev, np->name);
	if (!grp) {
		return dev_err_probe(ipctl->dev, -EINVAL,
			"unable to find group for node %pOFn\n", np);
	}

	ret = pinconf_generic_parse_dt_config(np, pctldev, &configs,
					      &num_configs);
	if (ret) {
		return dev_err_probe(ipctl->dev, -EINVAL,
			"Unable to parse dt config for node %pOFn\n", np);
	}

	if (num_configs)
		map_num += grp->grp.npins;

	new_map = kmalloc_array(map_num, sizeof(struct pinctrl_map),
				GFP_KERNEL);
	if (!new_map) {
		ret = -ENOMEM;
		goto out;
	}
	*map = new_map;
	*num_maps = map_num;

	/* create mux map */
	parent = of_get_parent(np);
	if (!parent) {
		kfree(new_map);
		ret = -EINVAL;
		goto out;
	}

	new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
	new_map[0].data.mux.function = parent->name;
	new_map[0].data.mux.group = np->name;
	of_node_put(parent);

	if (num_configs) {
		/* j index starts with 1 as the first map is already taken */
		/* create config map */
		j = 1;
		for (i = 0; i < grp->grp.npins; i++) {
			pin = &((struct ensemble_pin *)(grp->data))[i];

			/* The counter 'j' is incremented by this function */
			ret = pinctrl_utils_add_map_configs(pctldev, &new_map,
							    num_maps, &j,
							    pin_get_name(pctldev, pin->pin_no),
							    configs, num_configs,
							    PIN_MAP_TYPE_CONFIGS_PIN);
		}
	}

	dev_dbg(pctldev->dev, "maps: function %s group %s num %d\n",
		(*map)->data.mux.function, (*map)->data.mux.group, map_num);
out:
	if (num_configs)
		kfree(configs);
	return ret;
}

static const struct pinctrl_ops ensemble_pctl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.pin_dbg_show = ensemble_pin_dbg_show,
	.dt_node_to_map = ensemble_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

static inline int pinmux_set_one_pin(struct ensemble_pinctrl *pctl, struct ensemble_pin *pin)
{
	unsigned int pin_id = pin->pin_no;
	u32 offset, val;

	if (pin_id >= ENSEMBLE_NUM_IOS) {
		return dev_err_probe(pctl->dev, -EINVAL,
				"Unsupported pin %d\n", pin_id);
	}

	offset = pin_id * 4;
	val = readl(pctl->pinmux_base + offset);
	val = (pin->mux & 0xF) | ((pin->padctrl & 0xFF) << 16);
	writel(val, pctl->pinmux_base + offset);

	dev_dbg(pctl->dev, "write: offset 0x%x val 0x%lx\n",
		offset, pin->mux);

	return 0;
}

static int pinmux_set(struct pinctrl_dev *pctldev, unsigned int selector, unsigned int group)
{
	struct ensemble_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct function_desc *func;
	struct group_desc *grp;
	struct ensemble_pin *pin;
	unsigned int npins;
	int i, err;

	/*
	 * Configure mux mode for each pin in group for a specific
	 * function.
	 */
	grp = pinctrl_generic_get_group(pctldev, group);
	if (!grp)
		return -EINVAL;

	func = pinmux_generic_get_function(pctldev, selector);
	if (!func)
		return -EINVAL;

	npins = grp->grp.npins;

	dev_dbg(pctl->dev, "enable function %s group %s\n",
		func->func.name, grp->grp.name);

	for (i = 0; i < npins; i++) {
		pin = &((struct ensemble_pin *)(grp->data))[i];
		err = pinmux_set_one_pin(pctl, pin);
		if (err)
			return err;
	}
	return 0;
}

const struct pinmux_ops ensemble_pmx_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = pinmux_set,
};

static int ensemble_padconf_get_config(struct ensemble_pinctrl *info, unsigned int pin, u32 *val)
{
	u32 offset = pin * 4;
	u32 tmp;

	/* Bits 23:16 hold the padconf values */
	tmp = readl_relaxed(info->pinmux_base + offset);
	*val = (tmp >> 16) & 0xFF;
	return 0;
}

static void ensemble_padconf_set_config(struct ensemble_pinctrl *info, unsigned int pin, u32 val)
{
	u32 offset = pin * 4;
	u32 tmp;

	/* Retain the mux setting. Bits 3:0 hold the mux
	 * padconf is bits 23:16
	 */
	tmp = readl_relaxed(info->pinmux_base + offset);
	tmp &= 0xF;
	tmp |= (val << 16);
	writel_relaxed(tmp, info->pinmux_base + offset);
}

static int ensemble_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin_id,
				unsigned long *configs, unsigned int num_configs)
{
	int i;
	unsigned long config;
	struct ensemble_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);
	unsigned int arg, param, val;

	if (pin_id >= ENSEMBLE_NUM_IOS)
		return -EOPNOTSUPP;

	/* for each config */
	for (i = 0; i < num_configs; i++) {
		config = configs[i];
		val = 0;
		param = pinconf_to_config_param(config);
		arg = pinconf_to_config_argument(config);
		dev_dbg(info->dev,
			"%s:%d, pin_id=%d, config=0x%lx",
			__func__, __LINE__, pin_id, config);

		switch (param) {
		case PIN_CONFIG_BIAS_BUS_HOLD:
			val |= ENSEMBLE_PINCONF_DSC_REP;
			break;
		case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
			val |= ENSEMBLE_PINCONF_DSC_Z;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			val |= ENSEMBLE_PINCONF_DSC_PU;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			val |= ENSEMBLE_PINCONF_DSC_PD;
			break;
		case PIN_CONFIG_INPUT_SCHMITT:
			val |= ENSEMBLE_PINCONF_SCHMITT;
			break;
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			val &= ~ENSEMBLE_PINCONF_DRV;
			break;
		case PIN_CONFIG_DRIVE_PUSH_PULL:
			val |= ENSEMBLE_PINCONF_DRV;
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			val |= ENSEMBLE_PINCONF_REN;
			break;
		case PIN_CONFIG_SLEW_RATE:
			val |= ENSEMBLE_PINCONF_SR;
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			switch (arg) {
			case 2:   /* 2mA */
				val = 0;
				break;
			case 4:   /* 4mA */
				val = 1;
				break;
			case 8:   /* 8mA */
				val = 2;
				break;
			case 12:  /* 12mA */
				val = 3;
				break;
			}
			val = ENSEMBLE_PINCONF_DRIVE_STRENGTH(val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		ensemble_padconf_set_config(info, pin_id, val);
	}
	return 0;
}

static int ensemble_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin_id,
				unsigned long *config)
{
	struct ensemble_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);
	unsigned int param = pinconf_to_config_param(*config);
	unsigned int arg = 1;
	u32 reg;
	int ret;

	if (pin_id >= ENSEMBLE_NUM_IOS)
		return -EOPNOTSUPP;

	ret =  ensemble_padconf_get_config(info, pin_id, &reg);
	if (ret)
		return -EIO;

	switch (param) {
	case PIN_CONFIG_BIAS_BUS_HOLD:
		if (!(reg & ENSEMBLE_PINCONF_DSC_REP))
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		if (!(reg & ENSEMBLE_PINCONF_DSC_Z))
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (!(reg & ENSEMBLE_PINCONF_DSC_PU))
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (!(reg & ENSEMBLE_PINCONF_DSC_PD))
			return -EINVAL;
		break;
	case PIN_CONFIG_INPUT_SCHMITT:
		if (!(reg & ENSEMBLE_PINCONF_SCHMITT))
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		if (reg & ENSEMBLE_PINCONF_DRV)
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		if (!(reg & ENSEMBLE_PINCONF_DRV))
			return -EINVAL;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		if (!(reg & ENSEMBLE_PINCONF_REN))
			return -EINVAL;
		break;
	case PIN_CONFIG_SLEW_RATE:
		if (!(reg & ENSEMBLE_PINCONF_SR))
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		reg = (reg >> ENSEMBLE_ODS_SHIFT) & ENSEMBLE_ODS_MASK;
		switch (reg) {
		case 0: /* 2mA */
			arg = 2;
			break;
		case 1: /* 4mA */
			arg = 4;
			break;
		case 2: /* 8mA */
			arg = 8;
			break;
		case 3: /* 12mA */
			arg = 12;
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	*config = pinconf_to_config_packed(param, arg);
	return 0;
}

static const struct pinconf_ops ensemble_conf_ops = {
	.pin_config_get		= ensemble_pinconf_get,
	.pin_config_set		= ensemble_pinconf_set,
};

/*
 * Each pin represented in consists of a number of u32 PIN_FUNC_ID
 * and 1 u32 CONFIG, the total size is PIN_FUNC_ID + CONFIG for each pin.
 * For generic_pinconf case, there's no extra u32 CONFIG.
 *
 * PIN_FUNC_ID format:
 * Default:
 *     <mux_reg conf_reg input_reg mux_mode input_val>
 *     <mux_conf_reg input_reg mux_mode input_val>
 *     <pin_id mux_mode>
 */
static void ensemble_pinctrl_parse_pin(struct ensemble_pinctrl *info, unsigned int *pin_id,
				       struct ensemble_pin *pin, const __be32 **list_p,
				       struct device_node *np)
{
	const __be32 *list = *list_p;
	u32 val;

	val = be32_to_cpu(*list++);

	pin->pin_no = *pin_id = val & 0xffff;
	pin->mux = (val >> 16) & 0xf;
	pin->padctrl = be32_to_cpu(*list++);

	/* Set initial default pad config */
	if (pin->padctrl != ENSEMBLE_NO_PAD_CTL)
		ensemble_padconf_set_config(info, pin->pin_no, pin->padctrl);

	*list_p = list;

	dev_dbg(info->dev, "%ld: 0x%lx 0x%08lx", pin->pin_no,
		pin->mux, pin->padctrl);
}

static int ensemble_pinctrl_parse_groups(struct device_node *np, struct group_desc *grp,
					 struct ensemble_pinctrl *pctl, u32 index)
{
	struct ensemble_pin *pin;
	const __be32 *list;
	int size, i;

	dev_dbg(pctl->dev, "group(%d): %pOFn\n", index, np);

	/* Initialise group */
	grp->grp.name = np->name;

	list = of_get_property(np, "pinmux", &size);
	if (!list) {
		dev_err(pctl->dev,
			"no pins property in node %pOF\n", np);
		return -EINVAL;
	}

	/* we do not check return since it's safe node passed down */
	if (!size || size % PIN_SIZE) {
		dev_err(pctl->dev, "Invalid pins property in node %pOF\n", np);
		return -EINVAL;
	}

	grp->grp.npins = size / PIN_SIZE;
	grp->data = devm_kcalloc(pctl->dev, grp->grp.npins,
				 sizeof(*pin), GFP_KERNEL);
	grp->grp.pins = devm_kcalloc(pctl->dev, grp->grp.npins,
				     sizeof(unsigned int), GFP_KERNEL);
	if (!grp->grp.pins || !grp->data)
		return -ENOMEM;

	for (i = 0; i < grp->grp.npins; i++) {
		pin = &((struct ensemble_pin *)(grp->data))[i];
		ensemble_pinctrl_parse_pin(pctl, (unsigned int *)&grp->grp.pins[i], pin, &list, np);
	}
	return 0;
}

static int ensemble_pinctrl_probe_dt(struct platform_device *pdev, struct ensemble_pinctrl *ipctl)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct pinctrl_dev *pctl = ipctl->pctl;
	struct function_desc *func;
	struct group_desc *grp;
	const char **group_names;
	u32 i = 0;

	if (!np)
		return -ENODEV;

	func = devm_kzalloc(&pdev->dev, sizeof(*func), GFP_KERNEL);
	if (!func)
		return -ENOMEM;

	mutex_lock(&ipctl->mutex);
	radix_tree_insert(&pctl->pin_function_tree, i, func);
	mutex_unlock(&ipctl->mutex);

	pctl->num_functions = 1;
	ipctl->group_index = 0;
	pctl->num_groups = of_get_child_count(np);
	if (pctl->num_groups == 0) {
		dev_err(ipctl->dev, "no groups defined in %pOF\n", np);
		return 0;
	}

	/* Initialise function */
	func->func.name = np->name;
	func->func.ngroups = pctl->num_groups;
	group_names = devm_kcalloc(ipctl->dev, func->func.ngroups,
				   sizeof(*group_names), GFP_KERNEL);
	if (!group_names)
		return -ENOMEM;

	for_each_child_of_node(np, child) {
		group_names[i] = child->name;

		grp = devm_kzalloc(ipctl->dev, sizeof(*grp), GFP_KERNEL);
		if (!grp) {
			of_node_put(child);
			return -ENOMEM;
		}
		mutex_lock(&ipctl->mutex);
		radix_tree_insert(&pctl->pin_group_tree,
				  ipctl->group_index++, grp);
		mutex_unlock(&ipctl->mutex);

		ensemble_pinctrl_parse_groups(child, grp, ipctl, i++);
	}
	func->func.groups = group_names;
	return 0;
}

static int ensemble_pctl_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct ensemble_pinctrl *info;
	struct device *dev = &pdev->dev;
	struct pinctrl_desc *pctl_desc;

	if (!dev->of_node)
		return dev_err_probe(dev, -ENODEV, "device tree node not found\n");

	pctl_desc = devm_kzalloc(dev, sizeof(*pctl_desc), GFP_KERNEL);
	if (!pctl_desc)
		return -ENOMEM;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	mutex_init(&info->mutex);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	info->pinmux_base = devm_ioremap_resource(dev, res);

	platform_set_drvdata(pdev, info);

	pctl_desc->name	   = dev_name(dev);
	pctl_desc->pins    = ensemble_pins;
	pctl_desc->npins   = ARRAY_SIZE(ensemble_pins);
	pctl_desc->owner   = THIS_MODULE;
	pctl_desc->pctlops = &ensemble_pctl_ops;
	pctl_desc->pmxops  = &ensemble_pmx_ops;
	pctl_desc->confops = &ensemble_conf_ops;

	info->pctl = devm_pinctrl_register(dev, pctl_desc, info);
	if (IS_ERR(info->pctl)) {
		dev_err(dev, "Failed pinctrl registration\n");
		return PTR_ERR(info->pctl);
	}
	if (ensemble_pinctrl_probe_dt(pdev, info))
		dev_err(dev, "fail to probe dt properties\n");

	dev_info(dev, "Ensemble pinctrl initialized\n");
	return 0;
}

static const struct of_device_id ensemble_pctl_of_match[] = {
	{ .compatible = "alif,ensemble-pinctrl" },
	{ }
};

static struct platform_driver ensemble_pctl_driver = {
	.probe = ensemble_pctl_probe,
	.driver = {
		.name = "ensemble-pinctrl",
		.of_match_table = ensemble_pctl_of_match,
	},
};

static int __init ensemble_pinctrl_drv_register(void)
{
	return platform_driver_register(&ensemble_pctl_driver);
}
postcore_initcall(ensemble_pinctrl_drv_register);
