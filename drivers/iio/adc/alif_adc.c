// SPDX-License-Identifier: GPL-2.0+
/*
 *  alif_adc.c - Support for ADC12/ADC24
 *
 *  8-channels, 12-bit ADC and 4 differential channels, 24-bit ADC
 *
 *  Copyright (C) 2024 Pankaj Pandey <pankaj.pandey@alifsemi.com>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/iio/sysfs.h>
#include <linux/mod_devicetable.h>
#include "alif_temp.h"

extern int temp_data[][2];
u32 channel_sel;

/* ADC_INTERRUPT register bit definitions */
#define AVG_SAMPLE_RDY		BIT(0)		/* Averaged sample ready */
#define ALL_SAMPLES_TAKEN	BIT(1)		/* All sequencer samples captured */
#define AVG_CALC_THRESH_0	BIT(2)		/* Average crossed threshold A */
#define AVG_CALC_THRESH_1	BIT(3)		/* Average crossed threshold B */
#define ADC_INTERRUPT_FLAGS	(AVG_SAMPLE_RDY | ALL_SAMPLES_TAKEN |	\
				AVG_CALC_THRESH_0 | AVG_CALC_THRESH_1)
#define ADC_INTERRUPT_ALL_MASK	(GENMASK(3, 0))

#define ADC_24BIT_READ		(GENMASK(23, 0))
#define ADC_12BIT_READ		(GENMASK(9, 0))
#define ADC_16BIT_READ		(GENMASK(19, 4))
#define ADC_8BIT_READ		(GENMASK(7, 0))
/* LSB is used to calculate analog voltage value
 * from the number of ADC sample value
 * For ADC24 (bipolar mode, with sign bit):
 *   Analog value = adc sample * vref / (2 ^ 23 - 1);
 * For ADC12 (unsigned):
 *   Analog value = adc sample * vref / (2 ^ 12 - 1);
 */
#define ADC_24BIT_LSB_DIV	BIT(23)
#define ADC_12BIT_LSB_DIV	BIT(12)

#define ALIFAD_IN		BIT(4)

#define ALIF_NAME "adc-dev"

/* PMU_PERIPH offset */
#define PMU_PERIPH_OFFSET	(0x40)

/* CMP Register Offset */
#define CMP_COMP_REG2_OFFSET	0x4

/* Analog Vref setting */
#define ANA_PERIPH_LDO_CONT_POS		6U
#define ANA_PERIPH_BG_CONT_POS		1U

#define ANA_PERIPH_LDO_CONT		(0xAU << ANA_PERIPH_LDO_CONT_POS)
#define ANA_PERIPH_BG_CONT		(0xAU << ANA_PERIPH_BG_CONT_POS)

/*
 * ALIF ADC Register Map (offsets from adc_base)
 *
 * Offset | Register            | Description
 * -------|---------------------|------------------------------
 * 0x00   | START_SRC           | Conversion start control
 * 0x04   | COMP_THRESH_A       | Comparator threshold A
 * 0x08   | COMP_THRESH_B       | Comparator threshold B
 * 0x0C   | CLK_DIVISOR         | ADC clock divider
 * 0x10   | INTERRUPT           | Interrupt status
 * 0x14   | INTERRUPT_MASK      | Interrupt mask
 * 0x18   | SAMPLE_WIDTH        | Sample width control
 * 0x20   | AVG_NUM             | Number of samples to average
 * 0x24   | SHIFT_CONTROL       | Data shift control
 * 0x30   | CONTROL             | General control
 * 0x34   | SEQUENCER_CTRL      | Channel sequencer control
 * 0x38   | REG1                | ADC instance config (per ADC12x)
 * 0x3C   | SEL                 | Current channel select
 * 0x50+  | SAMPLE_REG_0..n     | Sample result registers
 */

#define ADC_START_SRC		(0x00)
#define ADC_COMP_THRESH_A	(0x04)
#define ADC_COMP_THRESH_B	(0x08)
#define ADC_CLK_DIVISOR		(0x0C)
#define ADC_INTERRUPT		(0x10)
#define ADC_INTERRUPT_MASK	(0x14)
#define ADC_SAMPLE_WIDTH	(0x18)
#define ADC_AVG_NUM		(0x20)
#define ADC_SHIFT_CONTROL	(0x24)
#define ADC_CONTROL		(0x30)
#define ADC_SEQUENCER_CTRL	(0x34)
#define ADC_REG1		(0x38)
#define ADC_SEL			(0x3C)
#define ADC_SAMPLE_REG_0	(0x50)

/****ADC Register macros****/
#define ADC_START_CONTINUOUS_CONV_POS		6U
#define ADC_START_ENABLE_POS			7U
#define ADC_START_SINGLE_SHOT_CONV_POS		0U

