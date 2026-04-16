// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Jose Maria Perez Forte @ LineageOS
 *
 * appslog_kmsg.c - Kernel message dumper for the appslog partition
 *
 * This module registers a kmsg dumper and writes captured kernel log buffers
 * into a raw appslog block partition as fixed-size rotating records. Each
 * record contains a small metadata header followed by the kmsg payload.
 *
 * Records include sequence number, dump reason, wall-clock timestamp, boot ID,
 * kernel release, payload length, and CRC32 so userspace or recovery tooling can
 * identify and validate stored logs after reboot.
 *
 * Panic dumping is disabled by default because writing through the block layer
 * during panic context can be unsafe. It may be enabled with the enable_panic
 * module parameter for debugging only.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kmsg_dump.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/crc32.h>
#include <linux/random.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/utsname.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/sizes.h>
#include <linux/string.h>

#define APPLOG_MAGIC            0x474c5041
#define APPLOG_VERSION          2

#define APPLOG_MIN_RECORD_SIZE  SZ_4K
#define APPLOG_MAX_RECORD_SIZE  SZ_4M
#define APPLOG_MAX_SLOTS        64
#define APPLOG_OPEN_RETRY_MS    200
#define APPLOG_OPEN_MAX_TRIES   150

static char *appslog_path = "/dev/block/platform/soc/1d84000.ufshc/by-name/appslog";
module_param(appslog_path, charp, 0444);
MODULE_PARM_DESC(appslog_path, "Path to appslog block device");

static unsigned int record_size = SZ_256K;
module_param(record_size, uint, 0444);
MODULE_PARM_DESC(record_size, "Size of one raw log record (bytes)");

static unsigned int slots = 4;
module_param(slots, uint, 0444);
MODULE_PARM_DESC(slots, "Number of rotating records");

static unsigned long start_offset;
module_param(start_offset, ulong, 0444);
MODULE_PARM_DESC(start_offset, "Byte offset into appslog partition");

static bool enable_panic;
module_param(enable_panic, bool, 0444);
MODULE_PARM_DESC(enable_panic,
	"Also dump on KMSG_DUMP_PANIC (unsafe, may hang; debug-only)");

struct appslog_hdr {
	__le32 magic;
	__le16 version;
	__le16 hdr_size;
	__le32 seq;
	__le32 reason;
	__le64 real_nsec;
	__le64 boot_id;
	__le32 payload_len;
	__le32 payload_crc32;
	char   uts_release[32];
	u8     reserved[24];
} __packed;

static struct file *appslog_file;
static void *record_buf;

static atomic_t appslog_in_dump = ATOMIC_INIT(0);
static atomic_t appslog_seq     = ATOMIC_INIT(0);
static u64 appslog_boot_id;

static struct delayed_work appslog_open_work;
static int appslog_open_tries;

static struct kobject *appslog_kobj;

static u32 appslog_last_reason;
static u32 appslog_last_len;
static u32 appslog_write_err;

static void appslog_do_dump(struct kmsg_dumper *dumper,
			    enum kmsg_dump_reason reason);

static struct kmsg_dumper appslog_dumper = {
	.dump       = appslog_do_dump,
	.max_reason = KMSG_DUMP_SHUTDOWN,
};

static int appslog_write_record(enum kmsg_dump_reason reason,
				size_t payload_len)
{
	struct appslog_hdr *hdr = record_buf;
	const size_t max_payload = record_size - sizeof(*hdr);
	loff_t pos;
	ssize_t written;
	u32 seq;

	if (payload_len > max_payload)
		payload_len = max_payload;

	seq = atomic_inc_return(&appslog_seq);

	memset(hdr, 0, sizeof(*hdr));
	hdr->magic         = cpu_to_le32(APPLOG_MAGIC);
	hdr->version       = cpu_to_le16(APPLOG_VERSION);
	hdr->hdr_size      = cpu_to_le16(sizeof(*hdr));
	hdr->seq           = cpu_to_le32(seq);
	hdr->reason        = cpu_to_le32(reason);
	hdr->real_nsec     = cpu_to_le64(ktime_get_real_ns());
	hdr->boot_id       = cpu_to_le64(appslog_boot_id);
	hdr->payload_len   = cpu_to_le32(payload_len);
	hdr->payload_crc32 = cpu_to_le32(
		crc32(~0U, (u8 *)record_buf + sizeof(*hdr), payload_len));
	strscpy(hdr->uts_release, init_utsname()->release,
		sizeof(hdr->uts_release));

	pos = start_offset + (loff_t)((seq - 1) % slots) * record_size;

	written = kernel_write(appslog_file, record_buf, record_size, &pos);
	if (written != record_size) {
		appslog_write_err++;
		return (written < 0) ? (int)written : -EIO;
	}

	appslog_last_reason = reason;
	appslog_last_len    = payload_len;
	return 0;
}

