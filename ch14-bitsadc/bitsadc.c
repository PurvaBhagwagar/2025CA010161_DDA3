// SPDX-License-Identifier: GPL-2.0
/*
 * bitsadc.c - Emulated IIO ADC with LED threshold indicator
 *
 * Author: Purva Bhagwagar (2025CA01061)
 *
 * A platform_driver + platform_device pair (name-matched, no device
 * tree needed) that registers an IIO device with two voltage
 * channels. Channel 0 is animated by a periodic hrtimer as a
 * triangle wave over the 12-bit range [0, 4095]; channel 1 is a
 * fixed mid-scale reading. Whenever channel 0 crosses a module
 * parameter threshold, an LED classdev mirrors the comparison.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/leds.h>
#include <linux/err.h>

#define BITSADC_NAME     "bitsadc"
#define ADC_MAX          4095
#define ADC_MID          (ADC_MAX / 2)
#define TIMER_PERIOD_MS  100
#define TRIANGLE_STEP    64   /* per 100ms tick -> full sweep in ~6.4s */

static int threshold = 2048;
module_param(threshold, int, 0444);
MODULE_PARM_DESC(threshold, "LED threshold for channel 0, 0-4095 (default 2048)");

struct bitsadc_priv {
	struct hrtimer     timer;
	struct led_classdev led;
	int                 sample;     /* ch0 value; READ_ONCE/WRITE_ONCE only */
	int                 direction;  /* +1 or -1; only touched by the timer */
};

static int bitsadc_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct bitsadc_priv *priv = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->channel == 0)
			*val = READ_ONCE(priv->sample);
		else
			*val = ADC_MID;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* Emulated 3.3V reference over a 12-bit code, expressed
		 * as millivolts-per-LSB via IIO_VAL_FRACTIONAL_LOG2:
		 * scale = val / 2^val2 = 3300 / 4096.
		 */
		*val = 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info bitsadc_info = {
	.read_raw = bitsadc_read_raw,
};

#define BITSADC_VOLTAGE_CHAN(idx) {				\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = (idx),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			       BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec bitsadc_channels[] = {
	BITSADC_VOLTAGE_CHAN(0),
	BITSADC_VOLTAGE_CHAN(1),
};

/*
 * hrtimer callback: runs in HARDIRQ/atomic context (this is a
 * CLOCK_MONOTONIC hrtimer fired from interrupt context, not a
 * process context worker), so it must never sleep, take a mutex,
 * call anything that can block, or touch vmalloc'd/user memory.
 * Everything it does here is bounded, non-blocking arithmetic plus
 * an LED brightness write, which is safe from atomic context
 * (led_set_brightness dispatches to the LED's brightness_set
 * callback, which for classdevs without their own hardware access
 * requirements is expected to be atomic-safe; here there is no real
 * hardware, so it's just a variable write).
 *
 * No mutex/spinlock guards `priv->sample`: the hrtimer callback is
 * the *only* writer (single-threaded, serialized by hrtimer itself -
 * it cannot re-enter concurrently with itself on the same timer),
 * and sysfs reads via read_raw() are the only other accessor and
 * only ever read it. A single aligned int read/write is atomic on
 * every architecture Linux supports; the only risk is the compiler
 * tearing the access into multiple loads/stores or caching a stale
 * value in a register across calls, which WRITE_ONCE()/READ_ONCE()
 * prevent by forcing a single, non-reordered memory access each
 * time. That is sufficient here because we don't need read-modify-
 * write atomicity across the two accessors — just that each side
 * sees a whole, freshly-read value.
 */
static enum hrtimer_restart bitsadc_timer_cb(struct hrtimer *timer)
{
	struct bitsadc_priv *priv = container_of(timer, struct bitsadc_priv, timer);
	int sample = READ_ONCE(priv->sample);
	int dir = priv->direction;

	sample += dir * TRIANGLE_STEP;
	if (sample >= ADC_MAX) {
		sample = ADC_MAX;
		dir = -1;
	} else if (sample <= 0) {
		sample = 0;
		dir = 1;
	}

	WRITE_ONCE(priv->sample, sample);
	priv->direction = dir;

	led_set_brightness(&priv->led, sample >= threshold ? 1 : 0);

	hrtimer_forward_now(timer, ms_to_ktime(TIMER_PERIOD_MS));
	return HRTIMER_RESTART;
}

static int bitsadc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct bitsadc_priv *priv;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->sample = 0;
	priv->direction = 1;

	indio_dev->name = BITSADC_NAME;
	indio_dev->info = &bitsadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = bitsadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(bitsadc_channels);

	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret)
		return ret;

	priv->led.name = "bits:status:adc";
	priv->led.max_brightness = 1;
	priv->led.brightness = 0;

	ret = devm_led_classdev_register(&pdev->dev, &priv->led);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	hrtimer_init(&priv->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	priv->timer.function = bitsadc_timer_cb;
	hrtimer_start(&priv->timer, ms_to_ktime(TIMER_PERIOD_MS), HRTIMER_MODE_REL);

	dev_info(&pdev->dev, "bitsadc probed, threshold=%d\n", threshold);
	return 0;
}

/*
 * remove()'s return type changed from int to void in Linux 6.11.
 * The macro keeps this file buildable on both sides of that change
 * without edits.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define BITSADC_REMOVE_RET void
#define BITSADC_REMOVE_RETURN return
#else
#define BITSADC_REMOVE_RET int
#define BITSADC_REMOVE_RETURN return 0
#endif

static BITSADC_REMOVE_RET bitsadc_remove(struct platform_device *pdev)
{
	struct bitsadc_priv *priv = platform_get_drvdata(pdev);

	/* Must cancel before devm teardown of the IIO/LED devices runs,
	 * otherwise the timer could fire and touch freed memory
	 * (led_classdev / iio_dev) after they're gone -> oops on unload.
	 */
	hrtimer_cancel(&priv->timer);

	dev_info(&pdev->dev, "bitsadc removed, timer cancelled\n");
	BITSADC_REMOVE_RETURN;
}

static struct platform_driver bitsadc_driver = {
	.driver = {
		.name = BITSADC_NAME,
	},
	.probe  = bitsadc_probe,
	.remove = bitsadc_remove,
};

static struct platform_device *bitsadc_pdev;

static int __init bitsadc_init(void)
{
	int ret;

	ret = platform_driver_register(&bitsadc_driver);
	if (ret)
		return ret;

	bitsadc_pdev = platform_device_register_simple(BITSADC_NAME, -1, NULL, 0);
	if (IS_ERR(bitsadc_pdev)) {
		ret = PTR_ERR(bitsadc_pdev);
		platform_driver_unregister(&bitsadc_driver);
		return ret;
	}

	return 0;
}

static void __exit bitsadc_exit(void)
{
	platform_device_unregister(bitsadc_pdev);
	platform_driver_unregister(&bitsadc_driver);
}

module_init(bitsadc_init);
module_exit(bitsadc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Purva Bhagwagar <2025CA01061>");
MODULE_DESCRIPTION("Emulated IIO ADC with LED threshold indicator (bitsadc)");
