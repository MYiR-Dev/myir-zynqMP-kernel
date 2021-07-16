#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <asm-generic/gpio.h>

#define DEV_NAME		"myir-watchdog"
#define RESET_MS		400 /* Default reset period set to 400 ms */
#define DEFAULT_WDI		376// PS_MIO0

struct watchdog_data {
	struct class class;
	struct timer_list timer;
	int period;

	int gpio;
	unsigned char gpio_value;
	unsigned char running;
};
	
static struct watchdog_data * gpdata;

static void reset_watchdog(struct timer_list *t)
{
	struct watchdog_data * pdata = from_timer(pdata, t, timer);
	
	pdata->gpio_value ^= 0x1;
	gpio_direction_output(pdata->gpio, pdata->gpio_value);
	mod_timer(&pdata->timer, jiffies + msecs_to_jiffies(pdata->period));
//	printk(KERN_ALERT "- reset wd.\n");
}

/* Initialize hrtimer */
static void initialize_timer(struct watchdog_data * pdata)
{
	if(!pdata) {
		printk(KERN_ERR "Watchdog device has not been initialized yet.\n");
		return;
	}

	if (pdata->period <= 0) {
		pdata->period = RESET_MS;
	}

	timer_setup( &pdata->timer, reset_watchdog, 0 );

}

/* Destroy hrtimer */
static void destroy_timer(struct watchdog_data * pdata)
{
	printk( KERN_ALERT "Watchdog timer destroy\n");
	if(pdata && pdata->running) {
		del_timer(&pdata->timer);
	}
}

/* class attribute show function. */
static ssize_t watchdog_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	struct watchdog_data *pdata = (struct watchdog_data *)container_of(cls, struct watchdog_data, class);
	return sprintf(buf, "%d\n", pdata->running?pdata->period:0);
}

/* class attribute store function. */
static ssize_t watchdog_store(struct class *cls, struct class_attribute *attr, const char *buf, size_t count)
{
	struct watchdog_data *pdata = (struct watchdog_data *)container_of(cls, struct watchdog_data, class);
	int tmp;
	
	if(!buf || sscanf(buf, "%d", &tmp) <= 0) {
		return -EINVAL;
	}
	
	if(tmp == 0 && pdata->running) { /* Stop the watchdog timer */
		del_timer(&pdata->timer);
		pdata->running = 0;
		
		/* Set gpio to input(High-Z state) to disable external watchdog timer */
		gpio_direction_input(pdata->gpio);
		
		printk("Cancel watchdog timer!\n");
	} else if(tmp > 0) {
		printk(KERN_ALERT "Set period to %d ms .\n", tmp);
		pdata->period = tmp;
		if(!pdata->running) {
			printk(KERN_ALERT "Start WD timer.\n");
			mod_timer(&pdata->timer, jiffies + msecs_to_jiffies(pdata->period));
			pdata->running = 1;
		}
	} else {
		return -EINVAL;
	}
	
	return count;
}
struct class_attribute class_attr_watchdog = __ATTR(wd_period_ms, S_IRUGO | S_IWUSR , watchdog_show, watchdog_store);

static ssize_t feed_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "write '1' to enable manual-mode and disable auto-mode.\n");
}

static ssize_t feed_store(struct class *cls, struct class_attribute *attr, const char *buf, size_t count)
{
	struct watchdog_data *pdata = (struct watchdog_data *)container_of(cls, struct watchdog_data, class);
	int tmp;
	if(!buf || sscanf(buf, "%d", &tmp) <= 0) {
		return -EINVAL;
	}
	
	if(tmp == 0) {
		if(pdata->running == 0) {
			printk(KERN_ALERT "Cancel watchdog.\n");
			/* Set gpio to input(High-Z state) to disable external watchdog timer */
			gpio_direction_input(pdata->gpio);
		} else {
			printk(KERN_ALERT "Can not cancel watchdog by writing 'wd_feed' while running in auto-mode.\n");
		}
	} else if(tmp > 0) {
		if(pdata->running) {
			printk(KERN_ALERT "Disable auto-mode and switch to manual-mode.\n");
			del_timer(&pdata->timer);
			pdata->running = 0;
		}
		pdata->gpio_value ^= 0x1;
		gpio_direction_output(pdata->gpio, pdata->gpio_value);
	} else {
		return -EINVAL;
	}
	return count;
}
struct class_attribute class_attr_feed = __ATTR(wd_feed, S_IRUGO | S_IWUSR , feed_show, feed_store);

/* Attributes declaration: Here I have declared only one attribute attr1 */
static struct attribute *watchdog_class_attrs[] = {
	&class_attr_watchdog.attr,
	&class_attr_feed.attr,
	NULL,
};
ATTRIBUTE_GROUPS(watchdog_class);

//Module initialization.
static int watchdog_probe(struct platform_device *pdev)
{
	int ret = 0;

	gpdata = kmalloc(sizeof(struct watchdog_data), GFP_KERNEL);
	if(!gpdata) {
		printk(KERN_ERR "No memory!\n");
		return -ENOMEM;
	}
	memset(gpdata, 0, sizeof(struct watchdog_data));

	/* Init gpio */	
	gpdata->gpio = DEFAULT_WDI;

	ret = gpio_request(DEFAULT_WDI, DEV_NAME);
	if(ret < 0) {
		printk(KERN_ERR "request gpio %d for %s failed!\n", DEFAULT_WDI, DEV_NAME);
		goto gpio_request_fail;
	}

	/* init wdt feed interval */
	gpdata->period = RESET_MS; /* Init period */
	
	initialize_timer(gpdata);

	/* Init class */
	gpdata->class.name = DEV_NAME;
	gpdata->class.owner = THIS_MODULE;
	gpdata->class.class_groups = watchdog_class_groups;
	ret = class_register(&gpdata->class);
	if(ret) {
		printk(KERN_ERR "class_register failed!\n");
		goto class_register_fail;
	}
	
	/* Start watchdog timer here */
	gpio_direction_output(gpdata->gpio, gpdata->gpio_value);
	mod_timer(&gpdata->timer, jiffies + msecs_to_jiffies(gpdata->period));
	gpdata->running = 1;
	
	printk(KERN_ALERT "%s driver initialized successfully!\n", DEV_NAME);
	return 0;

class_register_fail:
	destroy_timer(gpdata);
	gpio_free(gpdata->gpio);
	
gpio_request_fail:
	kfree(gpdata);
	
	return ret;
}

static int watchdog_remove(struct platform_device *pdev)
{
	struct watchdog_data * dev = gpdata;
	if(dev) {
		destroy_timer(dev);
		gpio_free(dev->gpio);
		class_unregister(&dev->class);
		kfree(dev);
	}

	return 0;
}

static struct of_device_id wdt_of_match[] = {
        { .compatible = "gpio-watchdog", },
        { /* end of table */}
};
MODULE_DEVICE_TABLE(of, wdt_of_match);


static struct platform_driver watchdog_platfrom_driver = {
        .probe   = watchdog_probe,
        .remove  = watchdog_remove,
        .driver = {
                .name = DEV_NAME,
                .owner = THIS_MODULE,
                .of_match_table = wdt_of_match,
        },
};

static int __init watchdog_init(void)
{
	return platform_driver_register(&watchdog_platfrom_driver);
}

static void __exit watchdog_exit(void)
{
	platform_driver_unregister(&watchdog_platfrom_driver);
}

module_init(watchdog_init);
module_exit(watchdog_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("<myirtech.com>");
MODULE_DESCRIPTION("MYIR Watch Dog Driver.");
