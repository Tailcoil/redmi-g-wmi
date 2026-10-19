// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Redmi G 2022（TM2137）WMI 驱动。
 *
 * 基于 bitland-mifs-wmi（Mingyou Chen）和 redmi-wmi（Gladyshev Ilya），
 * 按这台机器 BIOS 实际实现的 MiInterface 功能重写。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_profile.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/units.h>
#include <linux/wmi.h>

#define DRV_NAME		"redmi-g-wmi"
#define MIFS_GUID		"B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"
#define EVENT_GUID		"46C93E13-EE9B-4262-8488-563BCA757FEF"

#define MIFS_GET		0xFA00
#define MIFS_SET		0xFB00
#define MIFS_OK			0x8000

/* BIOS 只实现了这几个功能，编号写在 FUN2 的高字节 */
#define FN_PERF_MODE		0x0800
#define FN_FAN_TEMP		0x0900	/* 只读 */
#define FN_LOCKS		0x0A00	/* FUN3 选择子功能 */

enum mifs_lock {
	LOCK_TOUCHPAD	= 0,
	LOCK_FN		= 1,
	LOCK_WIN	= 2,
};

/*
 * 模式编号是 BIOS 接口上的值，和 EC 的 ITSM 不同：
 * 1/2 对应 ITSM 1/2，3 对应 ITSM 0，4 对应 ITSM 3。
 * 适配器不足 180W 时 BIOS 会强制降到 2，用电池时 3/4 会降到 1。
 */
enum mifs_mode {
	MODE_BALANCED	= 1,
	MODE_QUIET	= 2,
	MODE_PERF	= 3,
	MODE_MAX	= 4,
};

#define EVENT_TYPE_HOTKEY	1
#define EVENT_ID_PERF_MODE	0x16
#define AI_KEY_VALUE_MASK	BIT(8)

struct mifs_in {
	__le16 fun1;
	__le16 fun2;
	__le16 fun3;
	__le32 fun4;
	u8 reserved[22];
} __packed;

struct mifs_out {
	__le16 status;
	__le16 function;
	__le16 data0;
	__le32 data1;
	__le32 data2;
	__le32 data3;
	__le32 data4;
} __packed;

static_assert(sizeof(struct mifs_in) == 32);

static BLOCKING_NOTIFIER_HEAD(mode_notifier);

struct redmi_g_wmi {
	struct wmi_device *wdev;
	struct mutex lock;		/* 串行化 BIOS 调用 */
	struct device *pp_dev;
	struct notifier_block nb;
	struct input_dev *input;
	int saved_mode;
};

static int mifs_call(struct redmi_g_wmi *drv, u16 op, u16 func, u16 arg,
		     u32 val, struct mifs_out *out)
{
	struct mifs_in in = {
		.fun1 = cpu_to_le16(op),
		.fun2 = cpu_to_le16(func),
		.fun3 = cpu_to_le16(arg),
		.fun4 = cpu_to_le32(val),
	};
	struct wmi_buffer in_buf = { .length = sizeof(in), .data = &in };
	struct wmi_buffer out_buf = {};
	struct mifs_out *res;
	int ret;

	scoped_guard(mutex, &drv->lock)
		ret = wmidev_invoke_method(drv->wdev, 0, 1, &in_buf, &out_buf,
					   sizeof(*res));
	/* 不支持的功能 BIOS 返回整数 0xE000 而不是缓冲区 */
	if (ret)
		return ret == -ENODATA || ret == -EPROTO ? -EOPNOTSUPP : ret;

	res = out_buf.data;
	if (le16_to_cpu(res->status) != MIFS_OK) {
		kfree(res);
		return -EOPNOTSUPP;
	}
	if (out)
		*out = *res;
	kfree(res);
	return 0;
}

static int mode_get(struct redmi_g_wmi *drv)
{
	struct mifs_out out;
	int ret;

	ret = mifs_call(drv, MIFS_GET, FN_PERF_MODE, 0, 0, &out);
	return ret ?: le16_to_cpu(out.data0);
}

