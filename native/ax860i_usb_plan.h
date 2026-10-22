/* SPDX-License-Identifier: GPL-2.0-only */
/* UART request profile informed by Linux v7.0 drivers/usb/serial/cp210x.c:
 * Copyright (C) 2005 Craig Shelley (craig@microtron.org.uk)
 * Copyright (C) 2010-2021 Johan Hovold (johan@kernel.org)
 * Register definitions and flow-control masks adapted from that GPL-2.0 source.
 * This allocation-free plan/validation implementation is independently written.
 */
#ifndef AX860I_USB_PLAN_H
#define AX860I_USB_PLAN_H

#include "ax860i_core.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define AX_USB_RX_SIZE 64
#define AX_USB_TX_SIZE (2 * AX_RX_MAX + 2)
#define AX_USB_CONTROL_MS 1000

struct ax_usb_endpoint {
	unsigned int address;
	unsigned int attributes;
	unsigned int max_packet;
};

static inline int ax_usb_identity(unsigned int vid, unsigned int pid,
		unsigned int interface, unsigned int alt, unsigned int klass,
		unsigned int subclass, unsigned int protocol,
		const struct ax_usb_endpoint *ep, unsigned int count)
{
	unsigned int i, seen = 0;

	if (vid != 0x1b1c || pid != 0x1c00 || interface || alt ||
	    klass != 0xff || subclass || protocol || count != 2)
		return -ENODEV;
	for (i = 0; i < count; i++) {
		if (ep[i].attributes != 2 || ep[i].max_packet != 64)
			return -ENODEV;
		if (ep[i].address == 0x81 && !(seen & 1))
			seen |= 1;
		else if (ep[i].address == 0x01 && !(seen & 2))
			seen |= 2;
		else
			return -ENODEV;
	}
	return seen == 3 ? 0 : -ENODEV;
}

