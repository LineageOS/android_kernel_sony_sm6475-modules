// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024, 2025 Sony Corporation
 */

#include "tznl_bypass.h"

static int __init tznl_bypass_init(void)
{
	int ret;

	if ((ret = tznl_genl_init()) < 0)
		tznl_bypass_err("failed to register netlink family\n");
	else
		tznl_bypass_info("init done\n");

	return ret;
}

static void __exit tznl_bypass_exit(void)
{
	struct tznl_info *tznl_info = NULL;
	struct tznl_info *next_tznl_info = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry_safe(tznl_info, next_tznl_info, &tznl_list, node)
		unregister_tznl_sensor(tznl_info);

	spin_unlock_irqrestore(&tznl_list_slock, flags);
	tznl_genl_exit();
	tznl_bypass_info("exit done\n");
	return;
}

module_init(tznl_bypass_init);
module_exit(tznl_bypass_exit);

MODULE_DESCRIPTION("thermal_zone netlink bypass driver");
MODULE_LICENSE("GPL v2");