/* BIOS 会按供电情况悄悄改写请求，设置后读回确认 */
static int mode_set(struct redmi_g_wmi *drv, int mode)
{
	int ret;

	ret = mifs_call(drv, MIFS_SET, FN_PERF_MODE, mode, 0, NULL);
	if (ret)
		return ret;

	ret = mode_get(drv);
	if (ret < 0)
		return ret;
	if (ret != mode) {
		platform_profile_notify(drv->pp_dev);
		return -EOPNOTSUPP;
	}
	return 0;
}

static const struct {
	int mode;
	enum platform_profile_option profile;
} mode_map[] = {
	{ MODE_QUIET,		PLATFORM_PROFILE_LOW_POWER },
	{ MODE_BALANCED,	PLATFORM_PROFILE_BALANCED },
	{ MODE_PERF,		PLATFORM_PROFILE_PERFORMANCE },
	{ MODE_MAX,		PLATFORM_PROFILE_MAX_POWER },
};

static int profile_get(struct device *dev, enum platform_profile_option *profile)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);
	int mode = mode_get(drv);

	if (mode < 0)
		return mode;

	for (int i = 0; i < ARRAY_SIZE(mode_map); i++) {
		if (mode_map[i].mode == mode) {
			*profile = mode_map[i].profile;
			return 0;
		}
	}
	dev_warn_once(dev, "unknown mode %d\n", mode);
	return -EPROTO;
}

static int profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);

	for (int i = 0; i < ARRAY_SIZE(mode_map); i++)
		if (mode_map[i].profile == profile)
			return mode_set(drv, mode_map[i].mode);
	return -EOPNOTSUPP;
}

static int profile_probe(void *data, unsigned long *choices)
{
	for (int i = 0; i < ARRAY_SIZE(mode_map); i++)
		set_bit(mode_map[i].profile, choices);
	return 0;
}

static const struct platform_profile_ops profile_ops = {
	.probe = profile_probe,
	.profile_get = profile_get,
	.profile_set = profile_set,
};

/* 风扇编号沿用 EC（F1/F2），哪个对应 CPU 还没确认；temp1 是 CPU（EC TSR6） */
static int fan_temp_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);
	struct mifs_out out;
	int ret;

	ret = mifs_call(drv, MIFS_GET, FN_FAN_TEMP, 0, 0, &out);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_fan:
		*val = channel ? le16_to_cpu(out.data0) : le32_to_cpu(out.data1);
		return 0;
	case hwmon_temp:
		*val = (channel ? le32_to_cpu(out.data3) : le32_to_cpu(out.data2)) *
		       MILLIDEGREE_PER_DEGREE;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int fan_temp_label(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, const char **str)
{
	if (type == hwmon_temp && channel == 0) {
		*str = "CPU";
		return 0;
	}
	return -EOPNOTSUPP;
}

static umode_t fan_temp_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_label && channel != 0)
		return 0;
	return 0444;
}

static const struct hwmon_channel_info *const fan_temp_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops fan_temp_ops = {
	.is_visible = fan_temp_visible,
	.read = fan_temp_read,
	.read_string = fan_temp_label,
};

static const struct hwmon_chip_info fan_temp_chip = {
	.ops = &fan_temp_ops,
	.info = fan_temp_info,
};

/*
 * 三个锁的读写语义并不一致：
 * 触控板和 Win 键是直接读写 EC 位；Fn 锁读回的是 !FNHK，写入 1 时 BIOS 清掉 FNHK。
 * 这里统一成“写什么读回什么”。
 */
static ssize_t lock_show(struct device *dev, char *buf, enum mifs_lock lock)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);
	struct mifs_out out;
	int ret;

	ret = mifs_call(drv, MIFS_GET, FN_LOCKS, lock, 0, &out);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", !!le32_to_cpu(out.data1));
}

static ssize_t lock_store(struct device *dev, const char *buf, size_t count,
			  enum mifs_lock lock)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);
	bool val;
	int ret;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	ret = mifs_call(drv, MIFS_SET, FN_LOCKS, lock, val, NULL);
	return ret ?: count;
}

