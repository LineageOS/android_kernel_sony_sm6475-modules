// SPDX-License-Identifier: GPL-2.0-only
/*
 * Netlink routines for TZNL_BYPASS
 *
 * Copyright 2024, 2025 Sony Corporation
 */

#include <net/genetlink.h>

#include "netlink.h"
#include "tznl_bypass.h"

static const struct nla_policy tznl_genl_policy[TZNL_GENL_ATTR_MAX + 1] = {
	[TZNL_GENL_ATTR_TZ_ID]		= { .type = NLA_S32 },
	[TZNL_GENL_ATTR_SENSOR_NAME]	= { .type = NLA_STRING,
					    .len = THERMAL_NAME_LENGTH },
	[TZNL_GENL_ATTR_TEMPERATURE]	= { .type = NLA_S32 },
};

static const struct genl_multicast_group tznl_genl_mcgrps[] = {
	{ .name = TZNL_GENL_GROUP_NAME, },
};

static struct genl_family tznl_genl_family;

static int tznl_reply(struct genl_info *info, enum tznl_genl_cmd cmd, char *name, int tzid)
{
	struct sk_buff *skb;
	void *hdr;
	struct tznl_info *tznl_info = NULL;
	struct tznl_info *next_tznl_info = NULL;
	unsigned long flags;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		tznl_bypass_err("Failed to allocate message buffer\n");
		goto error;
	}

	hdr = genlmsg_put(skb, info->snd_portid, info->snd_seq,
			&tznl_genl_family, 0, cmd);
	if (!hdr) {
		tznl_bypass_err("Failed to create genetlink header\n");
		goto free_msg;
	}

	if (nla_put_s32(skb, TZNL_GENL_ATTR_TZ_ID, tzid)) {
		tznl_bypass_err("Failed to put thermal_zone id %d\n", tzid);
		goto nla_put_failure;
	}

	if (cmd == TZNL_GENL_CMD_SET_SENSOR_NAME) {
		if (name == NULL) {
			tznl_bypass_err("Not specify sensor name: %d\n", tzid);
			goto nla_put_failure;
		}
		if ((nla_put_string(skb, TZNL_GENL_ATTR_SENSOR_NAME, name))) {
			tznl_bypass_err("Failed to put sensor name %s\n", name);
			goto nla_put_failure;
		}
	}

	genlmsg_end(skb, hdr);
	return genlmsg_reply(skb, info);

nla_put_failure:
	genlmsg_cancel(skb, hdr);
free_msg:
	nlmsg_free(skb);
error:
	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry_safe(tznl_info, next_tznl_info, &tznl_list, node) {
		if (tznl_info->tz->id == tzid)
			unregister_tznl_sensor(tznl_info);
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	return -EMSGSIZE;
}

static int tznl_genl_set_sensor_name_doit(struct sk_buff *skb, struct genl_info *info)
{
	int cmd = info->genlhdr->cmd;
	struct nlattr *nla_sname = info->attrs[TZNL_GENL_ATTR_SENSOR_NAME];
	struct tznl_info *tznl_info = NULL;
	char *sname = NULL;
	int tzid;
	unsigned long flags;

	if (cmd != TZNL_GENL_CMD_SET_SENSOR_NAME || !nla_sname) {
		tznl_bypass_err("Command mismatch(%d) or not found sensor name\n", cmd);
		tzid = -EINVAL;
		goto reply;
	}

	sname = (char *)nla_data(nla_sname);

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry(tznl_info, &tznl_list, node) {
		if (!strncmp(tznl_info->tz->type, sname, THERMAL_NAME_LENGTH)) {
			spin_unlock_irqrestore(&tznl_list_slock, flags);
			tznl_bypass_info("Re-use thermal_zone%d(%s)\n", tznl_info->tz->id, sname);
			tzid = tznl_info->tz->id;
			goto reply;
		}
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);

	tzid = register_tznl_sensor(sname);
reply:
	return tznl_reply(info, TZNL_GENL_CMD_SET_SENSOR_NAME, sname, tzid);
}

