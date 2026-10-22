// SPDX-License-Identifier: GPL-2.0-only
/* Experimental direct-USB AX860i hwmon driver; no TTY/ldisc dependency.
 * CP210x UART request profile: see ax860i_usb_plan.h and SOURCES.md.
 */
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "ax860i_core.h"
#include "ax860i_usb_plan.h"

struct ax_device {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct device *hwmon;
	struct urb *rx_urb;
	struct urb *tx_urb;
	u8 *rx_buffer;
	u8 *tx_buffer;
	struct workqueue_struct *wq;
	struct delayed_work work;
	spinlock_t state_lock;
	struct mutex teardown;
	wait_queue_head_t wait;
	struct ax_core core;
	int error;
	int tx_status;
	unsigned int tx_actual;
	bool tx_done;
	bool initialized;
	struct ax_usb_uart uart;
	bool quiesced;
};

static u64 ax_now(void)
{
	return ktime_get_boottime_ns() / NSEC_PER_MSEC;
}

/* state_lock held. */
static void ax_fail_locked(struct ax_device *a, int error)
{
	ax_poison(&a->core);
	if (!a->error)
		a->error = error;
}

static int ax_fail(struct ax_device *a, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&a->state_lock, flags);
	ax_fail_locked(a, error);
	spin_unlock_irqrestore(&a->state_lock, flags);
	wake_up_all(&a->wait);
	return error;
}

static int ax_live(struct ax_device *a)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&a->state_lock, flags);
	if (a->core.state != AX_RUNNING)
		ret = a->core.state == AX_POISONED ? -EIO : -ENODEV;
	spin_unlock_irqrestore(&a->state_lock, flags);
	return ret;
}

static bool ax_reply_wake(struct ax_device *a)
{
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&a->state_lock, flags);
	done = a->core.state != AX_RUNNING || a->core.rx.done;
	spin_unlock_irqrestore(&a->state_lock, flags);
	return done;
}

static bool ax_tx_wake(struct ax_device *a)
{
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&a->state_lock, flags);
	done = a->core.state != AX_RUNNING || a->tx_done;
	spin_unlock_irqrestore(&a->state_lock, flags);
	return done;
}

/* The persistent RX URB stays armed through acquire, guard AND idle. */
static void ax_rx_complete(struct urb *urb)
{
	struct ax_device *a = urb->context;
	unsigned long flags;
	unsigned int i;
	int ret;
	u64 now = ax_now();

	spin_lock_irqsave(&a->state_lock, flags);
	if (a->core.state != AX_RUNNING)
		goto out;
	if (urb->status) {
		ax_fail_locked(a, urb->status);
		goto out;
	}
	if (urb->actual_length > AX_USB_RX_SIZE) {
		ax_fail_locked(a, -EOVERFLOW);
		goto out;
	}
	for (i = 0; i < urb->actual_length; i++) {
		if (ax_receive(&a->core, a->rx_buffer[i], 0, now) < 0 &&
		    !a->error)
			a->error = -EPROTO;
	}
	/* Atomic check+submit prevents submissions after the stop transition.
	 * Persistent poison also closes submit-versus-cancel callback races.
	 */
	if (a->core.state == AX_RUNNING) {
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			ax_fail_locked(a, ret);
	}
out:
	spin_unlock_irqrestore(&a->state_lock, flags);
	wake_up_all(&a->wait);
}

static void ax_tx_complete(struct urb *urb)
{
	struct ax_device *a = urb->context;
	unsigned long flags;

	spin_lock_irqsave(&a->state_lock, flags);
	a->tx_status = urb->status;
	a->tx_actual = urb->actual_length;
	a->tx_done = true;
	if (urb->status && a->core.state == AX_RUNNING)
		ax_fail_locked(a, urb->status);
	spin_unlock_irqrestore(&a->state_lock, flags);
	wake_up_all(&a->wait);
}