static void appslog_do_dump(struct kmsg_dumper *dumper,
			    enum kmsg_dump_reason reason)
{
	struct kmsg_dump_iter iter;
	size_t len = 0;
	char *payload;

	if (!record_buf || !appslog_file)
		return;

	if (atomic_cmpxchg(&appslog_in_dump, 0, 1) != 0)
		return;

	if (reason == KMSG_DUMP_PANIC && !enable_panic)
		goto out;

	memset(record_buf, 0, record_size);
	payload = (char *)record_buf + sizeof(struct appslog_hdr);

	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, true, payload,
			     record_size - sizeof(struct appslog_hdr),
			     &len);

	appslog_write_record(reason, len);

out:
	atomic_set(&appslog_in_dump, 0);
}

static void appslog_scan_slots(void)
{
	struct appslog_hdr *hdr;
	u32 max_seq = 0;
	u32 i;

	hdr = kmalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr)
		return;

	for (i = 0; i < slots; i++) {
		loff_t pos = start_offset + (loff_t)i * record_size;
		ssize_t n  = kernel_read(appslog_file, hdr, sizeof(*hdr),
					 &pos);

		if (n != sizeof(*hdr))
			continue;
		if (le32_to_cpu(hdr->magic) != APPLOG_MAGIC)
			continue;
		if (le16_to_cpu(hdr->version) != APPLOG_VERSION)
			continue;
		if (le16_to_cpu(hdr->hdr_size) != sizeof(*hdr))
			continue;
		if (le32_to_cpu(hdr->payload_len) >
		    record_size - sizeof(*hdr))
			continue;

		if (le32_to_cpu(hdr->seq) > max_seq)
			max_seq = le32_to_cpu(hdr->seq);
	}

	atomic_set(&appslog_seq, max_seq);
	kfree(hdr);
}

static ssize_t seq_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", atomic_read(&appslog_seq));
}

static ssize_t boot_id_show(struct kobject *k, struct kobj_attribute *a,
			    char *buf)
{
	return sysfs_emit(buf, "%016llx\n", appslog_boot_id);
}

static ssize_t last_reason_show(struct kobject *k, struct kobj_attribute *a,
				char *buf)
{
	return sysfs_emit(buf, "%u\n", appslog_last_reason);
}

static ssize_t last_len_show(struct kobject *k, struct kobj_attribute *a,
			     char *buf)
{
	return sysfs_emit(buf, "%u\n", appslog_last_len);
}

static ssize_t write_err_show(struct kobject *k, struct kobj_attribute *a,
			      char *buf)
{
	return sysfs_emit(buf, "%u\n", appslog_write_err);
}

static ssize_t trigger_store(struct kobject *k, struct kobj_attribute *a,
			     const char *buf, size_t n)
{
	appslog_do_dump(&appslog_dumper, KMSG_DUMP_SHUTDOWN);
	return n;
}

static struct kobj_attribute appslog_attr_seq         = __ATTR_RO(seq);
static struct kobj_attribute appslog_attr_boot_id     = __ATTR_RO(boot_id);
static struct kobj_attribute appslog_attr_last_reason = __ATTR_RO(last_reason);
static struct kobj_attribute appslog_attr_last_len    = __ATTR_RO(last_len);
static struct kobj_attribute appslog_attr_write_err   = __ATTR_RO(write_err);
static struct kobj_attribute appslog_attr_trigger     =
	__ATTR(trigger, 0200, NULL, trigger_store);

