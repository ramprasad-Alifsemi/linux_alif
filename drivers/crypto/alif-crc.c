// SPDX-License-Identifier: GPL-2.0-only

/*
 * Alif CRC Driver
 * Copyright (c) 2021-2025 Alif Semiconductor
 */

#include <linux/bitrev.h>
#include <linux/crc32.h>
#include <linux/crc32poly.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/byteorder.h>
#include <crypto/internal/hash.h>
#include <linux/unaligned.h>
#include <linux/io.h>

#define DRIVER_NAME             "alif-crc32"
#define CHKSUM_DIGEST_SIZE      4
#define CHKSUM_BLOCK_SIZE       1

#define ALIF_CRC_PRIORITY_HIGH   200
#define ALIF_CRC_PRIORITY_LOW    100

#define CRC_CTRL                0x00000000
#define CRC_SEED                0x00000010
#define CRC_RESULT              0x00000018
#define MAX_LEN                 156000
#define CRC_DATA                0x00000060
#define CRC_DATA16              0x00000020

#define CRC_KEY_DEFAULT		0xFFFFFFFF
#define CRC_KEY_ZERO		0x00000000

#define CRC_INIT_VALUE_CRC32          0xd25
#define CRC_INIT_VALUE_CRC32C         0xd2d
#define CRC_INIT_VALUE_CRC16          0x13
#define CRC_INIT_VALUE_CRC16_CCITT    0x1b
#define CRC_INIT_VALUE_CRC8           0x1

enum alif_crc_poly {
	CRC32_POLY_CRC32 = 1,
	CRC32_POLY_CRC32C,
	CRC16_POLY_CRC16,
	CRC16_POLY_CRC16_CCITT,
	CRC8_POLY_CRC8,
	CRC32_POLY_CRC32_LINUX
};

struct alif_crc {
	struct list_head list;
	struct device    *dev;
	/* Lock for protecting CRC device access */
	spinlock_t lock;
	u8 extra_data[sizeof(u32)];
	u32 num_extra;
	void __iomem *regs;
};

struct alif_crc_list {
	struct list_head dev_list;
	/* Lock for protecting device list */
	spinlock_t	lock;
};

static struct alif_crc_list crc_list = {
	.dev_list = LIST_HEAD_INIT(crc_list.dev_list),
	.lock	= __SPIN_LOCK_UNLOCKED(crc_list.lock),
};

struct alif_crc_ctx {
	u32 key;
	u32 poly;
	u32 init;
};

struct alif_crc_desc_ctx {
	u32 partial_result;
};

static u32 bit_reflect(u32 input)
{
	u32 refin = 0;
	u32 bit;
	unsigned int i;

	for (i = 0; i < 32; i++) {
		bit = (input >> i) & 1;
		bit = bit << (32 - (i + 1));
		refin |= bit;
	}
	return refin;
}

static int crc32_cra_init(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_DEFAULT;
	mtx->poly = CRC32_POLY_CRC32;
	mtx->init = CRC_INIT_VALUE_CRC32;

	return 0;
}

static int crc32_cra_init_linux(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_ZERO;
	mtx->poly = CRC32_POLY_CRC32_LINUX;
	mtx->init = CRC_INIT_VALUE_CRC32;

	return 0;
}

static int crc32c_cra_init(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_DEFAULT;
	mtx->poly = CRC32_POLY_CRC32C;
	mtx->init = CRC_INIT_VALUE_CRC32C;

	return 0;
}

static int crc16_cra_init(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_ZERO;
	mtx->poly = CRC16_POLY_CRC16;
	mtx->init = CRC_INIT_VALUE_CRC16;

	return 0;
}

static int crc16_ccitt_cra_init(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_ZERO;
	mtx->poly = CRC16_POLY_CRC16_CCITT;
	mtx->init = CRC_INIT_VALUE_CRC16_CCITT;

	return 0;
}

static int crc8_init(struct crypto_tfm *tfm)
{
	struct alif_crc_ctx *mtx = crypto_tfm_ctx(tfm);

	mtx->key = CRC_KEY_ZERO;
	mtx->poly = CRC8_POLY_CRC8;
	mtx->init = CRC_INIT_VALUE_CRC8;

	return 0;
}

static int crc_setkey(struct crypto_shash *tfm, const u8 *key, unsigned int keylen)
{
	struct alif_crc_ctx *mctx = crypto_shash_ctx(tfm);

	if (keylen != sizeof(u32))
		return -EINVAL;

	mctx->key = get_unaligned_le32(key);

	return 0;
}

static struct alif_crc *alif_crc_get_next_crc(void)
{
	struct alif_crc *crc;

	spin_lock_bh(&crc_list.lock);
	crc = list_first_entry_or_null(&crc_list.dev_list, struct alif_crc, list);
	if (crc)
		list_move_tail(&crc->list, &crc_list.dev_list);
	spin_unlock_bh(&crc_list.lock);

	return crc;
}

