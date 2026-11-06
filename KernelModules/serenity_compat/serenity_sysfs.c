// SPDX-License-Identifier: BSD-2-Clause
/*
 * SerenityOS' "virtual" kernel files, rendered from Linux's own bookkeeping.
 *
 * ResourceGraph and the Network applet read small JSON documents that on
 * SerenityOS are produced by the kernel. Providing them here means those
 * applets run unmodified instead of needing a Linux branch each.
 */

#include <linux/ethtool.h>
#include <linux/inetdevice.h>
#include <linux/kernel.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kernel_stat.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/net_namespace.h>

#include "serenity_compat.h"

#define JSON_BUF_SIZE PAGE_SIZE

struct json {
	char *buf;
	size_t len;
	size_t cap;
};

static void json_init(struct json *j)
{
	j->buf = kmalloc(JSON_BUF_SIZE, GFP_KERNEL);
	if (!j->buf)
		return;
	j->buf[0] = '\0';
	j->len = 0;
	j->cap = JSON_BUF_SIZE;
}

static void json_add(struct json *j, const char *fmt, ...)
{
	va_list args;
	int written;

	if (!j->buf || j->len >= j->cap - 1)
		return;

	va_start(args, fmt);
	written = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, args);
	va_end(args);

	if (written > 0) {
		j->len += min((size_t)written, j->cap - j->len - 1);
		j->buf[j->len] = '\0';
	}
}

/* /sys/kernel/stats */
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *page)
{
	struct json j;
	u64 total, idle = 0;
	int cpu, ret;

	json_init(&j);
	if (!j.buf)
		return -ENOMEM;

	/*
	 * kernel_cpustat is either ticks or nanoseconds depending on the
	 * accounting config, which makes it a poor source here. Idle time has a
	 * dedicated accessor, and "total" is simply wall clock * online CPUs -
	 * which is exactly what ResourceGraph's utilisation ratio needs.
	 */
	total = div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC) * num_online_cpus();

	/*
	 * Idle buckets are in nanoseconds on this kernel (they were ticks before
	 * v4.11; CONFIG_VIRT_CPU_ACCOUNTING keeps them in nsec). Clamp so a unit
	 * change can never make utilisation go negative.
	 */
	for_each_online_cpu(cpu) {
		const struct kernel_cpustat *kcs = &kcpustat_cpu(cpu);

		idle += kcs->cpustat[CPUTIME_IDLE];
		idle += kcs->cpustat[CPUTIME_IOWAIT];
	}
	idle = min(div_u64(idle, NSEC_PER_MSEC), total);

	json_add(&j, "{\"total_time\":%llu,\"idle_time\":%llu}\n", total, idle);
	ret = sysfs_emit(page, "%s", j.buf);
	kfree(j.buf);
	return ret;
}

/* /sys/kernel/memstat */
static ssize_t memstat_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *page)
{
	struct json j;
	struct sysinfo si;
	u64 allocated, uncommitted;
	int ret;

	json_init(&j);
	if (!j.buf)
		return -ENOMEM;

	si_meminfo(&si);

	allocated = (u64)(si.totalram - si.freeram);
	uncommitted = (u64)si.freeram;

	/*
	 * ResourceGraph computes:
	 *   allocated = kmalloc_allocated + (physical_allocated + physical_committed) * PAGE_SIZE
	 *   available = total * PAGE_SIZE - allocated
	 * so "allocated" must be used pages and "uncommitted" must be free ones.
	 */
	json_add(&j,
		 "{\"kmalloc_allocated\":0,\"kmalloc_available\":0,"
		 "\"kmalloc_total_pages\":0,\"kmalloc_total_bytes\":0,"
		 "\"physical_allocated\":%llu,\"physical_available\":%llu,"
		 "\"physical_committed\":0,\"physical_uncommitted\":%llu,"
		 "\"total_pages\":%llu}\n",
		 allocated, uncommitted, uncommitted, (u64)si.totalram);

	ret = sysfs_emit(page, "%s", j.buf);
	kfree(j.buf);
	return ret;
}