static int tznl_genl_del_thermal_zone_doit(struct sk_buff *skb, struct genl_info *info)
{
	int cmd = info->genlhdr->cmd;
	struct nlattr *nla_tzid = info->attrs[TZNL_GENL_ATTR_TZ_ID];
	struct tznl_info *tznl_info = NULL;
	struct tznl_info *next_tznl_info = NULL;
	int tzid;
	unsigned long flags;

	if (cmd != TZNL_GENL_CMD_DEL_THERMAL_ZONE || !nla_tzid) {
		tzid = -EINVAL;
		goto reply;
	}

	tzid = nla_get_s32(nla_tzid);

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry_safe(tznl_info, next_tznl_info, &tznl_list, node) {
		if (tznl_info->tz->id == tzid) {
			unregister_tznl_sensor(tznl_info);
			spin_unlock_irqrestore(&tznl_list_slock, flags);
			goto reply;
		}
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	tznl_bypass_err("Not found thermal_zone%d\n", tzid);
	tzid = -ENODEV;

reply:
	return tznl_reply(info, TZNL_GENL_CMD_DEL_THERMAL_ZONE, NULL, tzid);
}

static int tznl_genl_get_temperature_doit(struct sk_buff *skb, struct genl_info *info)
{
	int cmd = info->genlhdr->cmd;
	struct nlattr *nla_tzid = info->attrs[TZNL_GENL_ATTR_TZ_ID];
	struct nlattr *nla_temp = info->attrs[TZNL_GENL_ATTR_TEMPERATURE];
	struct tznl_info *tznl_info = NULL;
	int tzid;
	unsigned long flags;

	if (cmd != TZNL_GENL_CMD_GET_TEMPERATURE || !nla_tzid || !nla_temp)
		return -EINVAL;

	tzid = nla_get_s32(nla_tzid);

	spin_lock_irqsave(&tznl_list_slock, flags);
	list_for_each_entry(tznl_info, &tznl_list, node) {
		if (tznl_info->tz->id == tzid) {
			spin_unlock_irqrestore(&tznl_list_slock, flags);
			goto found;
		}
	}
	spin_unlock_irqrestore(&tznl_list_slock, flags);
	tznl_bypass_err("Not found thermal_zone%d\n", tzid);
	return -ENODEV;

found:
	tznl_info->temp = nla_get_s32(nla_temp);
	tznl_info->cond = 1;
	wake_up(&tznl_info->waitq);
	return 0;
}

static const struct genl_small_ops tznl_genl_ops[] = {
	{
		.cmd = TZNL_GENL_CMD_SET_SENSOR_NAME,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = tznl_genl_set_sensor_name_doit,
	},
	{
		.cmd = TZNL_GENL_CMD_DEL_THERMAL_ZONE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = tznl_genl_del_thermal_zone_doit,
	},
	{
		.cmd = TZNL_GENL_CMD_GET_TEMPERATURE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = tznl_genl_get_temperature_doit,
	},
};

static struct genl_family tznl_genl_family = {
	.name		= TZNL_GENL_FAMILY_NAME,
	.version	= TZNL_GENL_VERSION,
	.maxattr	= TZNL_GENL_ATTR_MAX,
	.module		= THIS_MODULE,
	.policy		= tznl_genl_policy,
	.small_ops	= tznl_genl_ops,
	.n_small_ops	= ARRAY_SIZE(tznl_genl_ops),
	.resv_start_op	= TZNL_GENL_CMD_SET_EMUL_TEMP + 1,
	.mcgrps		= tznl_genl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(tznl_genl_mcgrps),
};

int tznl_get_temperature(struct tznl_info *tznl_info)
{
	struct sk_buff *skb;
	void *hdr;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		tznl_bypass_err("Failed to allocate message buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &tznl_genl_family, 0, TZNL_GENL_CMD_GET_TEMPERATURE);
	if (!hdr) {
		tznl_bypass_err("Failed to create genetlink header\n");
		goto free_msg;
	}

	if (nla_put_s32(skb, TZNL_GENL_ATTR_TZ_ID, tznl_info->tz->id)) {
		tznl_bypass_err("Failed to put thermal_zone id %d\n", tznl_info->tz->id);
		goto nla_put_failure;
	}

	genlmsg_end(skb, hdr);
	tznl_info->cond = 0;
	return genlmsg_multicast(&tznl_genl_family, skb, 0, 0, GFP_ATOMIC);

nla_put_failure:
	genlmsg_cancel(skb, hdr);
free_msg:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

int tznl_set_emul_temperature(struct tznl_info *tznl_info, int temp)
{
	struct sk_buff *skb;
	void *hdr;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		tznl_bypass_err("Failed to allocate message buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &tznl_genl_family, 0, TZNL_GENL_CMD_SET_EMUL_TEMP);
	if (!hdr) {
		tznl_bypass_err("Failed to create genetlink header\n");
		goto free_msg;
	}

	if (nla_put_s32(skb, TZNL_GENL_ATTR_TZ_ID, tznl_info->tz->id)) {
		tznl_bypass_err("Failed to put thermal_zone id %d\n", tznl_info->tz->id);
		goto nla_put_failure;
	}

	if (nla_put_s32(skb, TZNL_GENL_ATTR_TEMPERATURE, temp)) {
		tznl_bypass_err("Failed to put emulation temp %d\n", temp);
		goto nla_put_failure;
	}

	genlmsg_end(skb, hdr);
	tznl_info->cond = 0;
	return genlmsg_multicast(&tznl_genl_family, skb, 0, 0, GFP_ATOMIC);

nla_put_failure:
	genlmsg_cancel(skb, hdr);
free_msg:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

/**
 * tznl_genl_init - Register generic netlink family
 *
 * Return zero if initialized successfully, otherwise non-zero.
 */
int tznl_genl_init(void)
{
	int ret;

	ret = genl_register_family(&tznl_genl_family);
	if (unlikely(ret)) {
		tznl_bypass_err("failed to register netlink family\n");
		return ret;
	}

	return 0;
}

/**
 * tznl_genl_exit - Unregister generic netlink family
 */
void tznl_genl_exit(void)
{
	int ret;

	ret = genl_unregister_family(&tznl_genl_family);
	if (unlikely(ret)) {
		tznl_bypass_err("failed to unregister netlink family\n");
	}
}
