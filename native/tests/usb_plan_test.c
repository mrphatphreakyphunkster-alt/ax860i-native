// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "ax860i_usb_plan.h"

struct fake {
	unsigned int step;
	int bad_step;
	int short_step;
	ax_u8 part;
	ax_u8 flow[16];
	struct ax_usb_uart uart;
	unsigned int events[16];
	unsigned int event_count;
	unsigned int disables;
	int disable_error;
	int reject_enable;
};

static int transfer(void *context, const struct ax_usb_request *r, ax_u8 *data)
{
	struct fake *f = context;
	unsigned int step = f->step++;
	int result;
	static const unsigned int requests[] = {0xff, 0, 0, 0x1e, 3, 0x14, 0x13, 7};
	static const unsigned int types[] = {0xc0, 0x41, 0x41, 0x41, 0x41, 0xc1, 0x41, 0x41};
	static const unsigned int values[] = {0x370b, 0, 1, 0, 0x0800, 0, 0, 0x0303};
	static const unsigned int sizes[] = {1, 0, 0, 4, 0, 16, 16, 0};

	assert(step < AX_USB_SETUP_STEPS);
	assert(r->request == requests[step] && r->type == types[step]);
	assert(r->value == values[step] && r->size == sizes[step] && !r->index);
	/* Model the real adapter's terminal-state admission before the API. */
	if (step == AX_USB_ENABLE && f->reject_enable)
		return -ENODEV;
	f->events[f->event_count++] = step;
	ax_usb_uart_begin(&f->uart, r);
	if ((int)step == f->bad_step) {
		result = -EPIPE;
		goto complete;
	}
	if ((int)step == f->short_step) {
		result = r->size ? r->size - 1 : 1;
		goto complete;
	}
	if (step == AX_USB_PART)
		data[0] = f->part;
	if (step == AX_USB_BAUD)
		assert(ax_get_le32(data) == 115200 && data[0] == 0 && data[1] == 0xc2);
	if (step == AX_USB_GET_FLOW)
		memcpy(data, f->flow, 16);
	if (step == AX_USB_SET_FLOW) {
		unsigned int handshake = ax_get_le32(f->flow);
		unsigned int replace = ax_get_le32(f->flow + 4);

		assert(ax_get_le32(data) == ((handshake & ~0x7bU) | 1));
		assert(ax_get_le32(data + 4) == ((replace & ~0xc3U) | 0x40));
		assert(!memcmp(data + 8, f->flow + 8, 8));
	}
	result = r->size;
complete:
	ax_usb_uart_complete(&f->uart, r, ax_usb_exact(result, r->size));
	return result;
}

static int disable(void *context, const struct ax_usb_request *r, ax_u8 *data)
{
	struct fake *f = context;
	unsigned int enable_seen = 0;

	assert(r->type == 0x41 && !r->request && !r->value && !r->index && !r->size);
	assert(!data && !f->uart.disable_pending);
	assert(AX_USB_CONTROL_MS == 1000);
	for (unsigned int i = 0; i < f->event_count; i++)
		enable_seen |= f->events[i] == AX_USB_ENABLE;
	assert(enable_seen); /* no speculative disable before an enable API call */
	f->events[f->event_count++] = AX_USB_SETUP_STEPS;
	f->disables++;
	return f->disable_error;
}

static struct fake fixture(void)
{
	struct fake f = {.bad_step = -1, .short_step = -1, .part = 2};

	ax_put_le32(f.flow, 0xa5a5007b);
	ax_put_le32(f.flow + 4, 0x800001c3);
	ax_put_le32(f.flow + 8, 0x12345678);
	ax_put_le32(f.flow + 12, 0xfedcba98);
	return f;
}

static void configuration(void)
{
	struct fake f = fixture();

	assert(!ax_usb_configure(transfer, &f) && f.step == AX_USB_SETUP_STEPS);
	f = fixture();
	f.part = 3;
	assert(!ax_usb_configure(transfer, &f));
	for (int step = 0; step < AX_USB_SETUP_STEPS; step++) {
		f = fixture();
		f.bad_step = step;
		assert(ax_usb_configure(transfer, &f) == -EPIPE);
		assert(f.step == (unsigned int)step + 1);
		f = fixture();
		f.short_step = step;
		assert(ax_usb_configure(transfer, &f) == -EREMOTEIO);
		assert(f.step == (unsigned int)step + 1);
	}
	for (unsigned int part = 0; part <= 255; part++) {
		if (part == 2 || part == 3)
			continue;
		f = fixture();
		f.part = part;
		assert(ax_usb_configure(transfer, &f) == -ENODEV && f.step == 1);
	}
	for (unsigned int bit = 4; bit <= 16; bit <<= 1) {
		f = fixture();
		f.flow[4] |= bit;
		assert(ax_usb_configure(transfer, &f) == -EOPNOTSUPP);
		assert(f.step == AX_USB_SET_FLOW); /* no unsupported SET_FLOW sent */
	}
	assert(ax_usb_exact(-EIO, 16) == -EIO);
	assert(ax_usb_exact(15, 16) == -EREMOTEIO);
	assert(ax_usb_exact(17, 16) == -EREMOTEIO);
}

