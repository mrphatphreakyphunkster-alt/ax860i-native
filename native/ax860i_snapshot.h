/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared kernel/userspace acquisition plan. No publication on partial success. */
#ifndef AX860I_SNAPSHOT_H
#define AX860I_SNAPSHOT_H

#include "ax860i_core.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/limits.h>
#else
#include <errno.h>
#include <limits.h>
#endif

#define AX_TOTAL_POWER_INDEX 14

struct ax_snapshot_ops {
	int (*select_page)(void *context, ax_u8 page);
	int (*number)(void *context, ax_u8 reg, ax_u64 scale, ax_s64 *value);
};

static inline int ax_acquire_snapshot(const struct ax_snapshot_ops *ops,
				      void *context, ax_s64 *candidate)
{
	static const ax_u8 registers[] = { 0x88, 0x89, 0x97, 0x8e, 0x90 };
	static const ax_u64 scales[] = { 1000, 1000, 1000000, 1000, 1 };
	static const ax_u8 rail_registers[] = { 0x8b, 0x8c, 0x96 };
	ax_s64 total = 0, power;
	unsigned int i, page;
	int ret;

	for (i = 0; i < 5; i++) {
		ret = ops->number(context, registers[i], scales[i], &candidate[i]);
		if (ret)
			return ret;
	}
	for (page = 0; page < 5; page++) {
		ret = ops->select_page(context, page);
		if (ret)
			return ret;
		if (page < 3) {
			for (i = 0; i < 3; i++) {
				ret = ops->number(context, rail_registers[i], scales[i],
						  &candidate[5 + page * 3 + i]);
				if (ret)
					return ret;
			}
			power = candidate[7 + page * 3];
		} else {
			/* Auxiliary pages contribute measured power only, never V*I. */
			ret = ops->number(context, 0x96, 1000000, &power);
			if (ret)
				return ret;
		}
		/* Subtractions cannot overflow after the explicit long bounds. */
		if (power < LONG_MIN || power > LONG_MAX ||
		    (power > 0 && total > LONG_MAX - power) ||
		    (power < 0 && total < LONG_MIN - power))
			return -ERANGE;
		total += power;
	}
	candidate[AX_TOTAL_POWER_INDEX] = total;
	return 0;
}

#endif /* AX860I_SNAPSHOT_H */