static inline unsigned int ax_get_le32(const ax_u8 *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static inline void ax_put_le32(ax_u8 *p, unsigned int value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

/* Preserve unrelated flow fields and XON/XOFF limits, as cp210x does.
 * Inherited byte filtering is NOT safe for our framed binary stream: refuse
 * it rather than guess whether error/null/break replacement may be changed.
 */
static inline int ax_usb_raw_flow(ax_u8 *flow)
{
	unsigned int handshake = ax_get_le32(flow);
	unsigned int replace = ax_get_le32(flow + 4);

	if (replace & 0x1c)
		return -EOPNOTSUPP;
	handshake &= ~(0x03U | 0x08U | 0x10U | 0x20U | 0x40U);
	handshake |= 0x01; /* active DTR, no CTS/DSR/DCD handshake/sensitivity */
	replace &= ~(0xc0U | 0x03U);
	replace |= 0x40; /* active RTS, no automatic XON/XOFF */
	ax_put_le32(flow, handshake);
	ax_put_le32(flow + 4, replace);
	return 0;
}

struct ax_usb_request {
	ax_u8 type;
	ax_u8 request;
	ax_u16 value;
	ax_u16 index;
	ax_u16 size;
};

enum ax_usb_setup_step {
	AX_USB_PART, AX_USB_DISABLE, AX_USB_ENABLE, AX_USB_BAUD,
	AX_USB_LINE, AX_USB_GET_FLOW, AX_USB_SET_FLOW, AX_USB_MHS,
	AX_USB_SETUP_STEPS
};

/* transfer returns actual byte count or a negative errno, never boolean ACK. */
typedef int (*ax_usb_transfer)(void *context, const struct ax_usb_request *r,
			       ax_u8 *data);

static inline int ax_usb_exact(int result, unsigned int expected)
{
	if (result < 0)
		return result;
	return (unsigned int)result == expected ? 0 : -EREMOTEIO;
}

/* Worker-owned until teardown has synchronously drained that worker. */
struct ax_usb_uart {
	unsigned int enabled;
	unsigned int disable_pending;
};

static inline int ax_usb_is_enable(const struct ax_usb_request *r)
{
	return r->type == 0x41 && !r->request && r->value == 1 &&
	       !r->index && !r->size;
}

/* Call only AFTER control admission and immediately before the real API.
 * A failed enable can still have reached the bridge: disable once even when
 * its completion status is ambiguous. Nothing before enable arms cleanup.
 */
static inline void ax_usb_uart_begin(struct ax_usb_uart *uart,
				     const struct ax_usb_request *r)
{
	if (ax_usb_is_enable(r))
		uart->disable_pending = 1;
}

static inline void ax_usb_uart_complete(struct ax_usb_uart *uart,
			const struct ax_usb_request *r, int status)
{
	if (ax_usb_is_enable(r) && !status)
		uart->enabled = 1;
}

/* One bounded attempt, including after an ambiguous enable. Consume before
 * calling out: a failed disable is not retried by later disconnect cleanup.
 * The caller serializes this worker-only path with teardown via cancel-sync.
 */
static inline int ax_usb_disable_once(struct ax_usb_uart *uart,
				      ax_usb_transfer transfer, void *context)
{
	const struct ax_usb_request disable = { .type = 0x41 };
	int ret;

	if (!uart->disable_pending)
		return 0;
	uart->disable_pending = 0;
	ret = ax_usb_exact(transfer(context, &disable, NULL), 0);
	if (!ret)
		uart->enabled = 0;
	return ret;
}

static inline int ax_usb_finish_initialization(struct ax_usb_uart *uart,
		int original_error, ax_usb_transfer disable, void *context)
{
	if (original_error)
		ax_usb_disable_once(uart, disable, context);
	/* Cleanup failure must never replace the initialization/model errno. */
	return original_error;
}

static inline int ax_usb_model_result(const ax_u8 *model, unsigned int length)
{
	return length == 7 && !memcmp(model, "AX860i\0", 7) ? 0 : -ENODEV;
}

/* Same completion accounting for real URBs and the offline fake transport. */
static inline int ax_usb_tx_advance(unsigned int total, unsigned int *offset,
		unsigned int actual, int status, ax_u64 now, ax_u64 deadline)
{
	if (status)
		return status;
	if (now >= deadline)
		return -ETIMEDOUT;
	if (!actual || *offset > total || actual > total - *offset)
		return -EIO;
	*offset += actual;
	return 0;
}

/* No hardware-independent path exposes arbitrary control or PMBus writes. */
static inline int ax_usb_configure(ax_usb_transfer transfer, void *context)
{
	struct ax_usb_request r;
	ax_u8 data[16] = { 0 };
	ax_u8 flow[16] = { 0 };
	unsigned int step;
	int ret;

	for (step = 0; step < AX_USB_SETUP_STEPS; step++) {
		memset(&r, 0, sizeof(r));
		r.type = 0x41;
		switch (step) {
		case AX_USB_PART:
			r.type = 0xc0;
			r.request = 0xff;
			r.value = 0x370b;
			r.size = 1;
			break;
		case AX_USB_DISABLE:
			/* IFC_DISABLE resets event insertion without unsupported 0x15. */
			r.request = 0;
			break;
		case AX_USB_ENABLE:
			r.request = 0;
			r.value = 1;
			break;
		case AX_USB_BAUD:
			r.request = 0x1e;
			r.size = 4;
			ax_put_le32(data, 115200);
			break;
		case AX_USB_LINE:
			r.request = 3;
			r.value = 0x0800;
			break;
		case AX_USB_GET_FLOW:
			r.type = 0xc1;
			r.request = 0x14;
			r.size = 16;
			break;
		case AX_USB_SET_FLOW:
			r.request = 0x13;
			r.size = 16;
			memcpy(data, flow, sizeof(flow));
			ret = ax_usb_raw_flow(data);
			if (ret)
				return ret;
			break;
		case AX_USB_MHS:
			r.request = 7;
			r.value = 0x0303; /* DTR/RTS active + both write-enable bits */
			break;
		default:
			return -EINVAL;
		}
		ret = ax_usb_exact(transfer(context, &r, data), r.size);
		if (ret)
			return ret;
		if (step == AX_USB_PART && data[0] != 2 && data[0] != 3)
			/* Avoid CP2101 baud limitations / newer-part flow errata. */
			return -ENODEV;
		if (step == AX_USB_GET_FLOW)
			memcpy(flow, data, sizeof(flow));
	}
	return 0;
}

#endif /* AX860I_USB_PLAN_H */
