// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024, 2025 Sony Corporation
 */
#ifndef _TZNL_BYPASS_H_
#define _TZNL_BYPASS_H_

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/thermal.h>

#define tznl_bypass_info(fmt, ...) \
	pr_info("tznl_bypass: " fmt, ##__VA_ARGS__)
#define tznl_bypass_err(fmt, ...) \
	pr_err("tznl_bypass: %s: " fmt, __func__, ##__VA_ARGS__)

struct tznl_info {
	struct thermal_zone_device *tz;
	int temp;
	wait_queue_head_t waitq;
	int cond;
	struct list_head node;
};

extern int tznl_genl_init(void);
extern void tznl_genl_exit(void);
extern int tznl_get_temperature(struct tznl_info *);
extern int tznl_set_emul_temperature(struct tznl_info *, int);
extern int register_tznl_sensor(char *);
extern void unregister_tznl_sensor(struct tznl_info *);

extern struct list_head tznl_list;
extern spinlock_t tznl_list_slock;

#endif /* _TZNL_BYPASS_H_ */
