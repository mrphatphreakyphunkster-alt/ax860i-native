// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "ax860i_core.h"
#include "ax860i_usb_plan.h"

static struct ax_core running(void)
{
	struct ax_core c;

	ax_core_init(&c);
	assert(!ax_start(&c, 100));
	assert(!ax_quarantine(&c, 599));
	assert(ax_quarantine(&c, 600) == 1);
	return c;
}

static unsigned int frame(const ax_u8 *data, unsigned int length, ax_u8 *out)
{
	unsigned int size = ax_encode(data, length, out, 66);

	assert(size);
	out[0] = 0xa8;
	return size;
}

static void feed(struct ax_core *c, const ax_u8 *data, unsigned int size, ax_u64 now)
{
	for (unsigned int i = 0; i < size; i++)
		ax_receive(c, data[i], 0, now);
}

static void codec(void)
{
	static const ax_u8 table[] = {
		0x55, 0x56, 0x59, 0x5a, 0x65, 0x66, 0x69, 0x6a,
		0x95, 0x96, 0x99, 0x9a, 0xa5, 0xa6, 0xa9, 0xaa
	};
	ax_u8 encoded[66], data[32];
	struct ax_core c;

	for (unsigned int i = 0; i < 16; i++) {
		assert(ax_symbol(i) == table[i]);
		assert(ax_nibble(table[i]) == (int)i);
	}
	for (unsigned int value = 0; value < 256; value++) {
		data[0] = value;
		assert(ax_encode(data, 1, encoded, sizeof(encoded)) == 4);
		assert(encoded[0] == 0x54 && !encoded[3]);
		assert(encoded[1] == table[value & 15]);
		assert(encoded[2] == table[value >> 4]);
		unsigned int size = frame(data, 1, encoded);

		for (unsigned int split = 0; split <= size; split++) {
			c = running();
			assert(!ax_arm(&c, 1, 0, 3600));
			feed(&c, encoded, split, 601);
			assert(c.rx.done == (split == size));
			feed(&c, encoded + split, size - split, 650);
			assert(c.state == AX_RUNNING && c.rx.done);
			assert(c.rx.len == 1 && c.rx.data[0] == value);
		}
	}
	for (unsigned int i = 0; i < 32; i++)
		data[i] = i * 7;
	c = running();
	assert(!ax_arm(&c, 32, 0, 3600));
	feed(&c, encoded, frame(data, 32, encoded), 700);
	assert(c.rx.done && !memcmp(data, c.rx.data, 32));
	assert(!ax_encode(data, 33, encoded, sizeof(encoded)));
	assert(!ax_encode(data, 32, encoded, 65));
}

static void malformed(void)
{
	static const ax_u8 cases[][8] = {
		{0x54, 0}, {0xa9, 0}, {0xaa, 0}, {0, 0}, {0xa8, 0},
		{0xa8, 0x55, 0}, {0xa8, 0xff, 0x55, 0},
		{0xa8, 0x54, 0x55, 0},
		{0xa8, 0x55, 0x55, 0x55, 0x55, 0},
		{0xa8, 0x55, 0x55, 0, 0xa8, 0},
		{0xa8, 0x55, 0x55, 0, 0x55}
	};
	static const unsigned int sizes[] = {2, 2, 2, 2, 2, 3, 4, 4, 6, 6, 5};
	struct ax_core c;
	ax_u8 encoded[66], zero = 0, busy = 0x80;

	for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		c = running();
		assert(!ax_arm(&c, 1, 0, 3600));
		feed(&c, cases[i], sizes[i], 601);
		assert(c.state == AX_POISONED && !c.valid);
		assert(ax_arm(&c, 1, 0, 5000) < 0);
	}
	for (unsigned int i = 0; i < 3; i++) {
		unsigned int size = frame(i == 2 ? &busy : &zero, i ? 1 : 0, encoded);

		c = running();
		assert(!ax_arm(&c, 0, 1, 3600));
		feed(&c, encoded, size, 601);
		assert((c.state == AX_RUNNING && c.rx.done) == (i != 2));
	}
	c = running();
	assert(!ax_arm(&c, 1, 0, 3600));
	assert(ax_receive(&c, 0xa8, 1, 601) < 0);
	c = running();
	assert(!ax_arm(&c, 1, 0, 3600));
	ax_receive(&c, 0xa8, 0, 601);
	assert(!ax_expire(&c, 3599));
	assert(ax_expire(&c, 3600) < 0 && c.state == AX_POISONED);
	c = running();
	assert(!ax_arm(&c, 0, 1, 3600));
	assert(ax_receive(&c, 0xa8, 0, 3600) < 0);
}