#define ADC_START_CONTINUOUS_CONV		BIT(ADC_START_CONTINUOUS_CONV_POS)
#define ADC_START_ENABLE			BIT(ADC_START_ENABLE_POS)
#define ADC_START_SINGLE_SHOT_CONV		BIT(ADC_START_SINGLE_SHOT_CONV_POS)

/* Sample width */
#define ADC12_SAMPLE_WIDTH_MSK		(GENMASK(15, 0))

#define SHIFT_DIR_POS			16U
#define SHIFT_CONTROL_POS		8U

#define SHIFT_DIR_LEFT		(0 << SHIFT_DIR_POS)
#define SHIFT_DIR_RIGHT		BIT(SHIFT_DIR_POS)

#define SHIFT_CONTROL		BIT(SHIFT_CONTROL_POS)

/**** ADC24 Specific Settings ****/
#define ADC24_SHIFT_AMOUNT		(8U)	/* because 2^8 = 256 average samples */
#define ADC24_SHIFT_CONTROL_SETTING	(SHIFT_DIR_RIGHT | ADC24_SHIFT_AMOUNT)

/**** Sample & Hold ****/
#define SAMPLE_HOLD_POS			16U
#define SAMPLE_HOLD			BIT(SAMPLE_HOLD_POS)

/**** Comparator Settings ****/
#define COMP_THRESHOLD_A_POS		0U
#define COMP_THRESHOLD_B_POS		0U
#define COMP_EN_POS			16U

#define COMP_THRESHOLD_A		BIT(COMP_THRESHOLD_A_POS)
#define COMP_THRESHOLD_B		BIT(COMP_THRESHOLD_B_POS)
#define COMP_EN				(0U << COMP_EN_POS)

/****Sequencer Macros****/
#define ADC_SEQUENCER_MSK_BIT		BIT(0)
#define ADC_SEQUENCER_INIT_MASK		(GENMASK(15, 12))
#define ADC_MAX_INIT_CHANNEL		BIT(8)
#define ADC_MSK_INIT_CHANNEL		(GENMASK(3, 0))
#define ADC_MSK_ALL_CHANNELS		(GENMASK(8, 0))

/********Interrupt mask macro*******/
#define ADC_INTR_CMPA_POS		2U
#define ADC_INTR_CMPA_MSK		BIT(ADC_INTR_CMPA_POS)
#define ADC_INTR_CMPB_POS		3U
#define ADC_INTR_CMPB_MSK		BIT(ADC_INTR_CMPB_POS)
#define ADC_THRSHLD_CMP_MASK_BIT_POS	16U
#define ADC_THRSHLD_CMP_MASK_BIT	(0x03 << ADC_THRSHLD_CMP_MASK_BIT_POS)

/****Shift bit macro****/
#define ADC_SHIFT_BIT		16U
#define ADC_SEQUENCER_INIT_POS	12U

/****Comparator Macros****/
#define ADC_CMP_THRHLD_ABOVE_A		(0)
#define ADC_CMP_THRHLD_BELOW_A		(1)
#define ADC_CMP_THRHLD_BETWEEN_A_B	(2)

/* ADC reg1 position macro */
#define ADC120_DIFFERENTIAL_EN_POS (1)
#define ADC120_COMPARATOR_EN_POS   (2)
#define ADC120_COMPARATOR_BIAS_POS (3)
#define ADC120_VCM_DIV_POS         (5)

#define ADC121_DIFFERENTIAL_EN_POS (1)
#define ADC121_COMPARATOR_EN_POS   (2)
#define ADC121_COMPARATOR_BIAS_POS (3)
#define ADC121_VCM_DIV_POS         (5)

#define ADC122_DIFFERENTIAL_EN_POS (1)
#define ADC122_COMPARATOR_EN_POS   (2)
#define ADC122_COMPARATOR_BIAS_POS (3)
#define ADC122_VCM_DIV_POS         (5)

/* PMU_PERIPH field definitions */
#define PMU_PERIPH_ADC24_EN			BIT(12)
#define PMU_PERIPH_ADC24_OUTPUT_RATE_POS	13U
#define PMU_PERIPH_ADC24_OUTPUT_RATE_MSK	(GENMASK(15, 13))
#define PMU_PERIPH_ADC24_BIAS_POS		20U
#define PMU_PERIPH_ADC24_BIAS_MSK		(GENMASK(22, 20))

/* ADC reference voltage: 1.76V, stored as 176 (divide by 100 to get volts) */
#define ADC_VREF_CENTIVOLTS		176

/* Program clk divisor from 2 to 16 on ADC_CLK_DIVISOR register */
static inline u32 alif_adc_clk_divisor(u32 x)
{
	return x < 16 ? x : 16;
}

