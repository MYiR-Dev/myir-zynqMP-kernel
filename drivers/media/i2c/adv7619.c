/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <asm-generic/gpio.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include "adv7619.h"

#define CS_PIN		419
#define RESET		420
#define CS_NAME		"adv7619-cs"
#define RES_NAME	"adv7619-reset"

enum adv7619_page {
	ADV7619_PAGE_IO,
	ADV7619_PAGE_CEC,
	ADV7619_PAGE_INFOFRAME,
	ADV7619_PAGE_DPLL,
	ADV7619_PAGE_KSV,
	ADV7619_PAGE_EDID,
	ADV7619_PAGE_HDMI,
	ADV7619_PAGE_CP,
	ADV7619_PAGE_MAX,
};

struct adv7619_state {
	struct i2c_client *i2c_clients[ADV7619_PAGE_MAX];
	struct regmap *regmap[ADV7619_PAGE_MAX];
	u8 i2c_addresses[ADV7619_PAGE_MAX];
};

static inline int io_read(struct adv7619_state *state, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_IO];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[ADV7619_PAGE_IO], reg, &val);

	if(err) {
		printk("error reading %02x, %02x\n", client->addr, reg);
		return err;
	}

	return val;
}

static inline int io_write(struct adv7619_state *state, u8 reg, u8 val)
{
	return regmap_write(state->regmap[ADV7619_PAGE_IO], reg, val);
}

static inline int ksv_read(struct adv7619_state *state, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_KSV];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[ADV7619_PAGE_KSV], reg, &val);

	if(err) {
		printk("error reading %02x, %02x\n", client->addr, reg);
		return err;
	}

	return val;
}

static inline int ksv_write(struct adv7619_state *state, u8 reg, u8 val)
{
	return regmap_write(state->regmap[ADV7619_PAGE_KSV], reg, val);
}

static inline int edid_read(struct adv7619_state *state, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_EDID];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[ADV7619_PAGE_EDID], reg, &val);

	if(err) {
		printk("error reading %02x, %02x\n", client->addr, reg);
		return err;
	}

	return val;
}

static inline int edid_write(struct adv7619_state *state, u8 reg, u8 val)
{
	return regmap_write(state->regmap[ADV7619_PAGE_EDID], reg, val);
}

static inline int hdmi_read(struct adv7619_state *state, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_HDMI];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[ADV7619_PAGE_HDMI], reg, &val);

	if(err) {
		printk("error reading %02x, %02x\n", client->addr, reg);
		return err;
	}

	return val;
}

static inline int hdmi_write(struct adv7619_state *state, u8 reg, u8 val)
{
	return regmap_write(state->regmap[ADV7619_PAGE_HDMI], reg, val);
}

static inline int dpll_read(struct adv7619_state *state, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_DPLL];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[ADV7619_PAGE_DPLL], reg, &val);

	if(err) {
		printk("error reading %02x, %02x\n", client->addr, reg);
		return err;
	}

	return val;
}

static inline int dpll_write(struct adv7619_state *state, u8 reg, u8 val)
{
	return regmap_write(state->regmap[ADV7619_PAGE_DPLL], reg, val);
}

