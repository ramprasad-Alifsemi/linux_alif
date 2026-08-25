// SPDX-License-Identifier: GPL-2.0
/*
 * Alif Hardware Semaphore Driver
 *
 * Copyright (c) 2021-2025, Alif Semiconductor
 * Author: Jagadeesh Krishnanjanappa <jagadeesh@alifsemi.com>
 */

#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/wait.h>

#define DRV_NAME "alif_hwsem"
#define HWSEM_NUM 16 /* a total of 16 hardware semaphores */

/* HWSEM register offsets */
#define HWSEM_ACQ_OFFSET 0x0 /* offset to acquire/lockk HWSEM */
#define HWSEM_REL_OFFSET 0x4 /* offset to release/unlock HWSEM */

/* MASTER_ID */
#ifndef A32_HWSEM_MST_ID /* A32_HWSEM_MST_ID sets MASTER_ID */
#define MASTER_ID 0xA1B2C3D4 /* Default MASTER_ID is 0xA1B2C3D4 */
#else
#define MASTER_ID A32_HWSEM_MST_ID
#endif

/* IOCTL commands */
#define HWSEM_RD_MST_ID 1 /* read MASTER_ID */
#define HWSEM_RD_CNT    3 /* read HWSEM count */
#define HWSEM_LOCK      4 /* lock HWSEM */
#define HWSEM_UNLOCK    5 /* unlock HWSEM */

struct hwsem {
	struct device *dev;
	struct miscdevice miscdev;
	void __iomem *hwsem_base;
	/* serializes access to @ready, @pid, @flags and the HWSEM registers */
	spinlock_t lock;
	unsigned long flags;
	wait_queue_head_t waitq;
	int irq;
	bool ready;
	int pid;
};

enum {
	HWSEM_IRQ_DISABLED = 0,
};

static int i;