/* Program Sample number from 2 to 256 to ADC_AVG_NUM register */
static inline u32 alif_adc_avg_num(u32 x)
{
	return x < 256 ? x : 256;
}

/* Program Sample width from 2 to 32 to ADC_SAMPLE_WIDTH register */
static inline u32 alif_adc_sample_width(u32 x)
{
	return x < 32 ? x : 32;
}

#define TIMEOUT_MS 100

#define ADC_DONE0_INTERRUPT     BIT(0)
#define ADC_DONE1_INTERRUPT     BIT(1)

#define ADC120_BASE 0x49020000
#define ADC121_BASE 0x49021000
#define ADC122_BASE 0x49022000
#define ADC24_BASE 0x49027000

enum ADC_SCAN_MODE {
	ADC_SCAN_MODE_MULTI_CH,
	ADC_SCAN_MODE_SINGLE_CH
};

enum COMPARATOR_BIAS {
	COMPARATOR_BIAS_0_5,   /* 0.5 MS/s */
	COMPARATOR_BIAS_1_0,   /* 1 MS/s */
	COMPARATOR_BIAS_2_5,   /* 2.5 MS/s */
	COMPARATOR_BIAS_5_0    /* 5 MS/s */
};

enum ADC24_BIAS {
	ADC24_BIAS_5_0,   /* 5uA */
	ADC24_BIAS_6_2,   /* 6.2uA */
	ADC24_BIAS_7_5,   /* 7.5uA */
	ADC24_BIAS_8_75   /* 8.75uA */
};

enum ADC_PGA_GAIN {
	ADC_PGA_GAIN_0_DB,
	ADC_PGA_GAIN_6_DB,
	ADC_PGA_GAIN_12_DB,
	ADC_PGA_GAIN_18_DB,
	ADC_PGA_GAIN_24_DB,
	ADC_PGA_GAIN_30_DB,
	ADC_PGA_GAIN_36_DB,
	ADC_PGA_GAIN_42_DB,
};

enum ADC24_OUTPUT_RATE {
	ADC_OUTPUT_RATE_1K, /* 1KS/s */
	ADC_OUTPUT_RATE_2K, /* 2KS/s */
	ADC_OUTPUT_RATE_3K, /* 3KS/s */
	ADC_OUTPUT_RATE_4K, /* 4KS/s */
	ADC_OUTPUT_RATE_16K, /* 16KS/s */
};

/**
 * enum ADC_CONV_STAT.
 * Status of an ongoing ADC conversion.
 */
enum ADC_CONV_STAT {
	ADC_CONV_STAT_CMP_THLD_ABOVE_A = BIT(0),
	ADC_CONV_STAT_CMP_THLD_ABOVE_B = BIT(1),
	ADC_CONV_STAT_CMP_THLD_BELOW_A = BIT(2),
	ADC_CONV_STAT_CMP_THLD_BELOW_B = BIT(3),
	ADC_CONV_STAT_CMP_THLD_BETWEEN_A_B = BIT(4),
	ADC_CONV_STAT_CMP_THLD_OUTSIDE_A_B = BIT(5),
};

struct alif_adc_state {
	void __iomem *adc_base;
	void __iomem *adc_reg1;
	struct clk *clk;
	struct completion completion;
	struct device dev;
	const char *name;
	u32 *buffer;
	u32 sample_width;
	u32 comparator;
	u32 instance;
	u32 value;
	u32 vref;
	u32 clk_div;
	u32 avg_sample;
	u32 width_sample;
	u32 sample_hold;
	u32 shift_control_val;
	u8 differential;
	u8 comparator_en;
	u8 comparator_bias;
	u8 adc24_bias;
	u8 adc24_output_rate;
	/* serializes concurrent readers of @value in alif_read_raw() */
	spinlock_t lock_s;
	void __iomem *cmp_base;
};

/* ADC12/ADC24 instances */
enum ADC_INSTANCE {
	ADC_INSTANCE_ADC12_0,
	ADC_INSTANCE_ADC12_1,
	ADC_INSTANCE_ADC12_2,
	ADC_INSTANCE_ADC24_0,
};

#define ALIF_ADC_CHANNEL_BASE(_channel, _scan_index, _address, _chan_type) \
		.type = (_chan_type),				\
		.indexed = 1,					\
		.channel = (_channel),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
		.address = (_address),				\
		.scan_index = (_scan_index),

#define ALIF_ADC_CHANNEL(_channel, _scan_index, _address, _chan_type) { \
		ALIF_ADC_CHANNEL_BASE(_channel, _scan_index, _address, \
				      _chan_type)		\
}