static void identity(void)
{
	struct ax_usb_endpoint ep[] = {{0x81, 2, 64}, {0x01, 2, 64}};

	assert(!ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
	assert(ax_usb_identity(0x10c4, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0xea60, 0, 0, 255, 0, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 1, 0, 255, 0, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 1, 255, 0, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 254, 0, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 1, 0, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 1, ep, 2));
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 1));
	for (unsigned int i = 0; i < 2; i++) {
		struct ax_usb_endpoint saved = ep[i];

		ep[i].address = 0x82;
		assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
		ep[i] = saved;
		ep[i].max_packet = 32;
		assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
		ep[i] = saved;
		ep[i].attributes = 3;
		assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
		ep[i] = saved;
	}
	ep[1] = ep[0];
	assert(ax_usb_identity(0x1b1c, 0x1c00, 0, 0, 255, 0, 0, ep, 2));
}

static void initialization_unwind(void)
{
	struct fake f;
	int ret;

	for (int step = 0; step < AX_USB_SETUP_STEPS; step++) {
		for (unsigned int short_reply = 0; short_reply < 2; short_reply++) {
			unsigned int cleanup = step >= AX_USB_ENABLE;
			int expected = short_reply ? -EREMOTEIO : -EPIPE;

			f = fixture();
			if (short_reply)
				f.short_step = step;
			else
				f.bad_step = step;
			ret = ax_usb_configure(transfer, &f);
			assert(ret == expected);
			assert(f.uart.enabled == (unsigned int)(step > AX_USB_ENABLE));
			assert(f.uart.disable_pending == cleanup);
			assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == expected);
			assert(f.disables == cleanup && !f.uart.disable_pending);
			assert(!f.uart.enabled);
			assert(f.event_count == (unsigned int)step + 1 + cleanup);
			for (unsigned int i = 0; i <= (unsigned int)step; i++)
				assert(f.events[i] == i);
			if (cleanup)
				assert(f.events[f.event_count - 1] == AX_USB_SETUP_STEPS);
			/* Later disconnect cannot repeat even an ambiguous unwind. */
			assert(!ax_usb_disable_once(&f.uart, disable, &f));
			assert(f.disables == cleanup);
		}
	}
	/* Cleanup errors never overwrite the original setup errno or retry. */
	f = fixture();
	f.bad_step = AX_USB_LINE;
	f.disable_error = -ETIMEDOUT;
	ret = ax_usb_configure(transfer, &f);
	assert(ret == -EPIPE && f.uart.enabled);
	assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == -EPIPE);
	assert(f.disables == 1 && !f.uart.disable_pending && f.uart.enabled);
	assert(!ax_usb_disable_once(&f.uart, disable, &f) && f.disables == 1);
	/* Bad model after successful setup follows the SAME shared unwind. */
	f = fixture();
	assert(!ax_usb_configure(transfer, &f) && f.uart.enabled);
	ret = ax_usb_model_result((const ax_u8 *)"AX1200i", 7);
	assert(ret == -ENODEV);
	assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == -ENODEV);
	assert(f.disables == 1 && !f.uart.enabled);
	assert(f.event_count == AX_USB_SETUP_STEPS + 1);
	assert(f.events[AX_USB_SETUP_STEPS] == AX_USB_SETUP_STEPS);
	assert(ax_usb_model_result((const ax_u8 *)"AX860i", 6) == -ENODEV);
	/* Successful initialization leaves UART on until ordinary disconnect. */
	f = fixture();
	assert(!ax_usb_configure(transfer, &f));
	assert(!ax_usb_model_result((const ax_u8 *)"AX860i\0", 7));
	assert(!ax_usb_finish_initialization(&f.uart, 0, disable, &f));
	assert(!f.disables && f.uart.enabled && f.uart.disable_pending);
	assert(!ax_usb_disable_once(&f.uart, disable, &f));
	assert(f.disables == 1 && !f.uart.enabled);
	/* Unsupported parts and rejected control admission do not arm cleanup. */
	for (unsigned int part = 0; part <= 255; part++) {
		if (part == 2 || part == 3)
			continue;
		f = fixture();
		f.part = part;
		ret = ax_usb_configure(transfer, &f);
		assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == -ENODEV);
		assert(f.event_count == 1 && !f.disables);
	}
	f = fixture();
	f.reject_enable = 1;
	ret = ax_usb_configure(transfer, &f);
	assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == -ENODEV);
	assert(f.event_count == 2 && !f.disables && !f.uart.disable_pending);
	/* A local flow-policy rejection also unwinds after successful enable. */
	f = fixture();
	f.flow[4] |= 4;
	ret = ax_usb_configure(transfer, &f);
	assert(ax_usb_finish_initialization(&f.uart, ret, disable, &f) == -EOPNOTSUPP);
	assert(f.disables == 1 && !f.uart.enabled);
}

int main(void)
{
	configuration();
	identity();
	initialization_unwind();
	puts("PASS: exact USB allowlist; eight-step UART plan; "
	     "short/error controls; 254 rejected parts; preserved flow fields; "
	     "failed-init/model one-shot unwind + original errno");
	return 0;
}