static const struct regmap_config adv7619_regmap_cnf[] = {
	{
		.name			= "io",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "cec",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "infoframe",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "ksv",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "dpll",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "edid",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "hdmi",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
	{
		.name			= "cp",
		.reg_bits		= 8,
		.val_bits		= 8,
		
		.max_register		= 0xff,
		.cache_type		= REGCACHE_NONE,
	},
};

static int configure_regmap(struct adv7619_state *state, int region)
{
	if(!state->i2c_clients[region])
		return -ENODEV;

	state->regmap[region] = devm_regmap_init_i2c(state->i2c_clients[region], &adv7619_regmap_cnf[region]);

	if(IS_ERR(state->regmap[region])) {
		printk("Error initializing regmap %d\n", region);
		return -EINVAL;
	}

	return 0;
}

static int configure_regmaps(struct adv7619_state *state)
{
	int i, err;

	for(i = ADV7619_PAGE_CEC; i < ADV7619_PAGE_MAX; i++) {
		err = configure_regmap(state, i);

		if(err && (err != -ENODEV))
			return err;
	}

	return 0;
}

static struct i2c_client *adv7619_dummy_client(struct adv7619_state *state, u8 addr, u8 io_reg)
{
	struct i2c_client *client = state->i2c_clients[ADV7619_PAGE_IO];

	if(addr) {
		io_write(state, io_reg, addr << 1);
	}

	return i2c_new_dummy(client->adapter, io_read(state, io_reg) >> 1);			
}

static int adv7619_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct adv7619_state *state;
	int i;
	int err;

	gpio_request(CS_PIN, CS_NAME);
	gpio_request(RESET, RES_NAME);

	gpio_direction_output(CS_PIN, 0);
	gpio_direction_output(RESET, 0);
	msleep(10);
	gpio_direction_output(RESET, 1);
	msleep(10);	  

	state = devm_kzalloc(&client->dev, sizeof(*state), GFP_KERNEL);
	if(!state) {
		printk("Could not allocate adv7619_state memory!\n");
		return -ENOMEM;
	}

	state->i2c_clients[ADV7619_PAGE_IO] = client;

	state->i2c_addresses[ADV7619_PAGE_CEC] = 0x40;
	state->i2c_addresses[ADV7619_PAGE_INFOFRAME] = 0x3e;
	state->i2c_addresses[ADV7619_PAGE_DPLL] = 0x26;
	state->i2c_addresses[ADV7619_PAGE_KSV] = 0x32;
	state->i2c_addresses[ADV7619_PAGE_EDID] = 0x36;
	state->i2c_addresses[ADV7619_PAGE_HDMI] = 0x34;
	state->i2c_addresses[ADV7619_PAGE_CP] = 0x22;

	err = configure_regmap(state, ADV7619_PAGE_IO);
	if(err) {
		printk("Error configuring IO regmap region\n");
		return -ENODEV;
	}
	
	state->i2c_clients[1] = adv7619_dummy_client(state, state->i2c_addresses[1], 0xf4);
	state->i2c_clients[2] = adv7619_dummy_client(state, state->i2c_addresses[2], 0xf5);
	state->i2c_clients[3] = adv7619_dummy_client(state, state->i2c_addresses[3], 0xf8);
	state->i2c_clients[4] = adv7619_dummy_client(state, state->i2c_addresses[4], 0xf9);
	state->i2c_clients[5] = adv7619_dummy_client(state, state->i2c_addresses[5], 0xfa);
	state->i2c_clients[6] = adv7619_dummy_client(state, state->i2c_addresses[6], 0xfb);
	state->i2c_clients[7] = adv7619_dummy_client(state, state->i2c_addresses[7], 0xfd);

	err = configure_regmaps(state);
	if(err) {
		printk("error configure regmaps\n");
		return -ENODEV;
	}

	i = 0;
	while (edid_data_2k[i].dev != 0xff) {
		switch(edid_data_2k[i].dev) {
		case 0x98:
			io_write(state, edid_data_2k[i].reg, edid_data_2k[i].val);
			break;
		case 0x64:
			ksv_write(state, edid_data_2k[i].reg, edid_data_2k[i].val);
			break;
		case 0x6c:
			edid_write(state, edid_data_2k[i].reg, edid_data_2k[i].val);
			break;
		default:
			printk("!error edid write\n");
			break;
		};

		i++;
		msleep(10);
	}
	printk("!EDID done\n");

	i = 0;
	while (adv7619_register_data[i].dev != 0xff) {
		switch(adv7619_register_data[i].dev) {
		case 0x98:
			io_write(state, adv7619_register_data[i].reg, adv7619_register_data[i].val);
			break;
		case 0x68:
			hdmi_write(state, adv7619_register_data[i].reg, adv7619_register_data[i].val);
			break;
		case 0x4c:
			dpll_write(state, adv7619_register_data[i].reg, adv7619_register_data[i].val);
			break;
		default:
			printk("!error hdmi write\n");
			break;
		};

		i++;
		msleep(10);
	}
	printk("HDMI initialization done\n");
	msleep(50);

	gpio_direction_output(CS_PIN, 1);	
	gpio_free(CS_PIN);
	gpio_free(RESET);

	return 0;	
}

static const struct i2c_device_id adv7619_idtable[] = {
	{ "adv7619", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, adv7619_idtable);

#ifdef CONFIG_OF
static const struct of_device_id adv7619_of_match[] = {
	{ .compatible = "hdmi,adv7619" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, adv7619_of_match);
#endif

static struct i2c_driver adv7619_driver = {
	.driver = {
		.name	= "adv7619",
		.of_match_table = of_match_ptr(adv7619_of_match),
	},
	.id_table	= adv7619_idtable,
	.probe		= adv7619_probe,
};

module_i2c_driver(adv7619_driver);

MODULE_AUTHOR("gpl");
MODULE_DESCRIPTION("adv7619 Driver");
MODULE_LICENSE("GPL");