/* Copying control helpers accept stack data safely, require exact length,
 * and return 0 on success. Adapt to the shared plan's actual-length contract.
 * Each in-flight control and the one-shot init unwind is bounded to 1 second.
 */
static int ax_setup_transfer(void *context, const struct ax_usb_request *r,
			     u8 *data)
{
	struct ax_device *a = context;
	unsigned long flags;
	bool active;
	int ret;

	spin_lock_irqsave(&a->state_lock, flags);
	active = a->core.state == AX_ATTACHED;
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (!active)
		return -ENODEV;
	ax_usb_uart_begin(&a->uart, r);
	if (r->type & USB_DIR_IN) {
		ret = usb_control_msg_recv(a->udev, 0, r->request, r->type,
				r->value, r->index, data, r->size,
				AX_USB_CONTROL_MS, GFP_KERNEL);
	} else {
		ret = usb_control_msg_send(a->udev, 0, r->request, r->type,
				r->value, r->index, data, r->size,
				AX_USB_CONTROL_MS, GFP_KERNEL);
	}
	ax_usb_uart_complete(&a->uart, r, ret);
	return ret ? ret : r->size;
}

/* Deliberately distinct from ATTACHED-only setup admission: a poisoned init
 * still needs exactly one disable. Only the worker calls this until cancel
 * has drained it, then disconnect may consume any remaining pending disable.
 * Never take teardown here: disconnect/PM can hold it while waiting for work.
 */
static int ax_disable_transfer(void *context, const struct ax_usb_request *r,
			       u8 *data)
{
	struct ax_device *a = context;
	int ret;

	if (r->type != 0x41 || r->request || r->value || r->index || r->size)
		return -EINVAL;
	if (a->udev->state == USB_STATE_NOTATTACHED)
		return -ENODEV;
	ret = usb_control_msg_send(a->udev, 0, 0, 0x41, 0, 0,
				   NULL, 0, AX_USB_CONTROL_MS, GFP_KERNEL);
	if (ret)
		dev_warn(&a->intf->dev, "one-shot UART disable failed: %d\n", ret);
	return ret;
}

/* Sole worker owns the fixed, kmalloc-backed DMA-safe TX buffer and URB.
 * Preserve the entire encoded frame across short completions. Arm RX first:
 * the device may answer before USB reports completion of the OUT URB.
 */