static long hwsem_ioctl(struct file *f, unsigned int cmd,
			unsigned long arg)
{
	struct hwsem *drvdata = container_of(f->private_data,
					struct hwsem, miscdev);
	switch (cmd) {
	case HWSEM_RD_MST_ID:
		return readl(drvdata->hwsem_base + HWSEM_ACQ_OFFSET);
	case HWSEM_RD_CNT:
		return readl(drvdata->hwsem_base + HWSEM_REL_OFFSET);
	case HWSEM_UNLOCK:
		spin_lock(&drvdata->lock);
		writel(MASTER_ID, drvdata->hwsem_base + HWSEM_REL_OFFSET);
		/*
		 * Read count before releasing to avoid race condition.
		 * If count is 1 (or less), it means this unlock will free it completely.
		 */
		if (readl(drvdata->hwsem_base + HWSEM_REL_OFFSET) == 0)
			drvdata->pid = -1;
		spin_unlock(&drvdata->lock);
		break;
	case HWSEM_LOCK:
		spin_lock(&drvdata->lock);
		/*
		 * On first acquisition of semaphore, the process goes through the
		 * wait_event_interruptible() path in the while loop then sets pid
		 * under the spinlock.
		 * Below is the recursive lock condition and acquisition.
		 */
		if (drvdata->pid == current->tgid) {
			u32 count_before, count_after;

			/* Recursive lock: increment hardware count */
			/* HWSEM_REL_OFFSET is used to read the current lock count */
			count_before = readl(drvdata->hwsem_base + HWSEM_REL_OFFSET);
			writel(MASTER_ID, drvdata->hwsem_base + HWSEM_ACQ_OFFSET);
			count_after = readl(drvdata->hwsem_base + HWSEM_REL_OFFSET);

			/* Verify that the lock was actually acquired/incremented */
			if (count_after != count_before + 1) {
				dev_err(drvdata->dev, "Recursive lock failed to increment count\n");
				spin_unlock(&drvdata->lock);
				return -EIO;
			}
			spin_unlock(&drvdata->lock);
			return 0;
		}
		spin_unlock(&drvdata->lock);

		if (__test_and_clear_bit(HWSEM_IRQ_DISABLED, &drvdata->flags)) {
			enable_irq(drvdata->irq);
			synchronize_irq(drvdata->irq);
		}
		while (1) {
			/* Wait until,
			 *   1. HWSEM becomes free,i.e drvdata->ready is true and
			 *      HWSEM count is 0 (as other cores can own HWSEM).
			 *   2. HWSEM is owned by one of the child threads or
			 *      the current process.
			 */
			if (wait_event_interruptible(drvdata->waitq, drvdata->ready &&
						     !readl(drvdata->hwsem_base +
							    HWSEM_REL_OFFSET))) {
				dev_info(drvdata->dev,
					 "Process waiting for HWSEM lock was interrupted\n");
				return -ERESTARTSYS;
			}
			spin_lock(&drvdata->lock);

			/*
			 * Race condition check (TOCTOU):
			 * Another process might have acquired the lock between
			 * wait_event returning and us acquiring the spinlock.
			 * Re-check if it's still free.
			 * Note: ready flag is cleared to false after successful acquisition below.
			 */
			if (!drvdata->ready || readl(drvdata->hwsem_base + HWSEM_REL_OFFSET) != 0) {
				spin_unlock(&drvdata->lock);
				continue;
			}

			/* First acquisition by this process */
			drvdata->pid = current->tgid;
			writel(MASTER_ID,
			       drvdata->hwsem_base + HWSEM_ACQ_OFFSET);
			drvdata->ready = false;
			spin_unlock(&drvdata->lock);
			break;
		}
		return 0;
	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct file_operations hwsem_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hwsem_ioctl,
};

static irqreturn_t hwsem_isr(int irq, void *dev_id)
{
	struct hwsem *hwsem_dev_id = dev_id;

	/* Just disable the interrupt in the interrupt controller, and
	 * remember the state so we can enable while locking.
	 */
	spin_lock(&hwsem_dev_id->lock);
	if (!__test_and_set_bit(HWSEM_IRQ_DISABLED, &hwsem_dev_id->flags))
		disable_irq_nosync(irq);
	hwsem_dev_id->ready = true;
	spin_unlock(&hwsem_dev_id->lock);

	wake_up_interruptible(&hwsem_dev_id->waitq);
	return IRQ_HANDLED;
}

static char *hwsem_drv_itoa(int i)
{
	static char str[8];

	sprintf(str, "hwsem%d", i);
	return str;
}

static int hwsem_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct hwsem *hwsem_ptr;
	struct resource *res;
	void __iomem *hwsem_base;
	int ret;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	dev = &pdev->dev;

	hwsem_ptr = devm_kzalloc(&pdev->dev, sizeof(struct hwsem), GFP_KERNEL);
	if (!hwsem_ptr)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hwsem_base");
	if (!res) {
		dev_err(&pdev->dev, "No REG resource for hwsem_base\n");
		return -ENXIO;
	}
	hwsem_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(hwsem_base))
		return PTR_ERR(hwsem_base);

	dev_set_drvdata(dev, (void *)hwsem_ptr);

	spin_lock_init(&hwsem_ptr->lock);
	init_waitqueue_head(&hwsem_ptr->waitq);

	hwsem_ptr->hwsem_base = hwsem_base;
	hwsem_ptr->dev = &pdev->dev;
	hwsem_ptr->ready = false;
	hwsem_ptr->flags = 0;
	hwsem_ptr->pid = -1;

	hwsem_ptr->miscdev.minor = MISC_DYNAMIC_MINOR;
	hwsem_ptr->miscdev.name = hwsem_drv_itoa(i);
	hwsem_ptr->miscdev.fops = &hwsem_fops;
	hwsem_ptr->miscdev.parent = dev;

	ret = misc_register(&hwsem_ptr->miscdev);
	if (ret) {
		dev_err(dev, "Unable to register Hardware Semaphore device\n");
		return ret;
	}

	dev_info(hwsem_ptr->dev, "Alif hardware semaphore registered\n");
	hwsem_ptr->irq = platform_get_irq(pdev, 0);
	if (hwsem_ptr->irq < 0) {
		dev_err(hwsem_ptr->dev, "platform_get_irq() failed with %d\n",
			hwsem_ptr->irq);
		return hwsem_ptr->irq;
	}

	ret = devm_request_irq(&pdev->dev, hwsem_ptr->irq, hwsem_isr, IRQF_SHARED,
			       "hardware semaphore", hwsem_ptr);

	if (ret) {
		dev_err(hwsem_ptr->dev, "devm_request_irq() failed with %d\n",
			ret);
		return ret;
	}
	++i;

	return 0;
}

static void hwsem_remove(struct platform_device *pdev)
{
	struct hwsem *hwsem_ptr = platform_get_drvdata(pdev);

	misc_deregister(&hwsem_ptr->miscdev);
	devm_free_irq(&pdev->dev, hwsem_ptr->irq, hwsem_ptr);
}

#ifdef CONFIG_OF
static const struct of_device_id hwsem_match[] = {
	{ .compatible = "alif,hwsem" },
	{ }
};

MODULE_DEVICE_TABLE(of, hwsem_match);
#endif

static struct platform_driver hwsem_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(hwsem_match),
	},
	.probe = hwsem_probe,
	.remove_new = hwsem_remove,
};

module_platform_driver(hwsem_driver)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Alif Hardware Semaphore Driver");
MODULE_AUTHOR("jagadeesh@alifsemi.com>");
