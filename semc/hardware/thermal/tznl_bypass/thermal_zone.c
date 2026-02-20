// SPDX-License-Identifier: GPL-2.0-only
/*
 * Netlink routines for TZNL_BYPASS
 *
 * Copyright 2024, 2025 Sony Corporation
/+ */

#include <linux/list.h>
#include <net/genetlink.h>

#include "thermal_zone.h"
#include "tznl_bypass.h"

LIST_HEAD(tznl_list);
DEFINE_SPINLOCK(tznl_list_slock);

static int tznl_get_temp(struct thermal_zone_device *thermal, int *temp)
{
	struct tznl_info *tznl_info = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry(tznl_info, &tznl_list, node) {
		if (tznl_info->tz == thermal) {
			spin_unlock_irqrestore(&tznl_list_slock, flags);
			goto found;
		}
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	tznl_bypass_err("Not found %s\n", thermal->type);
	return -ENODEV;

found:
	if (tznl_get_temperature(tznl_info) < 0)
		return -EIO;

	if (wait_event_timeout(tznl_info->waitq, tznl_info->cond, WAIT_NETLINK_TIMEOUT) == 0)
		return -ETIMEDOUT;

	if (abs(tznl_info->temp) >= -ABSOLUTE_ZERO_TEMP)
		return -ENODATA;

	*temp = tznl_info->temp;
	return 0;
}

static int tznl_set_emul_temp(struct thermal_zone_device *thermal, int temp)
{
	struct tznl_info *tznl_info = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry(tznl_info, &tznl_list, node) {
		if (tznl_info->tz == thermal) {
			spin_unlock_irqrestore(&tznl_list_slock, flags);
			goto found;
		}
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	tznl_bypass_err("Not found %s\n", thermal->type);
	return -ENODEV;

found:
	if (tznl_set_emul_temperature(tznl_info, temp) < 0)
		return -EIO;

	tznl_bypass_info("Set emul_temp %d in thermal_zone%d(%s)\n",
			temp, tznl_info->tz->id, tznl_info->tz->type);
	return 0;
}

static struct thermal_zone_device_ops tznl_ops = {
	.get_temp = tznl_get_temp,
	.set_emul_temp = tznl_set_emul_temp
};

int register_tznl_sensor(char *name)
{
	struct tznl_info *tznl_info;
	unsigned long flags;

	tznl_info = kzalloc(sizeof(struct tznl_info), GFP_KERNEL);
	if (!tznl_info) {
		tznl_bypass_err("Failed to allocate memory\n");
		return -ENOMEM;
	}

	init_waitqueue_head(&tznl_info->waitq);
	tznl_info->cond = 1;
	tznl_info->tz = thermal_tripless_zone_device_register(name, tznl_info, &tznl_ops, NULL);
	if (IS_ERR(tznl_info->tz)) {
		kfree(tznl_info);
		tznl_bypass_err("Failed to register thermal_zone: %s\n", name);
		return -EINVAL;
	}

	tznl_bypass_info("Create thermal_zone%d(%s)\n", tznl_info->tz->id, name);
	spin_lock_irqsave(&tznl_list_slock, flags);
	list_add(&tznl_info->node, &tznl_list);
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	return tznl_info->tz->id;
}

void unregister_tznl_sensor(struct tznl_info *tznl_info)
{
	thermal_zone_device_unregister(tznl_info->tz);
	tznl_bypass_info("Delete thermal_zone%d(%s)\n", tznl_info->tz->id, tznl_info->tz->type);
	list_del(&tznl_info->node);
	kfree(tznl_info);
	return;
}
