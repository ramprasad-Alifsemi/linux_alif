// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALIF Event Router DMA driver
 *
 * Based on TI DMA Crossbar driver
 * Routes DMA events from peripherals to the DMA controller
 * Acts as a 4:1 multiplexer for 32 DMA peripheral request interfaces
 *
 * Copyright 2025 Alif Semiconductor.
 * Author: Aravind Krishnan <aravind.krishnan@alifsemi.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include <linux/spinlock.h>

#define DRIVER_NAME "alif-event-router"
#define MAX_CHANNELS 32
#define MAX_SOURCES 4
#define EVTRTR0_DMA_CTRL(n)	(0x0 + ((n) * 4))
#define EVTRTR0_DMA_REQ_CTRL	0x80
#define EVTRTR0_DMA_ACK_TYPE0	0x90
#define EVTRTR0_DMA_ACK_TYPE1	0x94
#define EVTRTR0_DMA_ACK_TYPE2	0x98
#define EVTRTR0_DMA_ACK_TYPE3	0x9C
#define CTRL_SEL_SOURCE_MASK	GENMASK(1, 0)
#define DMA_CTRL_ENABLE		BIT(4)
#define DMA_REQ_CTRL_CLS	BIT(0)
#define DMA_REQ_CTRL_CLB	BIT(4)
#define DMA_REQ_CTRL_CS		BIT(8)
#define DMA_REQ_CTRL_CB		BIT(12)

struct alif_event_router {
	struct device *dev;
	void __iomem *regs;
	struct dma_router dma_router;
	struct device_node *dma_master;
	unsigned long channels_allocated;
	/* Lock for protecting channel allocation */
	spinlock_t lock;
};

static void alif_event_router_free(struct device *dev, void *route_data)
{
	struct alif_event_router *router = dev_get_drvdata(dev);
	unsigned long flags;
	u32 channel = (u32)(uintptr_t)route_data;

	spin_lock_irqsave(&router->lock, flags);

	clear_bit(channel, &router->channels_allocated);

	spin_unlock_irqrestore(&router->lock, flags);
}

static void alif_event_router_dma_cfg(struct device *dev, u32 channel, u32 source)
{
	struct alif_event_router *router = dev_get_drvdata(dev);
	u32 val = 0;
	u32 reg = 0;
	unsigned long flags;
	void __iomem *router_base = router->regs;

	spin_lock_irqsave(&router->lock, flags);

	if (test_and_set_bit(channel, &router->channels_allocated)) {
		dev_err(dev, "Channel %d already allocated\n", channel);
		goto unlock;
	}

	reg = EVTRTR0_DMA_CTRL(channel);
	val = (source & CTRL_SEL_SOURCE_MASK) | DMA_CTRL_ENABLE;
	writel(val, router_base + reg);

	reg = EVTRTR0_DMA_REQ_CTRL;
	val = DMA_REQ_CTRL_CLS | DMA_REQ_CTRL_CLB | DMA_REQ_CTRL_CS | DMA_REQ_CTRL_CB;
	writel(val, router_base + reg);

	switch (source) {
	case 0:
		reg = EVTRTR0_DMA_ACK_TYPE0;
		break;
	case 1:
		reg = EVTRTR0_DMA_ACK_TYPE1;
		break;
	case 2:
		reg = EVTRTR0_DMA_ACK_TYPE2;
		break;
	case 3:
		reg = EVTRTR0_DMA_ACK_TYPE3;
		break;
	default:
		dev_err(dev, "Invalid source %d\n", source);
		goto unlock;
	}
	val = readl(router_base + reg);
	val |= BIT(channel);
	writel(val, router_base + reg);

unlock:
	spin_unlock_irqrestore(&router->lock, flags);
}

static void *alif_event_router_route_allocate(struct of_phandle_args *dma_spec,
					      struct of_dma *ofdma)
{
	struct platform_device *pdev = of_find_device_by_node(ofdma->of_node);
	struct alif_event_router *router = platform_get_drvdata(pdev);
	u32 channel, source;

	if (dma_spec->args_count != 2) {
		dev_err(router->dev, "Invalid number of DMA specifier cells\n");
		return ERR_PTR(-EINVAL);
	}

	channel = dma_spec->args[0];
	source = dma_spec->args[1];

	if (channel >= MAX_CHANNELS) {
		dev_err(router->dev, "Invalid DMA channel %d (must be 0-%d)\n",
			channel, MAX_CHANNELS - 1);
		return ERR_PTR(-EINVAL);
	}

	if (source >= MAX_SOURCES) {
		dev_err(router->dev, "Invalid DMA source %d (must be 0-%d)\n",
			source, MAX_SOURCES - 1);
		return ERR_PTR(-EINVAL);
	}

	/* Configure the event router */
	alif_event_router_dma_cfg(router->dev, channel, source);

	dma_spec->np = of_node_get(router->dma_master);
	dma_spec->args_count = 1;
	return (void *)(uintptr_t)channel;
}

static int alif_event_router_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct alif_event_router *router;
	int ret;

	router = devm_kzalloc(dev, sizeof(*router), GFP_KERNEL);
	if (!router)
		return -ENOMEM;

	router->dev = dev;

	/* Get memory mapped registers */
	router->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(router->regs))
		return PTR_ERR(router->regs);

	spin_lock_init(&router->lock);

	router->dma_master = of_parse_phandle(node, "dma-masters", 0);
	if (!router->dma_master) {
		dev_err(dev, "Can't get DMA master node\n");
		return -ENODEV;
	}
	router->dma_router.dev = dev;
	router->dma_router.route_free = alif_event_router_free;

	platform_set_drvdata(pdev, router);

	ret = of_dma_router_register(node, alif_event_router_route_allocate, &router->dma_router);
	if (ret) {
		dev_err(dev, "Failed to register DMA router\n");
		of_node_put(router->dma_master);
		return ret;
	}

	dev_info(dev, "ALIF Event Router DMA registered with %d channels\n",
		 MAX_CHANNELS);
	return 0;
}

static const struct of_device_id alif_event_router_of_match[] = {
	{ .compatible = "alif,event-router" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, alif_event_router_of_match);

static struct platform_driver alif_event_router_driver = {
	.driver = {
	    .name = DRIVER_NAME,
	    .of_match_table = alif_event_router_of_match,
	},
	.probe = alif_event_router_probe,
};

module_platform_driver(alif_event_router_driver);

MODULE_AUTHOR("Aravind Krishnan <aravind.krishnan@alifsemi.com>");
MODULE_DESCRIPTION("ALIF Event Router DMA Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:alif-event-router");