#define ALIF_ADC_TEMPERATURE_CHAN(_channel, _scan_index, _chan_type) { \
		.type = (_chan_type),				\
		.channel = (_channel),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
				      BIT(IIO_CHAN_INFO_SCALE),	\
		.scan_index = (_scan_index),			\
		.scan_type = {					\
			.sign = 'u',				\
			.realbits = 12,				\
			.storagebits = 16,			\
		},						\
}

#define ALIF_ADC_CHANNEL_DIFF(chan1, chan2, si)             \
		{                                               \
	.type = IIO_VOLTAGE,                                    \
	.indexed = 1,                                           \
	.channel = (chan1),                                     \
	.channel2 = (chan2),                                    \
	.differential = 1,                                      \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),           \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),   \
	.scan_index = si,                                       \
	.scan_type = {                                          \
			.sign = 'u',                            \
			.realbits = 12,                         \
			.storagebits = 16,                      \
		},                                              \
		}

static const struct iio_chan_spec alif_adc_iio_channels[] = {
ALIF_ADC_CHANNEL(0, 0, ALIFAD_IN * 0, IIO_VOLTAGE),
ALIF_ADC_CHANNEL(1, 1, ALIFAD_IN * 1, IIO_VOLTAGE),
ALIF_ADC_CHANNEL(2, 2, ALIFAD_IN * 2, IIO_VOLTAGE),
ALIF_ADC_CHANNEL(3, 3, ALIFAD_IN * 3, IIO_VOLTAGE),
ALIF_ADC_CHANNEL(4, 4, ALIFAD_IN * 4, IIO_VOLTAGE),
ALIF_ADC_CHANNEL(5, 5, ALIFAD_IN * 5, IIO_VOLTAGE),
ALIF_ADC_TEMPERATURE_CHAN(6, 6, IIO_TEMP),
ALIF_ADC_CHANNEL(7, 7, ALIFAD_IN * 7, IIO_VOLTAGE), };

static const struct iio_chan_spec adc24_channels[] = {
ALIF_ADC_CHANNEL_DIFF(0, 4, 8),
ALIF_ADC_CHANNEL_DIFF(1, 5, 9),
ALIF_ADC_CHANNEL_DIFF(2, 6, 10),
ALIF_ADC_CHANNEL_DIFF(3, 7, 11), };

static inline void adc_disable_single_shot_conv(struct alif_adc_state *st)
{
	u32 data;

	data = readl(st->adc_base + ADC_START_SRC);
	data &= ~(ADC_START_ENABLE);
	writel(data, st->adc_base + ADC_START_SRC);

	data = readl(st->adc_base + ADC_CONTROL);
	data &= ~ADC_START_SINGLE_SHOT_CONV;
	writel(data, st->adc_base + ADC_CONTROL);
}

static inline void adc_sequencer_msk_ch_control(struct alif_adc_state *st,
						int channel) {
	u32 val;

	val = readl(st->adc_base + ADC_SEQUENCER_CTRL);
	val &= ~(ADC_MSK_ALL_CHANNELS);
	val |= ((~(1 << channel)) & ADC_MSK_ALL_CHANNELS);
	writel(val, st->adc_base + ADC_SEQUENCER_CTRL);
}

static inline void adc_enable_single_shot_conv(struct alif_adc_state *st)
{
	u32 val;

	val = readl(st->adc_base + ADC_START_SRC);
	val |= ADC_START_ENABLE;
	writel(val, st->adc_base + ADC_START_SRC);
	val = readl(st->adc_base + ADC_START_SRC);

	val = readl(st->adc_base + ADC_CONTROL);
	val |= ADC_START_SINGLE_SHOT_CONV;
	writel(val, st->adc_base + ADC_CONTROL);
	val = readl(st->adc_base + ADC_CONTROL);
}

static inline void adc_set_ch_scan_mode(struct alif_adc_state *st,
					int channel_scan_mode, int channel) {
	u32 val;

	val = readl(st->adc_base + ADC_SEQUENCER_CTRL);
	val = (channel_scan_mode << 0) | (channel << ADC_SEQUENCER_INIT_POS);
	writel(val, st->adc_base + ADC_SEQUENCER_CTRL);
	val = readl(st->adc_base + ADC_SEQUENCER_CTRL);
}

static inline void disable_adc(struct alif_adc_state *st)
{
	u32 data;

	data = readl(st->adc_base + ADC_START_SRC);
	data &= ~(ADC_START_ENABLE);
	writel(data, st->adc_base + ADC_START_SRC);

	data = readl(st->adc_base + ADC_CONTROL);
	data &= ~ADC_START_SINGLE_SHOT_CONV;
	writel(data, st->adc_base + ADC_CONTROL);
}