static int crc_init_crc_ctx(struct shash_desc *desc)
{
	struct alif_crc_desc_ctx *ctx = shash_desc_ctx(desc);
	struct alif_crc_ctx *mctx = crypto_shash_ctx(desc->tfm);
	struct alif_crc *crc;

	crc = alif_crc_get_next_crc();
	if (!crc)
		return -ENODEV;

	spin_lock(&crc->lock);
	if (mctx->poly == CRC32_POLY_CRC32_LINUX)
		writel(bit_reflect(mctx->key), crc->regs + CRC_SEED);
	else
		writel(mctx->key, crc->regs + CRC_SEED);

	writel(mctx->init, crc->regs + CRC_CTRL);

	if (mctx->poly == CRC32_POLY_CRC32_LINUX)
		ctx->partial_result = mctx->key ^ (~0);
	else
		ctx->partial_result = readl(crc->regs + CRC_RESULT);

	spin_unlock(&crc->lock);

	crc->num_extra = 0;

	return 0;
}

static unsigned int crc32_update_unaligned(u32 crc, const u8 *data, unsigned int length)
{
	return crc32_le(crc, data, length) ^ (~0);
}

static unsigned int crc32c_update_unaligned(u32 crc, const u8 *data, unsigned int length)
{
	return __crc32c_le(crc, data, length) ^ (~0);
}

static int crc_update(struct shash_desc *desc, const u8 *datain, unsigned int length)
{
	struct alif_crc_desc_ctx *ctx = shash_desc_ctx(desc);
	struct alif_crc *crc;

	crc = alif_crc_get_next_crc();
	if (!crc)
		return -ENODEV;

	unsigned int i;
	u32 value;
	u32 num_writes;
	u32 *d32;
	u8 *data;

	if (!datain || length == 0)
		return 0;

	data = kmalloc(length + 4, GFP_KERNEL);
	if (!data)
		return 0;

	if (crc->num_extra > 0)
		memcpy(data, crc->extra_data, crc->num_extra);

	memcpy(data + crc->num_extra, datain, length);

	length += crc->num_extra;

	num_writes = length / sizeof(u32);
	crc->num_extra = length % sizeof(u32);

	d32 = (u32 *)data;

	spin_lock(&crc->lock);
	for (i = 0; i < num_writes; i++) {
		value = *(d32++);
		value = __be32_to_cpu(value);
		writel(value, crc->regs + CRC_DATA);
	}

	ctx->partial_result = readl(crc->regs + CRC_RESULT);

	spin_unlock(&crc->lock);

	if (crc->num_extra)
		memcpy(crc->extra_data, d32, crc->num_extra);

	kfree(data);

	return 0;
}

static int crc16_update(struct shash_desc *desc, const u8 *datain, unsigned int length)
{
	struct alif_crc_desc_ctx *ctx = shash_desc_ctx(desc);
	struct alif_crc *crc;

	crc = alif_crc_get_next_crc();
	if (!crc)
		return -ENODEV;

	unsigned int i;
	u8 value;

	if (!datain)
		return 0;

	spin_lock(&crc->lock);
	for (i = 0; i < length; i++) {
		value = *(datain++);
		writeb(value, crc->regs + CRC_DATA16);
	}

	ctx->partial_result = readl(crc->regs + CRC_RESULT);

	spin_unlock(&crc->lock);

	return 0;
}

static int crc_final(struct shash_desc *desc, u8 *out)
{
	struct alif_crc_desc_ctx *ctx = shash_desc_ctx(desc);
	struct alif_crc_ctx *mctx = crypto_shash_ctx(desc->tfm);
	struct alif_crc *crc;

	crc = alif_crc_get_next_crc();
	if (!crc)
		return -ENODEV;

	u32 result;

	if (crc->num_extra > 0) {
		result = ctx->partial_result ^ (~0);
		if (mctx->poly == CRC32_POLY_CRC32 || mctx->poly == CRC32_POLY_CRC32_LINUX)
			ctx->partial_result = crc32_update_unaligned(result, crc->extra_data,
								     crc->num_extra);
		else
			ctx->partial_result = crc32c_update_unaligned(result, crc->extra_data,
								      crc->num_extra);
	}

	if (mctx->poly == CRC32_POLY_CRC32_LINUX)
		ctx->partial_result = ctx->partial_result ^ (~0);

	if (mctx->poly == CRC32_POLY_CRC32 || mctx->poly == CRC32_POLY_CRC32C ||
	    mctx->poly == CRC32_POLY_CRC32_LINUX)
		put_unaligned_le32(ctx->partial_result, out);
	else if (mctx->poly == CRC16_POLY_CRC16 || mctx->poly == CRC16_POLY_CRC16_CCITT)
		put_unaligned_le16(ctx->partial_result, out);
	else
		*out = (u8)ctx->partial_result;

	return 0;
}