#define LOCK_ATTR(_name, _lock)							\
static ssize_t _name##_show(struct device *dev,					\
			    struct device_attribute *attr, char *buf)		\
{										\
	return lock_show(dev, buf, _lock);					\
}										\
static ssize_t _name##_store(struct device *dev,				\
			     struct device_attribute *attr,			\
			     const char *buf, size_t count)			\
{										\
	return lock_store(dev, buf, count, _lock);				\
}										\
static DEVICE_ATTR_RW(_name)

LOCK_ATTR(touchpad_lock, LOCK_TOUCHPAD);
LOCK_ATTR(fn_lock, LOCK_FN);
LOCK_ATTR(win_lock, LOCK_WIN);

static struct attribute *mifs_attrs[] = {
	&dev_attr_touchpad_lock.attr,
	&dev_attr_fn_lock.attr,
	&dev_attr_win_lock.attr,
	NULL
};

static umode_t mifs_attr_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(kobj_to_dev(kobj));

	/* 事件设备不提供这些属性 */
	return drv->pp_dev ? attr->mode : 0;
}

static const struct attribute_group mifs_group = {
	.attrs = mifs_attrs,
	.is_visible = mifs_attr_visible,
};
__ATTRIBUTE_GROUPS(mifs);

/* 事件编码：type | id << 8 | value << 16，按键表取自 redmi-wmi */
static const struct key_entry keymap[] = {
	{ KE_KEY, 0x00000201, { KEY_SELECTIVE_SCREENSHOT } },
	{ KE_KEY, 0x00000301, { KEY_ALL_APPLICATIONS } },
	{ KE_KEY, 0x00001b01, { KEY_CONFIG } },
	{ KE_KEY, 0x00011b01, { KEY_CONFIG } },
	{ KE_KEY, 0x00010101, { KEY_SWITCHVIDEOMODE } },
	{ KE_KEY, 0x00001a01, { KEY_REFRESH_RATE_TOGGLE } },
	{ KE_KEY, 0x00011801, { KEY_ASSISTANT } },
	{ KE_KEY, 0x00011901, { KEY_ASSISTANT } },
	{ KE_KEY, 0x00010a01, { KEY_VENDOR } },
	{ KE_KEY, 0x00011101, { KEY_F13 } },
	{ KE_KEY, 0x00011201, { KEY_F14 } },
	{ KE_KEY, 0x00011301, { KEY_F15 } },
	{ KE_KEY, 0x00011401, { KEY_F16 } },
	{ KE_KEY, 0x00011501, { KEY_F17 } },

	/* 由 EC 自己处理，只是状态通知 */
	{ KE_IGNORE, 0x00000501, {} },	/* 键盘背光 */
	{ KE_IGNORE, 0x00050501, {} },
	{ KE_IGNORE, 0x000a0501, {} },
	{ KE_IGNORE, 0x00800501, {} },
	{ KE_IGNORE, 0x00000601, {} },	/* 触控板锁 */
	{ KE_IGNORE, 0x00010601, {} },
	{ KE_IGNORE, 0x00000701, {} },	/* Fn 锁 */
	{ KE_IGNORE, 0x00010701, {} },
	{ KE_IGNORE, 0x00000901, {} },	/* CapsLock */
	{ KE_IGNORE, 0x00010901, {} },
	{ KE_IGNORE, 0x00000d01, {} },	/* NumLock */
	{ KE_IGNORE, 0x00010d01, {} },
	{ KE_IGNORE, 0x00001701, {} },	/* Win 键锁 */
	{ KE_IGNORE, 0x00011701, {} },

	{ KE_END }
};

static void event_handle(struct wmi_device *wdev, const struct wmi_buffer *buf)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(&wdev->dev);
	const struct key_entry *entry;
	bool autorelease = true;
	int value = 1;
	u32 payload;
	u8 *p = buf->data;

	if (p[0] != EVENT_TYPE_HOTKEY)
		return;

	if (p[1] == EVENT_ID_PERF_MODE) {
		blocking_notifier_call_chain(&mode_notifier, 0, NULL);
		return;
	}

	payload = get_unaligned_le32(p);
	entry = sparse_keymap_entry_from_scancode(drv->input, payload);
	if (!entry) {
		/* 没见过的事件，留着方便继续摸 BIOS 接口 */
		dev_notice_ratelimited(&wdev->dev, "unknown event %*ph\n", 8, p);
		return;
	}

	if (entry->keycode == KEY_ASSISTANT) {
		value = !(payload & AI_KEY_VALUE_MASK);
		autorelease = false;
	}

	guard(mutex)(&drv->lock);
	sparse_keymap_report_entry(drv->input, entry, value, autorelease);
}

