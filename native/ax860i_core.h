/* SPDX-License-Identifier: GPL-2.0-only */
/* Independently written, allocation-free protocol and publication rules. */
#ifndef AX860I_CORE_H
#define AX860I_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
typedef u8 ax_u8;
typedef u16 ax_u16;
typedef u64 ax_u64;
typedef s64 ax_s64;
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t ax_u8;
typedef uint16_t ax_u16;
typedef uint64_t ax_u64;
typedef int64_t ax_s64;
#endif

#define AX_RX_MAX 32
#define AX_QUIET_MS 500
#define AX_START_MAX_MS 5000
#define AX_REQUEST_MS 3000
#define AX_GUARD_MS 500
#define AX_STALE_MS 5000
#define AX_SENSORS 15

enum ax_state {
	AX_ATTACHED, AX_RUNNING, AX_POISONED, AX_STOPPING, AX_QUIESCED
};

enum ax_phase {
	AX_QUARANTINE, AX_INIT, AX_ACQUIRE, AX_GUARD, AX_IDLE
};

struct ax_rx {
	ax_u8 data[AX_RX_MAX];
	unsigned int len;
	unsigned int min_len;
	unsigned int max_len;
	unsigned int header;
	unsigned int half;
	unsigned int low;
	unsigned int done;
	unsigned int ack;
};

/* Caller serializes every access, including publication and cache copying. */
struct ax_core {
	enum ax_state state;
	enum ax_phase phase;
	struct ax_rx rx;
	unsigned int expecting;
	unsigned int valid;
	unsigned int generation;
	ax_u64 started_ms;
	ax_u64 last_rx_ms;
	ax_u64 deadline_ms;
	ax_u64 guard_ms;
	ax_u64 published_ms;
	ax_s64 cache[AX_SENSORS];
};

static inline ax_u8 ax_symbol(unsigned int nibble)
{
	unsigned int i, out = 0;

	for (i = 0; i < 4; i++)
		out |= ((nibble & (1U << i)) ? 2U : 1U) << (2 * i);
	return out;
}

static inline int ax_nibble(ax_u8 byte)
{
	unsigned int i, value = 0;

	for (i = 0; i < 4; i++) {
		unsigned int pair = (byte >> (2 * i)) & 3;

		if (pair != 1 && pair != 2)
			return -1;
		if (pair == 2)
			value |= 1U << i;
	}
	return value;
}

static inline unsigned int ax_encode(const ax_u8 *payload, unsigned int n,
				     ax_u8 *out, unsigned int capacity)
{
	unsigned int i;

	if (n > AX_RX_MAX || capacity < n * 2 + 2)
		return 0;
	out[0] = 0x54;
	for (i = 0; i < n; i++) {
		out[1 + 2 * i] = ax_symbol(payload[i] & 15);
		out[2 + 2 * i] = ax_symbol(payload[i] >> 4);
	}
	out[1 + n * 2] = 0;
	return n * 2 + 2;
}

static inline void ax_core_init(struct ax_core *c)
{
	memset(c, 0, sizeof(*c));
	c->state = AX_ATTACHED;
	c->phase = AX_QUARANTINE;
}

static inline void ax_poison(struct ax_core *c)
{
	if (c->state == AX_ATTACHED || c->state == AX_RUNNING)
		c->state = AX_POISONED;
	c->valid = 0;
	c->expecting = 0;
}

static inline int ax_start(struct ax_core *c, ax_u64 now)
{
	if (c->state != AX_ATTACHED)
		return -1;
	c->state = AX_RUNNING;
	c->started_ms = now;
	c->last_rx_ms = now;
	return 0;
}

/* 1 = continuous quiet achieved, 0 = keep waiting, -1 = terminal. */
static inline int ax_quarantine(struct ax_core *c, ax_u64 now)
{
	if (c->state != AX_RUNNING || c->phase != AX_QUARANTINE)
		return -1;
	if (now - c->started_ms >= AX_START_MAX_MS) {
		ax_poison(c);
		return -1;
	}
	if (now - c->last_rx_ms < AX_QUIET_MS)
		return 0;
	c->phase = AX_INIT;
	return 1;
}

static inline int ax_arm(struct ax_core *c, unsigned int length,
			 unsigned int ack, ax_u64 deadline)
{
	if (c->state != AX_RUNNING || c->expecting || length > AX_RX_MAX ||
	    (c->phase != AX_INIT && c->phase != AX_ACQUIRE))
		return -1;
	memset(&c->rx, 0, sizeof(c->rx));
	c->rx.min_len = ack ? 0 : length;
	c->rx.max_len = ack ? 1 : length;
	c->rx.ack = ack;
	c->deadline_ms = deadline;
	c->expecting = 1;
	return 0;
}