static int crc_finup(struct shash_desc *desc, const u8 *data, unsigned int length, u8 *out)
{
	struct alif_crc_ctx *mctx = crypto_shash_ctx(desc->tfm);
	int ret = -EINVAL;

	pr_info("Processing %s CRC\n",
		(mctx->poly == CRC32_POLY_CRC32 || mctx->poly == CRC32_POLY_CRC32C ||
		 mctx->poly == CRC32_POLY_CRC32_LINUX) ? "32-bit" :
		(mctx->poly == CRC8_POLY_CRC8) ? "8-bit" : "16-bit");

	if (mctx->poly == CRC32_POLY_CRC32 || mctx->poly == CRC32_POLY_CRC32C ||
	    mctx->poly == CRC32_POLY_CRC32_LINUX) {
		ret = crc_update(desc, data, length);
	} else if (mctx->poly == CRC16_POLY_CRC16 || mctx->poly == CRC16_POLY_CRC16_CCITT ||
		   mctx->poly == CRC8_POLY_CRC8) {
		ret = crc16_update(desc, data, length);
	} else {
		pr_err("Unsupported CRC type %d\n", mctx->poly);
	}

	return ret == 0 ? crc_final(desc, out) : ret;
}

static int crc_digest(struct shash_desc *desc, const u8 *data, unsigned int length, u8 *out)
{
	return crc_init_crc_ctx(desc) ? : crc_finup(desc, data, length, out);
}

static unsigned int refcnt;
static DEFINE_MUTEX(refcnt_lock);

static struct shash_alg crc_alg[] = {
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crc",
		.cra_driver_name        = "alif-crc32-alif-crc",
		.cra_priority           = ALIF_CRC_PRIORITY_HIGH,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc32_cra_init,
	}
	},
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crcc",
		.cra_driver_name        = "alif-crc32-alif-crcc",
		.cra_priority           = ALIF_CRC_PRIORITY_HIGH,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc32c_cra_init,
	}
	},
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crc-linux",
		.cra_driver_name        = "alif-crc32-alif-crc-linux",
		.cra_priority           = ALIF_CRC_PRIORITY_LOW,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc32_cra_init_linux,
	}
	},
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc16_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crc16",
		.cra_driver_name        = "alif-crc32-alif-crc16",
		.cra_priority           = ALIF_CRC_PRIORITY_HIGH,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc16_cra_init,
	}
	},
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc16_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crc16-ccitt",
		.cra_driver_name        = "alif-crc32-alif-crc16-ccitt",
		.cra_priority           = ALIF_CRC_PRIORITY_HIGH,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc16_ccitt_cra_init,
	}
	},
	{
	.setkey         = crc_setkey,
	.init           = crc_init_crc_ctx,
	.update         = crc16_update,
	.final          = crc_final,
	.finup          = crc_finup,
	.digest         = crc_digest,
	.descsize       = sizeof(struct alif_crc_desc_ctx),
	.digestsize     = CHKSUM_DIGEST_SIZE,
	.base           = {
		.cra_name               = "alif-crc8",
		.cra_driver_name        = "alif-crc32-alif-crc8",
		.cra_priority           = ALIF_CRC_PRIORITY_HIGH,
		.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
		.cra_blocksize          = CHKSUM_BLOCK_SIZE,
		.cra_ctxsize            = sizeof(struct alif_crc_ctx),
		.cra_module             = THIS_MODULE,
		.cra_init               = crc8_init,
	}
	}
};

static int alif_crc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct alif_crc *crc;
	int ret;

	crc = devm_kzalloc(dev, sizeof(*crc), GFP_KERNEL);
	if (!crc)
		return -ENOMEM;

	crc->dev = dev;

	crc->regs = devm_platform_ioremap_resource(pdev, 0);

	if (IS_ERR(crc->regs)) {
		dev_err(dev, "Cannot map CRC Registers\n");
		return PTR_ERR(crc->regs);
	}

	spin_lock_init(&crc->lock);

	platform_set_drvdata(pdev, crc);

	spin_lock(&crc_list.lock);
	list_add(&crc->list, &crc_list.dev_list);
	spin_unlock(&crc_list.lock);

	mutex_lock(&refcnt_lock);
	if (!refcnt) {
		ret = crypto_register_shashes(crc_alg, ARRAY_SIZE(crc_alg));
		if (ret) {
			mutex_unlock(&refcnt_lock);
			dev_err(dev, "Failed to register\n");
			return ret;
		}
	}
	refcnt++;
	mutex_unlock(&refcnt_lock);

	return 0;
}

static void alif_crc_remove(struct platform_device *pdev)
{
	mutex_lock(&refcnt_lock);
	if (!--refcnt)
		crypto_unregister_shashes(crc_alg, ARRAY_SIZE(crc_alg));
	mutex_unlock(&refcnt_lock);
}

static const struct of_device_id alif_ids[] = {
	{.compatible = "alif,alif-crccode",},
	{},
};
MODULE_DEVICE_TABLE(of, alif_ids);

static struct platform_driver alif_crc_driver = {
	.probe  = alif_crc_probe,
	.remove = alif_crc_remove,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = alif_ids,
	},
};

module_platform_driver(alif_crc_driver);
MODULE_DESCRIPTION("Alif CRC hardware driver");
MODULE_LICENSE("GPL");