static inline void adc_set_diff_and_comp(struct alif_adc_state *st, u32 inst, u8 differential,
					 u8 comparator_en, u8 comparator_bias)
{
	u32 val_r, read_r;

	switch (inst) {
	case ADC_INSTANCE_ADC12_0:
		val_r = ((differential << ADC120_DIFFERENTIAL_EN_POS));
		val_r |= ((1 << ADC120_VCM_DIV_POS) | (comparator_en << ADC120_COMPARATOR_EN_POS)
			    | (comparator_bias << ADC120_COMPARATOR_BIAS_POS));
		break;

	case ADC_INSTANCE_ADC12_1:
		val_r = ((differential << ADC121_DIFFERENTIAL_EN_POS));
		val_r |= ((1 << ADC121_VCM_DIV_POS) | (comparator_en << ADC121_COMPARATOR_EN_POS)
			    | (comparator_bias << ADC121_COMPARATOR_BIAS_POS));
		break;

	case ADC_INSTANCE_ADC12_2:
		val_r = ((differential << ADC122_DIFFERENTIAL_EN_POS));
		val_r |= ((1 << ADC122_VCM_DIV_POS)
			    | (comparator_en << ADC122_COMPARATOR_EN_POS)
			    | (comparator_bias << ADC122_COMPARATOR_BIAS_POS));
		break;

	default:
		pr_err("ADC instances not found for %s\n", __func__);
	}

	read_r = readl(st->adc_reg1);
	val_r |= read_r;
	writel(val_r, st->adc_reg1);
	read_r = readl(st->adc_reg1);
	pr_debug("ADC instances ADC_12%d and value set  0x%x\n", inst, read_r);
}

static int get_temp(int *adc_value)
{
	u32 i;

	/* check for temperature operating range */
	if ((*adc_value < temp_data[0][0]) ||
	    (*adc_value > temp_data[MAX_TEMP_RANGE][0]))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(temp_data); i++) {
		/* check if value matches with tempdata */
		if (*adc_value == temp_data[i][0])
			break;
	}

	return (temp_data[i][1]);
}

static int alif_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			 int *val, int *val2, long mask)
{
	struct alif_adc_state *st = iio_priv(indio_dev);
	int ret, temp, buff;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		ret = clk_prepare_enable(st->clk);
		if (ret)
			return ret;

		adc_set_ch_scan_mode(st, ADC_SCAN_MODE_SINGLE_CH,
				     chan->channel);
		adc_sequencer_msk_ch_control(st, chan->channel);
		channel_sel = ((readl(st->adc_base + ADC_SEQUENCER_CTRL)
				& ADC_SEQUENCER_INIT_MASK) >> 12);
		/* check channel number passed is enabled or not */
		if (((readl(st->adc_base + ADC_SEQUENCER_CTRL))
			& (1 << channel_sel))) {
			pr_err(" Error: channel is masked\n");
			return -EINVAL;
		}
		adc_enable_single_shot_conv(st);
		ret = wait_for_completion_timeout(&st->completion,
						  msecs_to_jiffies(TIMEOUT_MS)); /* set by ISR */
		if (ret == 0) {
			clk_disable_unprepare(st->clk);
			return -ETIMEDOUT;
		}

		reinit_completion(&st->completion);
		clk_disable_unprepare(st->clk);
		spin_lock(&st->lock_s);

		switch (chan->type) {
		case IIO_VOLTAGE:
			if (st->instance == ADC_INSTANCE_ADC24_0)
				*val = (st->value & ADC_24BIT_READ);
			else
				*val = (st->value & ADC_16BIT_READ) >> 8;

			break;

		case IIO_TEMP:
			*val = (st->value & ADC_16BIT_READ) >> 8;
			break;

		default:
			spin_unlock(&st->lock_s);
			return -EINVAL;
		}
		spin_unlock(&st->lock_s);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
	/* we are using below mentioned formula to calculate voltage
	 * fractional value in volt:-
	 * Analog Voltage =  (ADC Sample Value(24bit or 12bit) * Vref) /
	 *		     (2^12 or 2^24  - 1)
	 *
	 * --> Reference Voltage (Vref):This is the maximum input voltage
	 * that the ADC can read.
	 * For example, if Vref is 1.76V, the analog input range is 0
	 * to 1.76V.
	 * --> 2^12 or 2^24: Powerof 2 it is decided based upon according
	 * to 12-bit or 24-bit ADC.
	 */
		switch (chan->type) {
		case IIO_VOLTAGE:
			if (st->instance == ADC_INSTANCE_ADC24_0) {
				*val = (st->value & ADC_24BIT_READ);
				*val = (*val * st->vref) / 100;
				*val2 = (ADC_24BIT_LSB_DIV - 1);
			} else {
				*val = (st->value & ADC_16BIT_READ) >> 8;
				*val = (*val * st->vref) / 100;
				*val2 = (ADC_12BIT_LSB_DIV - 1);
			}

			return IIO_VAL_FRACTIONAL;

		case IIO_TEMP:
			*val = (st->value & ADC_16BIT_READ) >> 8;
			buff = *val;
			temp = get_temp(&buff);
			pr_info("Temperature: %d.%d°C\n", temp / 10,
				abs(temp % 10));
			if (temp < 0)
				return (~temp + 1);
			else
				return temp;

		default:
			return -EINVAL;
		}
	}
	return -EINVAL;
}