static int ax_exchange(struct ax_device *a, const u8 *command, size_t size,
		       u8 *reply, unsigned int length, bool ack, u64 deadline)
{
	u8 encoded[AX_USB_TX_SIZE];
	unsigned int total, offset = 0, actual;
	unsigned long flags;
	int ret;
	u64 now;

	total = ax_encode(command, size, encoded, sizeof(encoded));
	if (!total)
		return ax_fail(a, -EINVAL);
	spin_lock_irqsave(&a->state_lock, flags);
	ret = ax_arm(&a->core, length, ack, deadline);
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (ret)
		return ax_fail(a, -EIO);
	while (offset < total) {
		now = ax_now();
		if (now >= deadline)
			return ax_fail(a, -ETIMEDOUT);
		memcpy(a->tx_buffer, encoded + offset, total - offset);
		a->tx_urb->transfer_buffer_length = total - offset;
		spin_lock_irqsave(&a->state_lock, flags);
		if (a->core.state != AX_RUNNING) {
			spin_unlock_irqrestore(&a->state_lock, flags);
			return -ENODEV;
		}
		if (ax_now() >= deadline) {
			spin_unlock_irqrestore(&a->state_lock, flags);
			return ax_fail(a, -ETIMEDOUT);
		}
		a->tx_done = false;
		ret = usb_submit_urb(a->tx_urb, GFP_ATOMIC);
		spin_unlock_irqrestore(&a->state_lock, flags);
		if (ret)
			return ax_fail(a, ret);
		now = ax_now();
		if (now >= deadline ||
		    !wait_event_timeout(a->wait, ax_tx_wake(a),
					msecs_to_jiffies(deadline - now)))
			ax_fail(a, -ETIMEDOUT);
		/* Drain callback before reading results/reusing buffer, or cancel
		 * immediately on poison, timeout or teardown. No spinlock held.
		 */
		usb_kill_urb(a->tx_urb);
		spin_lock_irqsave(&a->state_lock, flags);
		ret = a->core.state == AX_RUNNING ? a->tx_status : -EIO;
		actual = a->tx_actual;
		spin_unlock_irqrestore(&a->state_lock, flags);
		ret = ax_usb_tx_advance(total, &offset, actual, ret, ax_now(), deadline);
		if (ret)
			return ax_fail(a, ret);
	}
	for (;;) {
		now = ax_now();
		spin_lock_irqsave(&a->state_lock, flags);
		ret = ax_expire(&a->core, now);
		if (!ret && now >= deadline) {
			ax_fail_locked(a, -ETIMEDOUT);
			ret = -ETIMEDOUT;
		}
		if (!ret && a->core.rx.done) {
			if (reply)
				memcpy(reply, a->core.rx.data, length);
			spin_unlock_irqrestore(&a->state_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&a->state_lock, flags);
		if (ret)
			return ax_fail(a, -ETIMEDOUT);
		wait_event_timeout(a->wait, ax_reply_wake(a),
				   msecs_to_jiffies(deadline - now));
	}
}

/* Sensor protocol and sysfs surface: transport-independent, kernel-owned. */
#include "ax860i_sensors.h"

/* Check+queue is atomic with the terminal state transition. */
static void ax_queue(struct ax_device *a, unsigned long delay)
{
	unsigned long flags;

	spin_lock_irqsave(&a->state_lock, flags);
	if (a->core.state == AX_RUNNING)
		queue_delayed_work(a->wq, &a->work, delay);
	spin_unlock_irqrestore(&a->state_lock, flags);
}

static int ax_quiet_start(struct ax_device *a)
{
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&a->state_lock, flags);
		ret = ax_quarantine(&a->core, ax_now());
		spin_unlock_irqrestore(&a->state_lock, flags);
		if (ret)
			return ret > 0 ? 0 : ax_fail(a, -ETIMEDOUT);
		wait_event_timeout(a->wait, ax_live(a), msecs_to_jiffies(10));
	}
}

static int ax_initialize(struct ax_device *a)
{
	static const u8 init[] = { 0x11, 2, 0x64, 0, 0, 0, 0 };
	unsigned long flags;
	u8 model[7];
	int ret;

	ret = ax_usb_configure(ax_setup_transfer, a);
	if (ret)
		return ax_fail(a, ret);
	spin_lock_irqsave(&a->state_lock, flags);
	ret = ax_start(&a->core, ax_now());
	if (!ret)
		ret = usb_submit_urb(a->rx_urb, GFP_ATOMIC);
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (ret)
		return ax_fail(a, ret);
	ret = ax_quiet_start(a);
	if (ret)
		return ret;
	ret = ax_exchange(a, init, sizeof(init), NULL, 0, true,
			  ax_now() + AX_REQUEST_MS);
	if (ret)
		return ret;
	ret = ax_read_register(a, 0x9a, sizeof(model), model,
			       ax_now() + AX_REQUEST_MS);
	if (ret)
		return ret;
	ret = ax_usb_model_result(model, sizeof(model));
	if (ret)
		return ax_fail(a, ret);
	a->initialized = true;
	return 0;
}