static int mode_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct redmi_g_wmi *drv = container_of(nb, struct redmi_g_wmi, nb);

	platform_profile_notify(drv->pp_dev);
	return NOTIFY_OK;
}

static void mode_notifier_unregister(void *nb)
{
	blocking_notifier_chain_unregister(&mode_notifier, nb);
}

static int probe_event(struct redmi_g_wmi *drv)
{
	struct device *dev = &drv->wdev->dev;
	int ret;

	drv->input = devm_input_allocate_device(dev);
	if (!drv->input)
		return -ENOMEM;

	drv->input->name = "Redmi G WMI keys";
	drv->input->phys = "wmi/input0";
	drv->input->id.bustype = BUS_HOST;

	ret = sparse_keymap_setup(drv->input, keymap, NULL);
	if (ret)
		return ret;

	return input_register_device(drv->input);
}

static int probe_control(struct redmi_g_wmi *drv)
{
	struct device *dev = &drv->wdev->dev;
	struct device *hwmon;
	int ret;

	drv->pp_dev = devm_platform_profile_register(dev, DRV_NAME, drv, &profile_ops);
	if (IS_ERR(drv->pp_dev))
		return PTR_ERR(drv->pp_dev);

	hwmon = devm_hwmon_device_register_with_info(dev, "redmi_g_wmi", drv,
						     &fan_temp_chip, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	drv->nb.notifier_call = mode_notify;
	ret = blocking_notifier_chain_register(&mode_notifier, &drv->nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, mode_notifier_unregister, &drv->nb);
}

static int redmi_g_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct redmi_g_wmi *drv;
	int ret;

	drv = devm_kzalloc(&wdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->wdev = wdev;
	ret = devm_mutex_init(&wdev->dev, &drv->lock);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, drv);

	return context ? probe_event(drv) : probe_control(drv);
}

/* 唤醒后 EC 可能回到默认模式，恢复休眠前的选择 */
static int redmi_g_wmi_suspend(struct device *dev)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);

	if (drv->pp_dev)
		drv->saved_mode = mode_get(drv);
	return 0;
}

static int redmi_g_wmi_resume(struct device *dev)
{
	struct redmi_g_wmi *drv = dev_get_drvdata(dev);

	if (drv->pp_dev && drv->saved_mode > 0 && mode_get(drv) != drv->saved_mode)
		mode_set(drv, drv->saved_mode);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(redmi_g_wmi_pm, redmi_g_wmi_suspend, redmi_g_wmi_resume);

static const struct wmi_device_id redmi_g_wmi_id_table[] = {
	{ MIFS_GUID, NULL },
	{ EVENT_GUID, (void *)1 },
	{}
};
MODULE_DEVICE_TABLE(wmi, redmi_g_wmi_id_table);

static struct wmi_driver redmi_g_wmi_driver = {
	.no_singleton = true,
	.driver = {
		.name = DRV_NAME,
		.dev_groups = mifs_groups,
		.pm = pm_sleep_ptr(&redmi_g_wmi_pm),
	},
	.id_table = redmi_g_wmi_id_table,
	.min_event_size = 8,
	.probe = redmi_g_wmi_probe,
	.notify_new = event_handle,
};
module_wmi_driver(redmi_g_wmi_driver);

MODULE_AUTHOR("Tailcoil");
MODULE_AUTHOR("Mingyou Chen <qby140326@gmail.com>");
MODULE_AUTHOR("Gladyshev Ilya <foxido@foxido.dev>");
MODULE_DESCRIPTION("Redmi G 2022 WMI driver");
MODULE_LICENSE("GPL");