static ssize_t adc_differential_var_store(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct alif_adc_state *st = iio_priv(dev_to_iio_dev(dev));
	int ret;

	ret = sscanf(buf, "%du", (int *)&st->differential);
	if (ret != 1)
		return -EINVAL;

	adc_set_diff_and_comp(st, st->instance, st->differential,
			      st->comparator_en, st->comparator_bias);

	pr_info("differential mode %s\n",
		st->differential ? "enabled" : "disabled");
	return count;
}

static ssize_t adc_differential_var_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct alif_adc_state *st = iio_priv(dev_to_iio_dev(dev));

	return sprintf(buf, "%d\n", st->differential);
}

static IIO_DEVICE_ATTR(differential_var, 0644,
			adc_differential_var_show,
			adc_differential_var_store, 0);

static struct attribute *alif_attributes[] = {
		&iio_dev_attr_differential_var.dev_attr.attr, NULL, };

static const struct attribute_group alif_attribute_group = { .attrs =
		alif_attributes, };

static const struct iio_info alif_adc_iio_info = { .read_raw =
		&alif_read_raw, .attrs = &alif_attribute_group, };

static inline void read_adc_data(struct alif_adc_state *st)
{
	u32 value;
	void __iomem *channel_sample_reg;
	void __iomem *sample_reg = (st->adc_base + ADC_SAMPLE_REG_0);
	u32 channel_num = readl(st->adc_base + ADC_SEL);

	channel_sample_reg = (sample_reg + sizeof(u32) * channel_num);
	st->value = (readl((u32 *)channel_sample_reg));
	value = readl(st->adc_base + ADC_CONTROL);
	value &= ADC_THRSHLD_CMP_MASK_BIT;
	value >>= ADC_SHIFT_BIT;

	switch (value) {
	case ADC_CMP_THRHLD_ABOVE_A:
		st->comparator = ADC_CONV_STAT_CMP_THLD_ABOVE_A;
		break;
	case ADC_CMP_THRHLD_BELOW_A:
		st->comparator = ADC_CONV_STAT_CMP_THLD_BELOW_A;
		break;
	case ADC_CMP_THRHLD_BETWEEN_A_B:
		st->comparator = ADC_CONV_STAT_CMP_THLD_BETWEEN_A_B;
		break;
	default:
		pr_err("ADC CMP Theshold instances not found\n");
		return;
	}
}

static irqreturn_t alif_adc_isr(int irq, void *dev_id)
{
	u32 status;
	struct alif_adc_state *st = dev_id;

	status = readl(st->adc_base + ADC_INTERRUPT);

	if ((status & ADC_INTERRUPT_FLAGS) == ((AVG_SAMPLE_RDY) |
		(ALL_SAMPLES_TAKEN) | (AVG_CALC_THRESH_0) |
		(AVG_CALC_THRESH_1))) {
		iowrite32(status, st->adc_base + ADC_INTERRUPT);
		read_adc_data(st);
	} else {
		return IRQ_NONE;
	}

	complete(&st->completion);
	disable_adc(st);

	return IRQ_HANDLED;
}

static inline void adc_unmask_interrupt(struct alif_adc_state *st)
{
	u32 val;

	val = readl(st->adc_base + ADC_INTERRUPT_MASK);
	val = (~(ADC_DONE0_INTERRUPT | ADC_DONE1_INTERRUPT) & ADC_INTERRUPT_ALL_MASK);
	writel(val, st->adc_base + ADC_INTERRUPT_MASK);
}

static inline void adc_set_comparator_ctrl_bit(struct alif_adc_state *st)
{
	u32 val;

	val = readl(st->adc_base + ADC_CONTROL);
	val = COMP_EN;
	writel(val, st->adc_base + ADC_CONTROL);
}

static inline void adc_set_comparator_b(struct alif_adc_state *st)
{
	u32 val;

	val = readl(st->adc_base + ADC_COMP_THRESH_B);
	val = COMP_THRESHOLD_B;
	writel(val, st->adc_base + ADC_COMP_THRESH_B);
}

static inline void adc_set_comparator_a(struct alif_adc_state *st)
{
	u32 val;

	val = readl(st->adc_base + ADC_COMP_THRESH_A);
	val = COMP_THRESHOLD_A;
	writel(val, st->adc_base + ADC_COMP_THRESH_A);
}