static void ax_worker(struct work_struct *work)
{
	struct ax_device *a = container_of(to_delayed_work(work),
					 struct ax_device, work);
	s64 candidate[AX_SENSORS];
	unsigned long flags;
	u64 guard_end, now, sampled_ms;
	int ret;

	if (!a->initialized) {
		ret = ax_initialize(a);
		if (ret) {
			/* ax_initialize already poisoned the session/kept first errno.
			 * Drain callbacks before the bounded disable; concurrent PM or
			 * disconnect poisons URBs and waits for this worker to finish.
			 */
			usb_kill_urb(a->rx_urb);
			usb_kill_urb(a->tx_urb);
			ax_usb_finish_initialization(&a->uart, ret,
						     ax_disable_transfer, a);
			return;
		}
	}
	if (ax_live(a))
		return;
	spin_lock_irqsave(&a->state_lock, flags);
	if (a->core.state == AX_RUNNING)
		a->core.phase = AX_ACQUIRE;
	sampled_ms = ax_now();
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (ax_snapshot(a, candidate))
		return;
	spin_lock_irqsave(&a->state_lock, flags);
	now = ax_now();
	ret = ax_begin_guard(&a->core, now);
	guard_end = now + AX_GUARD_MS;
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (ret)
		return;
	while ((now = ax_now()) < guard_end) {
		if (ax_live(a))
			return;
		wait_event_timeout(a->wait, ax_live(a),
				   msecs_to_jiffies(guard_end - now));
	}
	spin_lock_irqsave(&a->state_lock, flags);
	ret = ax_publish_at(&a->core, candidate, ax_now(), sampled_ms);
	spin_unlock_irqrestore(&a->state_lock, flags);
	if (ret)
		goto failed_publication;
	if (!a->hwmon) {
		/* Retain even a registration racing stop: stop cancels this
		 * worker before unregistering and draining all sysfs callbacks.
		 */
		a->hwmon = hwmon_device_register_with_info(&a->intf->dev,
				"ax860i_native", a, &ax_chip, NULL);
		if (IS_ERR(a->hwmon)) {
			ret = PTR_ERR(a->hwmon);
			a->hwmon = NULL;
			ax_fail(a, ret);
			return;
		}
	}
	ax_queue(a, msecs_to_jiffies(1000));
	return;
failed_publication:
	ax_fail(a, -ETIMEDOUT);
}

/* USB core serializes interface PM/reset/disconnect; the mutex also makes
 * repeated quiescence explicit. PM/reset keep hwmon present with EIO. Only
 * disconnect unregisters it and frees memory. No session ever resurrects.
 */
static void ax_quiesce(struct ax_device *a, bool disconnect)
{
	unsigned long flags;

	mutex_lock(&a->teardown);
	spin_lock_irqsave(&a->state_lock, flags);
	if (disconnect)
		ax_stop(&a->core);
	else
		ax_fail_locked(a, -EHOSTDOWN);
	spin_unlock_irqrestore(&a->state_lock, flags);
	wake_up_all(&a->wait);
	if (!a->quiesced) {
		/* Persistent reject count prevents callback resubmit and worker
		 * submit races, including URBs not currently on a USB anchor.
		 */
		usb_poison_urb(a->rx_urb);
		usb_poison_urb(a->tx_urb);
		cancel_delayed_work_sync(&a->work);
		a->quiesced = true;
	}
	if (disconnect) {
		if (a->hwmon) {
			hwmon_device_unregister(a->hwmon);
			a->hwmon = NULL;
		}
		/* Cancel-sync serializes with failed-init unwind: never double
		 * disable, even if its earlier attempt failed. Unplug skips I/O.
		 */
		ax_usb_disable_once(&a->uart, ax_disable_transfer, a);
		spin_lock_irqsave(&a->state_lock, flags);
		a->core.state = AX_QUIESCED;
		spin_unlock_irqrestore(&a->state_lock, flags);
	}
	mutex_unlock(&a->teardown);
}

static void ax_free(struct ax_device *a)
{
	usb_free_urb(a->rx_urb);
	usb_free_urb(a->tx_urb);
	kfree(a->rx_buffer);
	kfree(a->tx_buffer);
	usb_put_intf(a->intf);
	usb_put_dev(a->udev);
	kfree(a);
}

