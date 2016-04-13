/*
 * State Helper Driver
 *
 * Copyright (c) 2016, Pranav Vashi <neobuddy89@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/state_notifier.h>
#include <linux/workqueue.h>

#define STATE_HELPER			"state_helper"
#define HELPER_ENABLED			0
#define DEFAULT_MAX_CPUS_ONLINE		NR_CPUS
#define DEFAULT_SUSP_CPUS		1
#define DEBUG_MASK			1

static struct state_helper {
	unsigned int enabled;
	unsigned int max_cpus_online;
	unsigned int debug;	
} helper = {
	.enabled = HELPER_ENABLED,
	.max_cpus_online = DEFAULT_MAX_CPUS_ONLINE,
	.debug = DEBUG_MASK
};

static struct notifier_block notif;

static struct workqueue_struct *helper_wq;
static struct delayed_work helper_work;

static unsigned int target_cpus;

#define dprintk(msg...)		\
do { 				\
	if (helper.debug)	\
		pr_info(msg);	\
} while (0)


static void __ref state_helper_work(struct work_struct *work)
{
	int cpu;

	if (state_suspended)
		target_cpus = DEFAULT_SUSP_CPUS;
	else
		target_cpus = helper.max_cpus_online;

	if (target_cpus < num_online_cpus()) {
		for_each_online_cpu(cpu) {
			if (cpu == 0)
				continue;
			dprintk("%s: Switching CPU%u offline.\n",
				STATE_HELPER, cpu);
			cpu_down(cpu);
			if (target_cpus >= num_online_cpus())
				break;
		}
	} else if (target_cpus > num_online_cpus()) {
		for_each_possible_cpu(cpu) {
			if (target_cpus <= num_online_cpus())
				break;
			if (!cpu_online(cpu)) {
				cpu_up(cpu);
				dprintk("%s: Switching CPU%u online.\n",
					STATE_HELPER, cpu);
			}
		}
	} else {
		dprintk("%s: Target already achieved: %u.\n",
			STATE_HELPER, target_cpus);
		return;
	}

	if (helper.debug) {
		pr_info("%s: Target requested: %u.\n",
			STATE_HELPER, target_cpus);
		for_each_possible_cpu(cpu)
			pr_info("%s: CPU%u status: %u\n",
				STATE_HELPER, cpu, cpu_online(cpu));
	}
}

static void reschedule_work(void)
{
	queue_delayed_work(helper_wq, &helper_work, 0);
}

static int state_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	if (!helper.enabled)
		return NOTIFY_OK;

	reschedule_work();

	return NOTIFY_OK;
}

static void state_helper_start(void)
{
	helper_wq =
	    alloc_workqueue("state_helper_wq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!helper_wq) {
		pr_err("%s: Failed to allocate helper workqueue\n",
		       STATE_HELPER);
		goto err_out;
	}

	notif.notifier_call = state_notifier_callback;
	if (state_register_client(&notif)) {
		pr_err("%s: Failed to register State notifier callback\n",
			STATE_HELPER);
		goto err_dev;
	}

	INIT_DELAYED_WORK(&helper_work, state_helper_work);
	reschedule_work();

	return;
err_dev:
	destroy_workqueue(helper_wq);
err_out:
	helper.enabled = 0;
	return;
}

static void __ref state_helper_stop(void)
{
	int cpu;

	state_unregister_client(&notif);
	notif.notifier_call = NULL;

	flush_workqueue(helper_wq);
	cancel_delayed_work_sync(&helper_work);

	/* Wake up all the sibling cores */
	for_each_possible_cpu(cpu)
		if (!cpu_online(cpu))
			cpu_up(cpu);
}


/************************** sysfs interface ************************/

static ssize_t show_enabled(struct kobject *kobj,
				struct kobj_attribute *attr, 
				char *buf)
{
	return sprintf(buf, "%u\n", helper.enabled);
}

static ssize_t store_enabled(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 0 || val > 1)
		return -EINVAL;

	if (val == helper.enabled)
		return count;

	helper.enabled = val;

	if (helper.enabled)
		state_helper_start();
	else
		state_helper_stop();

	return count;
}

static ssize_t show_max_cpus_online(struct kobject *kobj,
				struct kobj_attribute *attr, 
				char *buf)
{
	return sprintf(buf, "%u\n",helper.max_cpus_online);
}

static ssize_t store_max_cpus_online(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > NR_CPUS)
		return -EINVAL;

	helper.max_cpus_online = val;
	if (helper.enabled)
		reschedule_work();

	return count;
}

static ssize_t show_debug_mask(struct kobject *kobj,
				struct kobj_attribute *attr, 
				char *buf)
{
	return sprintf(buf, "%u\n", helper.debug);
}

static ssize_t store_debug_mask(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 0 || val > 1)
		return -EINVAL;

	if (val == helper.debug)
		return count;

	helper.debug = val;

	return count;
}

#define KERNEL_ATTR_RW(_name) 				\
static struct kobj_attribute _name##_attr = 		\
	__ATTR(_name, 0664, show_##_name, store_##_name)

KERNEL_ATTR_RW(enabled);
KERNEL_ATTR_RW(max_cpus_online);
KERNEL_ATTR_RW(debug_mask);

static struct attribute *state_helper_attrs[] = {
	&enabled_attr.attr,
	&max_cpus_online_attr.attr,
	&debug_mask_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = state_helper_attrs,
	.name = STATE_HELPER,
};

/************************** sysfs end ************************/

static int state_helper_probe(struct platform_device *pdev)
{
	int ret = 0;

	ret = sysfs_create_group(kernel_kobj, &attr_group);

	if (helper.enabled)
		state_helper_start();

	return ret;
}

static struct platform_device state_helper_device = {
	.name = STATE_HELPER,
	.id = -1,
};

static int state_helper_remove(struct platform_device *pdev)
{
	if (helper.enabled)
		state_helper_stop();

	return 0;
}

static struct platform_driver state_helper_driver = {
	.probe = state_helper_probe,
	.remove = state_helper_remove,
	.driver = {
		.name = STATE_HELPER,
		.owner = THIS_MODULE,
	},
};

static int __init state_helper_init(void)
{
	int ret;

	ret = platform_driver_register(&state_helper_driver);
	if (ret) {
		pr_err("%s: Driver register failed: %d\n", STATE_HELPER, ret);
		return ret;
	}

	ret = platform_device_register(&state_helper_device);
	if (ret) {
		pr_err("%s: Device register failed: %d\n", STATE_HELPER, ret);
		return ret;
	}

	pr_info("%s: Device init\n", STATE_HELPER);

	return ret;
}

static void __exit state_helper_exit(void)
{
	platform_device_unregister(&state_helper_device);
	platform_driver_unregister(&state_helper_driver);
}

late_initcall(state_helper_init);
module_exit(state_helper_exit);

MODULE_AUTHOR("Pranav Vashi <neobuddy89@gmail.com>");
MODULE_DESCRIPTION("State Helper Driver");
MODULE_LICENSE("GPLv2");
