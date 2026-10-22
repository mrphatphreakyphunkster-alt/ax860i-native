// SPDX-License-Identifier: GPL-2.0-only
/* Abstract URB/cancel model, NOT a Linux USB execution test. It exercises the
 * shared state transitions while callback/registration threads race stop.
 * Python additionally checks the real driver's structural drain ordering.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "ax860i_core.h"

struct model {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	struct ax_core core;
	unsigned int callbacks_entered;
	unsigned int callbacks_drained;
	unsigned int attempted_resubmits;
	int release_callbacks;
	int register_entered;
	int release_register;
	int hwmon;
	int worker_drained;
};

static void *callback(void *arg)
{
	struct model *m = arg;

	pthread_mutex_lock(&m->lock);
	m->callbacks_entered++;
	pthread_cond_broadcast(&m->changed);
	while (!m->release_callbacks)
		pthread_cond_wait(&m->changed, &m->lock);
	/* Exactly the gate used by RX resubmit/TX submit/work requeue. */
	if (m->core.state == AX_RUNNING)
		m->attempted_resubmits++;
	else
		assert(ax_receive(&m->core, 0xa8, 0, 1000) < 0);
	m->callbacks_drained++;
	pthread_mutex_unlock(&m->lock);
	return 0;
}

static void *registration(void *arg)
{
	struct model *m = arg;

	pthread_mutex_lock(&m->lock);
	m->register_entered = 1;
	pthread_cond_broadcast(&m->changed);
	while (!m->release_register)
		pthread_cond_wait(&m->changed, &m->lock);
	/* Retain a successful result even if stop happened during register. */
	m->hwmon = 1;
	m->worker_drained = 1;
	pthread_mutex_unlock(&m->lock);
	return 0;
}

static void trial(void)
{
	struct model m = {0};
	pthread_t rx, tx, worker;

	assert(!pthread_mutex_init(&m.lock, 0));
	assert(!pthread_cond_init(&m.changed, 0));
	ax_core_init(&m.core);
	assert(!ax_start(&m.core, 0));
	m.core.valid = 1;
	assert(!pthread_create(&rx, 0, callback, &m));
	assert(!pthread_create(&tx, 0, callback, &m));
	assert(!pthread_create(&worker, 0, registration, &m));
	pthread_mutex_lock(&m.lock);
	while (m.callbacks_entered != 2 || !m.register_entered)
		pthread_cond_wait(&m.changed, &m.lock);
	ax_stop(&m.core);
	assert(!m.core.valid && m.core.state == AX_STOPPING);
	assert(ax_arm(&m.core, 1, 0, 2000) < 0);
	m.release_callbacks = 1; /* model poison/drain both URBs first */
	pthread_cond_broadcast(&m.changed);
	pthread_mutex_unlock(&m.lock);
	assert(!pthread_join(rx, 0));
	assert(!pthread_join(tx, 0));
	pthread_mutex_lock(&m.lock);
	assert(m.callbacks_drained == 2 && !m.attempted_resubmits);
	m.release_register = 1;
	pthread_cond_broadcast(&m.changed);
	pthread_mutex_unlock(&m.lock);
	assert(!pthread_join(worker, 0)); /* cancel_delayed_work_sync model */
	assert(m.worker_drained && m.hwmon);
	m.hwmon = 0; /* unregister only after the racing result is retained */
	m.core.state = AX_QUIESCED;
	ax_stop(&m.core);
	assert(m.core.state == AX_QUIESCED);
	assert(!m.hwmon && m.callbacks_drained == 2 && m.worker_drained);
	assert(!pthread_cond_destroy(&m.changed));
	assert(!pthread_mutex_destroy(&m.lock));
}

int main(void)
{
	for (unsigned int i = 0; i < 100; i++)
		trial();
	puts("PASS: 100 abstract callback/registration/stop races; "
	     "no requeue; retain/drain/unregister ordering (not kernel runtime)");
	return 0;
}