/* Exactly one byte consumed. No skipping empty frames or surplus bytes. */
static inline int ax_receive(struct ax_core *c, ax_u8 byte,
			     unsigned int bad_flag, ax_u64 now)
{
	struct ax_rx *r = &c->rx;
	int nibble;

	if (c->state == AX_STOPPING || c->state == AX_QUIESCED ||
	    c->state == AX_POISONED)
		return -1;
	if (bad_flag)
		goto bad;
	if (c->phase == AX_QUARANTINE) {
		c->last_rx_ms = now;
		return 0;
	}
	if (!c->expecting || r->done || now >= c->deadline_ms)
		goto bad;
	if (!r->header) {
		if (byte != 0xa8)
			goto bad;
		r->header = 1;
		return 0;
	}
	if (!byte) {
		if (r->half || r->len < r->min_len ||
		    (r->ack && r->len && r->data[0]))
			goto bad;
		r->done = 1;
		c->expecting = 0;
		return 1;
	}
	nibble = ax_nibble(byte);
	if (nibble < 0 || r->len >= r->max_len)
		goto bad;
	if (!r->half) {
		r->low = nibble;
		r->half = 1;
	} else {
		r->data[r->len++] = r->low | (nibble << 4);
		r->half = 0;
	}
	return 0;
bad:
	ax_poison(c);
	return -1;
}

static inline int ax_expire(struct ax_core *c, ax_u64 now)
{
	if (c->state != AX_RUNNING)
		return -1;
	if (c->expecting && now >= c->deadline_ms) {
		ax_poison(c);
		return -1;
	}
	return 0;
}

static inline int ax_begin_guard(struct ax_core *c, ax_u64 now)
{
	if (c->state != AX_RUNNING || c->expecting || !c->rx.done)
		return -1;
	c->phase = AX_GUARD;
	c->guard_ms = now;
	return 0;
}

static inline int ax_publish_at(struct ax_core *c, const ax_s64 *candidate,
				ax_u64 now, ax_u64 sampled_ms)
{
	if (c->state != AX_RUNNING || c->phase != AX_GUARD ||
	    now - c->guard_ms < AX_GUARD_MS)
		return -1;
	if (sampled_ms > now || now - sampled_ms >= AX_STALE_MS) {
		ax_poison(c);
		return -1;
	}
	memcpy(c->cache, candidate, sizeof(c->cache));
	c->published_ms = sampled_ms;
	c->valid = 1;
	c->generation++;
	c->phase = AX_IDLE;
	return 0;
}

/* Historical callers without acquisition timestamps retain their semantics. */
static inline int ax_publish(struct ax_core *c, const ax_s64 *candidate,
			     ax_u64 now)
{
	return ax_publish_at(c, candidate, now, now);
}

static inline int ax_fresh(const struct ax_core *c, ax_u64 now)
{
	return c->state == AX_RUNNING && c->valid &&
	       now - c->published_ms < AX_STALE_MS;
}

static inline void ax_stop(struct ax_core *c)
{
	if (c->state != AX_QUIESCED)
		c->state = AX_STOPPING;
	c->valid = 0;
	c->expecting = 0;
}

/* Signed LINEAR11, round nearest with exact ties away from zero. */
static inline int ax_linear11(ax_u16 word, ax_u64 scale, ax_s64 *result)
{
	const ax_u64 limit = ((ax_u64)1 << 63) - 1;
	int exponent = word >> 11;
	int mantissa = word & 2047;
	ax_u64 magnitude, divisor, remainder;

	if (exponent & 16)
		exponent -= 32;
	if (mantissa & 1024)
		mantissa -= 2048;
	magnitude = mantissa < 0 ? -mantissa : mantissa;
	if (magnitude && scale > limit / magnitude)
		return -1;
	magnitude *= scale;
	if (exponent >= 0) {
		if (magnitude > (limit >> exponent))
			return -1;
		magnitude <<= exponent;
	} else {
		divisor = (ax_u64)1 << -exponent;
		remainder = magnitude % divisor;
		magnitude /= divisor;
		if (remainder >= divisor / 2)
			magnitude++;
	}
	*result = mantissa < 0 ? -(ax_s64)magnitude : (ax_s64)magnitude;
	return 0;
}

#endif /* AX860I_CORE_H */