static struct attribute *appslog_attrs[] = {
	&appslog_attr_seq.attr,
	&appslog_attr_boot_id.attr,
	&appslog_attr_last_reason.attr,
	&appslog_attr_last_len.attr,
	&appslog_attr_write_err.attr,
	&appslog_attr_trigger.attr,
	NULL,
};

static const struct attribute_group appslog_attr_group = {
	.attrs = appslog_attrs,
};

static void appslog_open_worker(struct work_struct *work)
{
	struct file *f;
	int ret;

	f = filp_open(appslog_path,
		      O_RDWR | O_DSYNC | O_LARGEFILE,
		      0);
	if (IS_ERR(f)) {
		long err = PTR_ERR(f);

		if ((err == -ENOENT || err == -ENXIO) &&
		    appslog_open_tries++ < APPLOG_OPEN_MAX_TRIES) {
			schedule_delayed_work(&appslog_open_work,
				msecs_to_jiffies(APPLOG_OPEN_RETRY_MS));
			return;
		}
		pr_err("appslog_kmsg: open %s failed: %ld\n",
		       appslog_path, err);
		return;
	}

	appslog_file = f;
	appslog_scan_slots();

	ret = kmsg_dump_register(&appslog_dumper);
	if (ret) {
		pr_err("appslog_kmsg: kmsg_dump_register: %d\n", ret);
		filp_close(appslog_file, NULL);
		appslog_file = NULL;
		return;
	}

	pr_info("appslog_kmsg: ready, %u slots x %u bytes at %s+0x%lx, resume seq=%u, boot_id=%016llx\n",
		slots, record_size, appslog_path, start_offset,
		atomic_read(&appslog_seq), appslog_boot_id);
}

static int __init appslog_kmsg_init(void)
{
	int ret;

	if (record_size < APPLOG_MIN_RECORD_SIZE ||
	    record_size > APPLOG_MAX_RECORD_SIZE ||
	    !is_power_of_2(record_size))
		return -EINVAL;
	if (slots == 0 || slots > APPLOG_MAX_SLOTS)
		return -EINVAL;
	if (record_size <= sizeof(struct appslog_hdr))
		return -EINVAL;

	get_random_bytes(&appslog_boot_id, sizeof(appslog_boot_id));

	record_buf = vzalloc(record_size);
	if (!record_buf)
		return -ENOMEM;

	if (enable_panic)
		appslog_dumper.max_reason = KMSG_DUMP_MAX;

	appslog_kobj = kobject_create_and_add("appslog_kmsg", kernel_kobj);
	if (!appslog_kobj) {
		ret = -ENOMEM;
		goto err_buf;
	}

	ret = sysfs_create_group(appslog_kobj, &appslog_attr_group);
	if (ret)
		goto err_kobj;

	INIT_DELAYED_WORK(&appslog_open_work, appslog_open_worker);
	schedule_delayed_work(&appslog_open_work, 0);

	return 0;

err_kobj:
	kobject_put(appslog_kobj);
	appslog_kobj = NULL;
err_buf:
	vfree(record_buf);
	record_buf = NULL;
	return ret;
}

static void __exit appslog_kmsg_exit(void)
{
	cancel_delayed_work_sync(&appslog_open_work);

	if (appslog_file) {
		kmsg_dump_unregister(&appslog_dumper);
		filp_close(appslog_file, NULL);
		appslog_file = NULL;
	}

	if (appslog_kobj) {
		sysfs_remove_group(appslog_kobj, &appslog_attr_group);
		kobject_put(appslog_kobj);
		appslog_kobj = NULL;
	}

	vfree(record_buf);
	record_buf = NULL;
}

module_init(appslog_kmsg_init);
module_exit(appslog_kmsg_exit);

MODULE_DESCRIPTION("Dump kernel kmsg into raw appslog partition");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_AUTHOR("Jose Maria Perez Forte <jmpfbmx@jmpfbmx.com>");
MODULE_LICENSE("GPL v2");
