// SPDX-License-Identifier: GPL-2.0-only
/*
 * Netlink routines for TZNL_BYPASS
 *
 * Copyright 2024 Sony Corporation
 */

#ifndef _NETLINK_H
#define _NETLINK_H

#define TZNL_GENL_VERSION	(1)
#define TZNL_GENL_FAMILY_NAME	"tznl_bypass"
#define TZNL_GENL_GROUP_NAME	"tznl_bypass"

enum {
	TZNL_GENL_ATTR_UNSPEC,
	TZNL_GENL_ATTR_TZ_ID,
	TZNL_GENL_ATTR_SENSOR_NAME,
	TZNL_GENL_ATTR_TEMPERATURE,
	__TZNL_GENL_ATTR_MAX,
};
#define TZNL_GENL_ATTR_MAX (__TZNL_GENL_ATTR_MAX - 1)

enum tznl_genl_cmd {
	TZNL_GENL_CMD_UNSPEC,
	TZNL_GENL_CMD_SET_SENSOR_NAME,  /* Set sensor name of thermal zone */
	TZNL_GENL_CMD_DEL_THERMAL_ZONE, /* Delete thermal zone */
	TZNL_GENL_CMD_GET_TEMPERATURE,  /* Get the temperature from userland application */
	TZNL_GENL_CMD_SET_EMUL_TEMP,    /* Set the emulation temperature to userland application */
	__TZNL_GENL_CMD_MAX,
};
#define TZNL_GENL_CMD_MAX (__TZNL_GENL_CMD_MAX - 1)

#endif /* _NETLINK_H */