/* /sys/kernel/net/adapters */
static ssize_t adapters_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *page)
{
	struct json j;
	struct net_device *dev;
	bool first = true;
	int ret;

	json_init(&j);
	if (!j.buf)
		return -ENOMEM;

	json_add(&j, "[");

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		struct rtnl_link_stats64 temp;
		const struct rtnl_link_stats64 *stats;
		char address[IFNAMSIZ * 4] = "null";
		bool link_up = netif_running(dev) && netif_carrier_ok(dev);
		int speed = 0;
		struct in_device *in_dev;

		stats = dev_get_stats(dev, &temp);

		in_dev = __in_dev_get_rtnl(dev);
		if (in_dev && in_dev->ifa_list)
			snprintf(address, sizeof(address), "%pI4",
				 &in_dev->ifa_list->ifa_address);

		if (dev->ethtool_ops && dev->ethtool_ops->get_link_ksettings) {
			struct ethtool_link_ksettings cmd;

			memset(&cmd, 0, sizeof(cmd));
			cmd.base.cmd = ETHTOOL_GLINKSETTINGS;
			if (!dev->ethtool_ops->get_link_ksettings(dev, &cmd))
				speed = cmd.base.speed;
		}

		json_add(&j,
			 "%s{\"name\":\"%s\",\"ipv4_address\":\"%s\","
			 "\"link_up\":%s,\"link_speed\":%d,"
			 "\"bytes_in\":%llu,\"bytes_out\":%llu,"
			 "\"packets_in\":%llu,\"packets_out\":%llu}",
			 first ? "" : ",", dev->name, address,
			 link_up ? "true" : "false", speed,
			 (u64)stats->rx_bytes, (u64)stats->tx_bytes,
			 (u64)stats->rx_packets, (u64)stats->tx_packets);
		first = false;
	}
	rtnl_unlock();

	json_add(&j, "]\n");
	ret = sysfs_emit(page, "%s", j.buf);
	kfree(j.buf);
	return ret;
}

static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute memstat_attr = __ATTR_RO(memstat);
static struct kobj_attribute adapters_attr = __ATTR_RO(adapters);

static struct kobject *net_kobj;

int serenity_sysfs_init(void)
{
	int ret;

	/*
	 * ResourceGraph opens /sys/kernel/stats and /sys/kernel/memstat directly,
	 * so those have to be files of kernel_kobj - not kobjects of their own.
	 */
	ret = sysfs_create_file(kernel_kobj, &stats_attr.attr);
	if (ret)
		return ret;

	ret = sysfs_create_file(kernel_kobj, &memstat_attr.attr);
	if (ret)
		goto err_stats;

	/* /sys/kernel/net/adapters is a directory, so a kobject is correct here. */
	net_kobj = kobject_create_and_add("net", kernel_kobj);
	if (!net_kobj) {
		ret = -ENOMEM;
		goto err_memstat;
	}

	ret = sysfs_create_file(net_kobj, &adapters_attr.attr);
	if (ret)
		goto err_net;

	pr_info("serenity_compat: /sys/kernel/{stats,memstat,net/adapters} ready\n");
	return 0;

err_net:
	kobject_put(net_kobj);
	net_kobj = NULL;
err_memstat:
	sysfs_remove_file(kernel_kobj, &memstat_attr.attr);
err_stats:
	sysfs_remove_file(kernel_kobj, &stats_attr.attr);
	return ret;
}

void serenity_sysfs_exit(void)
{
	if (net_kobj) {
		sysfs_remove_file(net_kobj, &adapters_attr.attr);
		kobject_put(net_kobj);
		net_kobj = NULL;
	}
	sysfs_remove_file(kernel_kobj, &memstat_attr.attr);
	sysfs_remove_file(kernel_kobj, &stats_attr.attr);
}
