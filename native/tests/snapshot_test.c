/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "ax860i_snapshot.h"

struct command {
	int page; /* -1 for global reads, -2 for a page selector */
	ax_u8 reg;
	ax_u64 scale;
};

static const struct command plan[] = {
	{ -1, 0x88, 1000 }, { -1, 0x89, 1000 },
	{ -1, 0x97, 1000000 }, { -1, 0x8e, 1000 }, { -1, 0x90, 1 },
	{ -2, 0, 0 }, { 0, 0x8b, 1000 }, { 0, 0x8c, 1000 }, { 0, 0x96, 1000000 },
	{ -2, 1, 0 }, { 1, 0x8b, 1000 }, { 1, 0x8c, 1000 }, { 1, 0x96, 1000000 },
	{ -2, 2, 0 }, { 2, 0x8b, 1000 }, { 2, 0x8c, 1000 }, { 2, 0x96, 1000000 },
	{ -2, 3, 0 }, { 3, 0x96, 1000000 },
	{ -2, 4, 0 }, { 4, 0x96, 1000000 },
};

struct fake {
	unsigned int step;
	int page;
	int fail_step;
	int overflow;
};

static int select_page(void *context, ax_u8 page)
{
	struct fake *f = context;
	const struct command *c = &plan[f->step];

	assert(f->step < sizeof(plan) / sizeof(plan[0]));
	assert(c->page == -2 && c->reg == page);
	if ((int)f->step++ == f->fail_step)
		return -EIO;
	f->page = page;
	return 0;
}

static int number(void *context, ax_u8 reg, ax_u64 scale, ax_s64 *value)
{
	struct fake *f = context;
	const struct command *c = &plan[f->step];

	assert(f->step < sizeof(plan) / sizeof(plan[0]));
	assert(c->page == f->page && c->reg == reg && c->scale == scale);
	if ((int)f->step++ == f->fail_step)
		return -EIO;
	if (reg == 0x96) {
		if (f->overflow) {
			*value = f->page ? f->overflow :
				(f->overflow > 0 ? LONG_MAX : LONG_MIN);
			return 0;
		}
		/* Odd mantissas must NOT receive the Windows increment. */
		return ax_linear11((31U << 11) | (101 + 2 * f->page), scale, value);
	}
	*value = 100 + f->step;
	return 0;
}

static int acquire(struct fake *f, struct ax_core *core, ax_s64 *candidate)
{
	const struct ax_snapshot_ops ops = { select_page, number };
	int ret = ax_acquire_snapshot(&ops, f, candidate);

	/* Same admission rule as the worker: guard/publication only on success. */
	if (ret) {
		ax_poison(core);
		return ret;
	}
	core->rx.done = 1;
	assert(!ax_begin_guard(core, 100));
	return ax_publish_at(core, candidate, 600, 100);
}

int main(void)
{
	struct ax_core core;
	ax_s64 candidate[AX_SENSORS];
	struct fake f;
	int fail;
	unsigned int i;

	for (fail = -1; fail < (int)(sizeof(plan) / sizeof(plan[0])); fail++) {
		f = (struct fake){ .page = -1, .fail_step = fail };
		ax_core_init(&core);
		assert(!ax_start(&core, 0));
		core.phase = AX_ACQUIRE;
		for (i = 0; i < AX_SENSORS; i++)
			candidate[i] = -999;
		if (fail >= 0) {
			assert(acquire(&f, &core, candidate) == -EIO);
			assert(!core.valid && !core.generation);
			assert(core.state == AX_POISONED);
			assert(candidate[14] == -999);
		} else {
			assert(!acquire(&f, &core, candidate));
			assert(f.step == sizeof(plan) / sizeof(plan[0]));
			assert(candidate[0] == 101 && candidate[4] == 105);
			assert(candidate[5] == 107 && candidate[6] == 108);
			assert(candidate[8] == 111 && candidate[9] == 112);
			assert(candidate[11] == 115 && candidate[12] == 116);
			assert(candidate[7] == 50500000 && candidate[10] == 51500000);
			assert(candidate[13] == 52500000);
			assert(candidate[14] == 262500000);
			assert(core.valid && core.generation == 1);
			assert(core.cache[14] == candidate[14]);
		}
	}
	for (fail = -1; fail <= 1; fail += 2) {
		f = (struct fake){ .page = -1, .fail_step = -1, .overflow = fail };
		ax_core_init(&core);
		assert(!ax_start(&core, 0));
		core.phase = AX_ACQUIRE;
		candidate[14] = -999;
		assert(acquire(&f, &core, candidate) == -ERANGE);
		assert(!core.valid && !core.generation && candidate[14] == -999);
	}
	puts("five-page snapshot plan, stable indices, all failures and signed overflow passed");
	return 0;
}