static void publication(void)
{
	struct ax_core c = running();
	ax_s64 candidate[AX_SENSORS];

	for (unsigned int i = 0; i < AX_SENSORS; i++)
		candidate[i] = 100 + i;
	assert(!ax_fresh(&c, 600));
	assert(ax_publish(&c, candidate, 600) < 0);
	assert(!ax_arm(&c, 0, 1, 3600));
	ax_receive(&c, 0xa8, 0, 601);
	ax_receive(&c, 0, 0, 602);
	assert(!ax_begin_guard(&c, 603));
	assert(ax_publish(&c, candidate, 1102) < 0 && !c.valid);
	assert(!ax_publish(&c, candidate, 1103));
	assert(c.generation == 1 && !memcmp(c.cache, candidate, sizeof(candidate)));
	assert(ax_fresh(&c, 6102) && !ax_fresh(&c, 6103));
	/* Continuous RX during idle invalidates even a fresh cache. */
	ax_receive(&c, 0, 0, 1200);
	assert(c.state == AX_POISONED && !ax_fresh(&c, 1200));
	assert(ax_start(&c, 1200) < 0);
	ax_stop(&c);
	assert(c.state == AX_STOPPING && !c.valid);
	assert(ax_arm(&c, 1, 0, 9000) < 0);
	assert(ax_publish(&c, candidate, 9000) < 0);
	c.state = AX_QUIESCED;
	ax_stop(&c);
	assert(c.state == AX_QUIESCED);
	/* Late guard bytes cannot be lost in a gap between IN requests. */
	c = running();
	assert(!ax_arm(&c, 0, 1, 3600));
	ax_receive(&c, 0xa8, 0, 601);
	ax_receive(&c, 0, 0, 602);
	assert(!ax_begin_guard(&c, 603));
	ax_receive(&c, 0xa8, 0, 604);
	assert(ax_publish(&c, candidate, 1103) < 0 && !c.valid);
	/* New USB snapshots age from start of acquisition, not publication. */
	c = running();
	assert(!ax_arm(&c, 0, 1, 3600));
	ax_receive(&c, 0xa8, 0, 601);
	ax_receive(&c, 0, 0, 602);
	assert(!ax_begin_guard(&c, 603));
	assert(!ax_publish_at(&c, candidate, 1103, 600));
	assert(ax_fresh(&c, 5599) && !ax_fresh(&c, 5600));
	c.phase = AX_GUARD;
	c.guard_ms = 5600;
	assert(ax_publish_at(&c, candidate, 6100, 1100) < 0);
	assert(c.state == AX_POISONED && !c.valid);
}

static void quarantine(void)
{
	struct ax_core c;

	ax_core_init(&c);
	assert(!ax_start(&c, 100));
	assert(ax_start(&c, 100) < 0);
	ax_receive(&c, 0xff, 0, 550);
	assert(!ax_quarantine(&c, 1049));
	assert(ax_quarantine(&c, 1050) == 1);
	ax_core_init(&c);
	assert(!ax_start(&c, 100));
	for (unsigned int i = 100; i < 5100; i += 100) {
		ax_receive(&c, 0xff, 0, i);
		assert(!ax_quarantine(&c, i));
	}
	assert(ax_quarantine(&c, 5100) < 0 && c.state == AX_POISONED);
	ax_core_init(&c);
	assert(ax_receive(&c, 0, 1, 0) < 0);
}

static void conversion(void)
{
	static const ax_u64 scales[] = {1, 1000, 1000000};
	ax_s64 result;

	for (unsigned int word = 0; word <= 65535; word++) {
		int e = word / 2048;
		int m = word % 2048;

		if (e >= 16)
			e -= 32;
		if (m >= 1024)
			m -= 2048;
		for (unsigned int i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
			ax_s64 numerator = (ax_s64)m * scales[i];
			ax_s64 expected;

			if (e >= 0) {
				expected = numerator * ((ax_s64)1 << e);
			} else {
				ax_s64 divisor = (ax_s64)1 << -e;
				ax_s64 adjust = numerator < 0 ? -divisor / 2 : divisor / 2;

				expected = (numerator + adjust) / divisor;
			}
			assert(!ax_linear11(word, scales[i], &result));
			assert(result == expected);
		}
	}
	assert(!ax_linear11(0xf801, 1, &result) && result == 1);
	assert(!ax_linear11(0xffff, 1, &result) && result == -1);
	assert(!ax_linear11(0x8001, 1000, &result) && !result);
	assert(!ax_linear11(0x7bff, 1000000, &result) && result == 33521664000000LL);
	assert(ax_linear11(0x0002, UINT64_MAX, &result) < 0);
	assert(ax_linear11(0x7801, (ax_u64)1 << 60, &result) < 0);
	assert(!ax_linear11(0, UINT64_MAX, &result) && !result);
}

static void usb_transport(void)
{
	struct ax_core c = running();
	unsigned int offset = 0;
	ax_u8 command[] = {8, 7, 2}, encoded[66], received[66], data[] = {0x23, 0xf8};
	unsigned int size = ax_encode(command, sizeof(command), encoded, sizeof(encoded));
	unsigned int n = frame(data, sizeof(data), received);
	ax_u64 now = 600;

	assert(!ax_arm(&c, 2, 0, 3600));
	/* Reply arrives before OUT completion. A short completion must not
	 * re-arm/reset RX or discard the unsent suffix of the same frame.
	 */
	assert(!ax_usb_tx_advance(size, &offset, 2, 0, ++now, 3600));
	feed(&c, received, n, ++now);
	assert(c.rx.done);
	while (offset < size) {
		unsigned int remaining = size - offset;
		unsigned int accepted = remaining < 2 ? remaining : 2;

		assert(!ax_usb_tx_advance(size, &offset, accepted, 0, ++now, 3600));
	}
	assert(c.rx.data[0] == data[0] && c.rx.data[1] == data[1]);
	assert(!c.valid); /* success is not publication */
	offset = 0;
	assert(ax_usb_tx_advance(size, &offset, 0, 0, now, 3600) == -EIO);
	assert(ax_usb_tx_advance(size, &offset, size + 1, 0, now, 3600) == -EIO);
	assert(ax_usb_tx_advance(size, &offset, 1, -ENOENT, now, 3600) == -ENOENT);
	assert(ax_usb_tx_advance(size, &offset, 1, 0, 3600, 3600) == -ETIMEDOUT);
	assert(!offset);
}

int main(void)
{
	codec();
	malformed();
	publication();
	quarantine();
	conversion();
	usb_transport();
	puts("PASS: 256 vectors/fragmentation; 196608 LINEAR11; malformed/deadline; "
	     "guard/idle/cache/stop; early USB RX + short/zero/failed TX");
	return 0;
}