static inline void adc_set_n_shift_bit(struct alif_adc_state *st)
{
	/* set ADC shift bit for ADC12/24 */
	writel(st->shift_control_val, st->adc_base + ADC_SHIFT_CONTROL);
}

static inline void adc_set_clock_divsor(struct alif_adc_state *st)
{
	writel(st->clk_div, st->adc_base + ADC_CLK_DIVISOR);
}

static inline void adc_set_avg_sample(struct alif_adc_state *st)
{
	/* set adc avg. num 256 for ADC12/ADC24 */
	writel(st->avg_sample, st->adc_base + ADC_AVG_NUM);
}

static inline void adc_set_sample_width(struct alif_adc_state *st)
{
	/* set Sample width value 16 for ADC12 and 0 for ADC24 */
	u32 val;

	val = readl(st->adc_base + ADC_SAMPLE_WIDTH);
	val = (val & ~ADC12_SAMPLE_WIDTH_MSK) | st->width_sample |
				st->sample_hold;
	writel(val, st->adc_base + ADC_SAMPLE_WIDTH);
}

/* Vref setting */
static inline void adc_analog_config(struct alif_adc_state *st)
{
	u32 val = readl(st->cmp_base + CMP_COMP_REG2_OFFSET);

	val |= ANA_PERIPH_LDO_CONT | ANA_PERIPH_BG_CONT;
	writel(val, st->cmp_base + CMP_COMP_REG2_OFFSET);
}

static inline void enable_adc24(struct alif_adc_state *st)
{
	u32 data = 0;

	data = readl(st->adc_base + PMU_PERIPH_OFFSET);
	data |= PMU_PERIPH_ADC24_EN;
	writel(data, (st->adc_base + PMU_PERIPH_OFFSET));
}

static inline void set_adc24_bias(struct alif_adc_state *st, u32 bias)
{
	u32 data;

	data = readl(st->adc_base + PMU_PERIPH_OFFSET);
	data |= ((bias << PMU_PERIPH_ADC24_BIAS_POS) &
		PMU_PERIPH_ADC24_BIAS_MSK);
	writel(data, st->adc_base + PMU_PERIPH_OFFSET);
}

static inline void set_adc24_output_rate(struct alif_adc_state *st,
					 u32 rate)
{
	u32 data;

	data = readl(st->adc_base + PMU_PERIPH_OFFSET);
	data |= ((rate << PMU_PERIPH_ADC24_OUTPUT_RATE_POS)
			& PMU_PERIPH_ADC24_OUTPUT_RATE_MSK);
	writel(data, st->adc_base + PMU_PERIPH_OFFSET);
}