static int ax_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct usb_interface_descriptor *d = &alt->desc;
	struct ax_usb_endpoint endpoints[2];
	struct ax_device *a;
	unsigned int i;
	int ret;

	if (d->bNumEndpoints != ARRAY_SIZE(endpoints) || intf->num_altsetting != 1)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(endpoints); i++) {
		endpoints[i].address = alt->endpoint[i].desc.bEndpointAddress;
		endpoints[i].attributes = alt->endpoint[i].desc.bmAttributes;
		endpoints[i].max_packet = usb_endpoint_maxp(&alt->endpoint[i].desc);
	}
	ret = ax_usb_identity(le16_to_cpu(udev->descriptor.idVendor),
		le16_to_cpu(udev->descriptor.idProduct), d->bInterfaceNumber,
		d->bAlternateSetting, d->bInterfaceClass, d->bInterfaceSubClass,
		d->bInterfaceProtocol, endpoints, ARRAY_SIZE(endpoints));
	if (ret)
		return ret;
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->udev = usb_get_dev(udev);
	a->intf = usb_get_intf(intf);
	spin_lock_init(&a->state_lock);
	mutex_init(&a->teardown);
	init_waitqueue_head(&a->wait);
	ax_core_init(&a->core);
	INIT_DELAYED_WORK(&a->work, ax_worker);
	a->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
	a->tx_urb = usb_alloc_urb(0, GFP_KERNEL);
	a->rx_buffer = kmalloc(AX_USB_RX_SIZE, GFP_KERNEL);
	a->tx_buffer = kmalloc(AX_USB_TX_SIZE, GFP_KERNEL);
	if (!a->rx_urb || !a->tx_urb || !a->rx_buffer || !a->tx_buffer) {
		ret = -ENOMEM;
		goto free;
	}
	usb_fill_bulk_urb(a->rx_urb, udev, usb_rcvbulkpipe(udev, 1),
		a->rx_buffer, AX_USB_RX_SIZE, ax_rx_complete, a);
	usb_fill_bulk_urb(a->tx_urb, udev, usb_sndbulkpipe(udev, 1),
		a->tx_buffer, AX_USB_TX_SIZE, ax_tx_complete, a);
	a->wq = alloc_ordered_workqueue("ax860i_usb", WQ_MEM_RECLAIM);
	if (!a->wq) {
		ret = -ENOMEM;
		goto free;
	}
	/* All callback state, refs, buffers and URBs exist before publication.
	 * Early work is safe even before probe returns: no TTY open dependency.
	 */
	usb_set_intfdata(intf, a);
	queue_delayed_work(a->wq, &a->work, 0);
	dev_info(&intf->dev, "experimental AX860i USB telemetry; validation pending\n");
	return 0;
free:
	ax_free(a);
	return ret;
}

static void ax_disconnect(struct usb_interface *intf)
{
	struct ax_device *a = usb_get_intfdata(intf);

	ax_quiesce(a, true);
	usb_set_intfdata(intf, NULL);
	destroy_workqueue(a->wq);
	ax_free(a);
}

static int ax_suspend(struct usb_interface *intf, pm_message_t message)
{
	ax_quiesce(usb_get_intfdata(intf), false);
	return 0;
}

static int ax_resume(struct usb_interface *intf)
{
	/* Intentionally poisoned until an explicit manual unbind/rebind. */
	return 0;
}

static int ax_pre_reset(struct usb_interface *intf)
{
	ax_quiesce(usb_get_intfdata(intf), false);
	return 0;
}

static const struct usb_device_id ax_ids[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1b1c, 0x1c00, 0xff, 0, 0) },
	{ }
};
MODULE_DEVICE_TABLE(usb, ax_ids);

static struct usb_driver ax_usb_driver = {
	.name = "ax860i_native",
	.id_table = ax_ids,
	.probe = ax_probe,
	.disconnect = ax_disconnect,
	.suspend = ax_suspend,
	.resume = ax_resume,
	.reset_resume = ax_resume,
	.pre_reset = ax_pre_reset,
	.post_reset = ax_resume,
	.no_dynamic_id = 1,
	.supports_autosuspend = 0,
};
module_usb_driver(ax_usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental read-only AX860i direct-USB hwmon driver");
