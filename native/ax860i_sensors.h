/* SPDX-License-Identifier: GPL-2.0-only */
/* Kernel-owned read/selector protocol and read-only hwmon surface. */
#ifndef AX860I_SENSORS_H
#define AX860I_SENSORS_H

#include "ax860i_snapshot.h"

static int ax_read_register(struct ax_device *a, u8 reg, u8 count,
			    u8 *data, u64 deadline)
{
	u8 setup[] = { 0x13, 3, 6, 1, 7, count, reg };
	u8 memory[] = { 8, 7, count };
	int ret;

	ret = ax_exchange(a, setup, sizeof(setup), NULL, 0, true, deadline);
	if (ret)
		return ret;
	return ax_exchange(a, memory, sizeof(memory), data, count, false, deadline);
}

static int ax_select_page(struct ax_device *a, u8 page)
{
	u8 command[] = { 0x13, 1, 4, 2, 0, page };
	u64 deadline = ax_now() + AX_REQUEST_MS;
	u8 readback;
	int ret, tries;

	if (page > 4 || !a->initialized)
		return ax_fail(a, -EINVAL);
	for (tries = 0; tries < 5; tries++) {
		ret = ax_exchange(a, command, sizeof(command), NULL, 0, true, deadline);
		if (ret)
			return ret;
		ret = ax_read_register(a, 0, 1, &readback, deadline);
		if (ret)
			return ret;
		if (readback == page)
			return 0;
	}
	return ax_fail(a, -EPROTO);
}

static int ax_number(struct ax_device *a, u8 reg, u64 scale, s64 *value)
{
	u8 data[2];
	int ret;

	ret = ax_read_register(a, reg, 2, data, ax_now() + AX_REQUEST_MS);
	if (ret)
		return ret;
	if (ax_linear11(data[0] | (data[1] << 8), scale, value) ||
	    *value < LONG_MIN || *value > LONG_MAX)
		return ax_fail(a, -ERANGE);
	return 0;
}

static int ax_snapshot_select(void *context, ax_u8 page)
{
	return ax_select_page(context, page);
}

static int ax_snapshot_number(void *context, ax_u8 reg, ax_u64 scale,
			      ax_s64 *value)
{
	return ax_number(context, reg, scale, value);
}

static int ax_snapshot(struct ax_device *a, s64 *candidate)
{
	static const struct ax_snapshot_ops ops = {
		.select_page = ax_snapshot_select,
		.number = ax_snapshot_number,
	};
	int ret = ax_acquire_snapshot(&ops, a, candidate);

	return ret ? ax_fail(a, ret) : 0;
}

static int ax_channel_index(enum hwmon_sensor_types type, u32 attr, int ch,
			    bool label)
{
	if (ch < 0)
		return -EOPNOTSUPP;
	switch (type) {
	case hwmon_in:
		if (ch < 4 && attr == (label ? hwmon_in_label : hwmon_in_input))
			return ch ? 5 + (ch - 1) * 3 : 0;
		break;
	case hwmon_curr:
		if (ch < 4 && attr == (label ? hwmon_curr_label : hwmon_curr_input))
			return ch ? 6 + (ch - 1) * 3 : 1;
		break;
	case hwmon_power:
		if (ch < 5 && attr == (label ? hwmon_power_label : hwmon_power_input))
			return ch == 4 ? AX_TOTAL_POWER_INDEX :
				(ch ? 7 + (ch - 1) * 3 : 2);
		break;
	case hwmon_temp:
		if (!ch && attr == (label ? hwmon_temp_label : hwmon_temp_input))
			return 3;
		break;
	case hwmon_fan:
		if (!ch && attr == (label ? hwmon_fan_label : hwmon_fan_input))
			return 4;
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static umode_t ax_visible(const void *data, enum hwmon_sensor_types type,
			  u32 attr, int ch)
{
	/* Keep curr2..4 stable; hide both attributes of the AC-current slot. */
	if (type == hwmon_curr && ch == 0)
		return 0;
	return ax_channel_index(type, attr, ch, false) >= 0 ||
	       ax_channel_index(type, attr, ch, true) >= 0 ? 0444 : 0;
}

static int ax_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int ch, long *value)
{
	struct ax_device *a = dev_get_drvdata(dev);
	unsigned long flags;
	int index = ax_channel_index(type, attr, ch, false);
	int ret = 0;

	if (index < 0)
		return index;
	spin_lock_irqsave(&a->state_lock, flags);
	if (a->core.state == AX_STOPPING || a->core.state == AX_QUIESCED)
		ret = -ENODEV;
	else if (a->core.state == AX_POISONED)
		ret = -EIO;
	else if (!ax_fresh(&a->core, ax_now()))
		ret = -ENODATA;
	else
		*value = a->core.cache[index];
	spin_unlock_irqrestore(&a->state_lock, flags);
	return ret;
}

static int ax_hwmon_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int ch, const char **str)
{
	static const char * const labels[] = {
		"AC input", "AC input", "AC input (raw)",
		"Secondary ambient", "PSU fan",
		"+12V", "+12V", "+12V", "+5V", "+5V", "+5V",
		"+3.3V", "+3.3V", "+3.3V",
		"Total DC output",
	};
	int index = ax_channel_index(type, attr, ch, true);

	if (index < 0)
		return index;
	*str = labels[index];
	return 0;
}

static const struct hwmon_ops ax_hwmon_ops = {
	.is_visible = ax_visible,
	.read = ax_hwmon_read,
	.read_string = ax_hwmon_string,
};

static const struct hwmon_channel_info * const ax_info[] = {
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL,
		HWMON_P_INPUT | HWMON_P_LABEL, HWMON_P_INPUT | HWMON_P_LABEL,
		HWMON_P_INPUT | HWMON_P_LABEL, HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL,
};

static const struct hwmon_chip_info ax_chip = {
	.ops = &ax_hwmon_ops,
	.info = ax_info,
};

#endif /* AX860I_SENSORS_H */