static int alif_adc_probe(struct platform_device *pdev)
{
	struct alif_adc_state *st = NULL;
	struct resource *res;
	int retval = -ENODEV;
	struct iio_dev *iodev = NULL;
	int irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get platform I/O memory\n");
		return -ENXIO;
	}

	iodev = devm_iio_device_alloc(&pdev->dev, sizeof(*st));
	if (!iodev)
		return -ENOMEM;

	st = iio_priv(iodev);

	st->adc_base = devm_platform_ioremap_resource_byname(pdev, "adc_base");
	if (IS_ERR(st->adc_base))
		return PTR_ERR(st->adc_base);

	st->adc_reg1 = st->adc_base + ADC_REG1;

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -ENXIO;

	platform_set_drvdata(pdev, iodev);

	init_completion(&st->completion);

	spin_lock_init(&st->lock_s);

	st->clk = devm_clk_get(&pdev->dev, "adc_clk");
	if (IS_ERR(st->clk)) {
		dev_err(&pdev->dev, "error getting clock\n");
		return PTR_ERR(st->clk);
	}

	retval = clk_prepare_enable(st->clk);
	if (retval) {
		dev_err(&pdev->dev, "unable to enable clock\n");
		return retval;
	}

	st->differential = 0;
	iodev->name = dev_name(&pdev->dev);
	iodev->dev.parent = &pdev->dev;
	iodev->info = &alif_adc_iio_info;
	iodev->modes = INDIO_DIRECT_MODE;
	iodev->channels = alif_adc_iio_channels;
	iodev->num_channels = ARRAY_SIZE(alif_adc_iio_channels);
	st->name = iodev->name;
	st->vref = ADC_VREF_CENTIVOLTS;
	st->clk_div = alif_adc_clk_divisor(2);
	st->avg_sample = alif_adc_avg_num(256);
	st->sample_hold = 0;
	st->width_sample = alif_adc_sample_width(16);
	st->shift_control_val = (SHIFT_DIR_LEFT) | (SHIFT_CONTROL);

	/* set the clock divisor */
	adc_set_avg_sample(st);

	/* set clock divisor for ADC121 */
	adc_set_clock_divsor(st);

	/* set comparator_a threshold */
	adc_set_comparator_a(st);

	/* set comparator_b threshold */
	adc_set_comparator_b(st);

	/* set comparator ctrl bit for ADC120 */
	adc_set_comparator_ctrl_bit(st);

	/* disabling the ADC_INTERRUPT_MASK ADC12/ADC24 */
	adc_unmask_interrupt(st);

	/*
	 * Identify ADC instance using hardware base address.
	 * This is more robust than string matching as it:
	 * - Does not depend on device tree naming conventions
	 * - Hardware addresses are always unique and available
	 */
	switch (res->start) {
	case ADC120_BASE:
		st->instance = ADC_INSTANCE_ADC12_0;
		st->comparator_en = 1;
		st->comparator_bias = COMPARATOR_BIAS_2_5;
		break;

	case ADC121_BASE:
		st->instance = ADC_INSTANCE_ADC12_1;
		st->comparator_en = 1;
		st->comparator_bias = COMPARATOR_BIAS_2_5;
		break;

	case ADC122_BASE:
		st->instance = ADC_INSTANCE_ADC12_2;
		st->comparator_en = 1;
		st->comparator_bias = COMPARATOR_BIAS_2_5;
		break;

	case ADC24_BASE:
		st->comparator_en = 1;
		st->adc24_bias = ADC24_BIAS_8_75;
		st->adc24_output_rate = ADC_OUTPUT_RATE_1K;
		/* enable ADC24 from control register */
		enable_adc24(st);
		/* set output rate from control register */
		set_adc24_output_rate(st, st->adc24_output_rate);
		/* Set ADC24 bias from control register */
		set_adc24_bias(st, st->adc24_bias);
		iodev->channels = adc24_channels;
		iodev->num_channels = ARRAY_SIZE(adc24_channels);
		st->instance = ADC_INSTANCE_ADC24_0;
		st->width_sample = alif_adc_sample_width(0);
		st->sample_hold = SAMPLE_HOLD;
		st->shift_control_val = ADC24_SHIFT_CONTROL_SETTING;
		break;

	default:
		dev_err(&pdev->dev, "Unknown ADC instance at 0x%08llx\n",
			(unsigned long long)res->start);
		clk_disable_unprepare(st->clk);
		return -EINVAL;
	}

	if (st->instance == ADC_INSTANCE_ADC12_0 || st->instance == ADC_INSTANCE_ADC24_0) {
		st->cmp_base = devm_platform_ioremap_resource_byname(pdev, "cmp_base");
		if (IS_ERR(st->cmp_base)) {
			dev_err(&pdev->dev, "Failed to map CMP_BASE registers\n");
			return PTR_ERR(st->cmp_base);
		}
		/* Vref Setting */
		adc_analog_config(st);
	}

	/* Set sample width value ADC12/24 */
	adc_set_sample_width(st);

	/* Set ADC set and shift bit ADC12/24 */
	adc_set_n_shift_bit(st);

	/* set differential control for ADC12 */
	if (st->instance != ADC_INSTANCE_ADC24_0) {
		adc_set_diff_and_comp(st, st->instance, st->differential,
				      st->comparator_en, st->comparator_bias);
	}

	retval = devm_request_irq(&pdev->dev, irq, alif_adc_isr, 0,
				  ALIF_NAME, st);

	if (retval < 0) {
		dev_err(&pdev->dev, "failed requesting interrupt\n");
		return retval;
	}

	retval = devm_iio_device_register(&pdev->dev, iodev);
	if (retval)
		return retval;

	dev_info(&pdev->dev, "ALIF ADC%d driver loaded successfully\n",
		 st->instance == ADC_INSTANCE_ADC24_0 ? 24 : 12);
	return 0;
}

static void alif_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct alif_adc_state *adc_priv = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	adc_disable_single_shot_conv(adc_priv);
	clk_disable_unprepare(adc_priv->clk);
}

#ifdef CONFIG_OF
static const struct of_device_id alif_adc_match[] = {
		{ .compatible = "alif,adc-dev" },
		{},
};
MODULE_DEVICE_TABLE(of, alif_adc_match);
#endif

static struct platform_driver alif_adc_driver = {
		.probe  = alif_adc_probe,
		.remove_new = alif_adc_remove,
		.driver = {
			.name = ALIF_NAME,
			.of_match_table = alif_adc_match,
		},
};

module_platform_driver(alif_adc_driver);

MODULE_AUTHOR("Pankaj Pandey <pankaj.pandey@alifsemi.com>");
MODULE_DESCRIPTION("ALIF ADC12/ADC24 driver");
MODULE_LICENSE("GPL");
