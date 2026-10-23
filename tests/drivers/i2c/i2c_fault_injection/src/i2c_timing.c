/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "i2c_fault.h"
#include "i2c_timing.h"

static K_THREAD_STACK_DEFINE(worker_stack,
			     CONFIG_I2C_FAULT_TEST_WORKER_STACK_SIZE);
static struct k_thread worker_thread;
static K_SEM_DEFINE(work_ready, 0, 1);
static K_SEM_DEFINE(work_done, 0, 1);

static i2c_fault_job_fn job_fn;
static void *job_arg;
static int job_ret;
static struct i2c_timing_span job_span;
static bool harness_started;
static bool harness_stuck;

void i2c_timing_begin(struct i2c_timing_span *span)
{
	span->start = k_cycle_get_32();
	span->end = span->start;
} /* i2c_timing_begin() */

void i2c_timing_end(struct i2c_timing_span *span)
{
	span->end = k_cycle_get_32();
} /* i2c_timing_end() */

uint32_t i2c_timing_us(const struct i2c_timing_span *span)
{
	uint32_t elapsed = span->end - span->start;

	return (uint32_t)k_cyc_to_us_floor64((uint64_t)elapsed);
} /* i2c_timing_us() */

void i2c_timing_report(const char *what, uint32_t value, const char *unit)
{
	TC_PRINT("  measured %s: %u %s\n", what, value, unit);
} /* i2c_timing_report() */

/*
 * The worker runs one job per work_ready token and never exits, so the cost
 * of thread creation is paid once for the whole suite. A job that hangs
 * leaves this loop parked inside the controller driver, which is exactly the
 * state i2c_timing_run_bounded() reports and refuses to disturb.
 */
static void worker_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		(void)k_sem_take(&work_ready, K_FOREVER);

		i2c_timing_begin(&job_span);
		job_ret = job_fn(job_arg);
		i2c_timing_end(&job_span);

		k_sem_give(&work_done);
	}
} /* worker_entry() */

void i2c_timing_harness_start(void)
{
	int priority;

	if (harness_started) {
		return;
	}

	/*
	 * The worker runs one priority level below the thread that started it,
	 * and always at a preemptible level.
	 *
	 * Equal priority is not enough. CONFIG_ZTEST_THREAD_PRIORITY defaults
	 * to -1, which is cooperative, and two cooperative threads at the same
	 * priority never preempt one another: the worker would run only when
	 * it blocked. Most drivers do block, so the transfer thread yields and
	 * the arrangement happens to work, but a driver that polls the bus
	 * rather than sleeping on it would hold the CPU for the whole
	 * transfer. The conditions that must act while a transfer is in flight
	 * need the test thread to be able to take the CPU back.
	 */
	priority = k_thread_priority_get(k_current_get()) + 1;
	if (priority < 0) {
		priority = 0;
	}

	(void)k_thread_create(&worker_thread, worker_stack,
			      K_THREAD_STACK_SIZEOF(worker_stack),
			      worker_entry, NULL, NULL, NULL, priority, 0,
			      K_NO_WAIT);
	(void)k_thread_name_set(&worker_thread, "i2c_fault_worker");

	harness_started = true;
} /* i2c_timing_harness_start() */

int i2c_timing_submit(i2c_fault_job_fn fn, void *arg)
{
	if (harness_stuck) {
		return -ETIMEDOUT;
	}

	job_fn = fn;
	job_arg = arg;
	k_sem_give(&work_ready);

	return 0;
} /* i2c_timing_submit() */

int i2c_timing_collect(struct i2c_fault_outcome *out)
{
	uint32_t bound_ms = (uint32_t)CONFIG_I2C_FAULT_TEST_TIMEOUT_MS;

	out->ret = 0;
	out->elapsed_us = 0U;
	out->start_cycles = 0U;
	out->end_cycles = 0U;
	out->timed_out = false;

	if (harness_stuck || (k_sem_take(&work_done, K_MSEC(bound_ms)) != 0)) {
		harness_stuck = true;
		out->timed_out = true;
		out->elapsed_us = bound_ms * USEC_PER_MSEC;
		return -ETIMEDOUT;
	}

	out->ret = job_ret;
	out->elapsed_us = i2c_timing_us(&job_span);
	out->start_cycles = job_span.start;
	out->end_cycles = job_span.end;

	return 0;
} /* i2c_timing_collect() */

/*
 * Interval intersection on a counter that wraps.
 *
 * Both intervals are recent and short relative to the counter period, so the
 * unsigned difference from a common origin orders them correctly even across
 * a wrap. Taking the earlier start as that origin keeps both spans positive.
 */
bool i2c_timing_overlapped(uint32_t a_start, uint32_t a_end, uint32_t b_start,
			   uint32_t b_end)
{
	uint32_t origin = ((int32_t)(a_start - b_start) < 0) ? a_start : b_start;
	uint32_t a_from = a_start - origin;
	uint32_t a_to = a_end - origin;
	uint32_t b_from = b_start - origin;
	uint32_t b_to = b_end - origin;

	return (a_from < b_to) && (b_from < a_to);
} /* i2c_timing_overlapped() */

bool i2c_timing_harness_recover(void)
{
	uint32_t bound_ms = (uint32_t)CONFIG_I2C_FAULT_TEST_TIMEOUT_MS;

	if (!harness_stuck) {
		return true;
	}

	if (k_sem_take(&work_done, K_MSEC(bound_ms)) != 0) {
		return false;
	}

	harness_stuck = false;

	return true;
} /* i2c_timing_harness_recover() */

int i2c_timing_run_bounded(i2c_fault_job_fn fn, void *arg,
			   struct i2c_fault_outcome *out)
{
	int ret = i2c_timing_submit(fn, arg);

	if (ret != 0) {
		out->ret = 0;
		out->elapsed_us =
			(uint32_t)CONFIG_I2C_FAULT_TEST_TIMEOUT_MS *
			USEC_PER_MSEC;
		out->start_cycles = 0U;
		out->end_cycles = 0U;
		out->timed_out = true;
		return ret;
	}

	return i2c_timing_collect(out);
} /* i2c_timing_run_bounded() */

bool i2c_timing_harness_stuck(void)
{
	return harness_stuck;
} /* i2c_timing_harness_stuck() */

void i2c_timing_run_or_fail(i2c_fault_job_fn fn, void *arg,
			    const char *condition,
			    struct i2c_fault_outcome *out)
{
	(void)i2c_timing_run_bounded(fn, arg, out);

	zassert_false(out->timed_out,
		      "%s: transfer did not return within %d ms; the driver "
		      "hung rather than reporting an error",
		      condition, CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);
} /* i2c_timing_run_or_fail() */
