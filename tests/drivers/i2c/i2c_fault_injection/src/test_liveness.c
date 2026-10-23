/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Liveness and concurrency, conditions E1 to E5 of the suite.
 *
 * Bounded failure is the through-line of the whole suite: every faulted
 * transfer in every file runs on the bounded harness, so E1 is present
 * wherever a fault is injected. What is added here is the explicit sweep
 * across fault classes, and the checks that a fault does not leave the
 * driver's own locking or its other buses in a bad state.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "i2c_fault.h"
#include "i2c_timing.h"

#if I2C_FAULT_HAS_TARGET
#include <zephyr/drivers/i2c/target/fault.h>
#endif

#ifdef CONFIG_I2C_CALLBACK
static K_SEM_DEFINE(async_done, 0, 1);
static int async_result;
static struct i2c_msg async_msg;
#endif

/* Addresses sampled on the second bus to show it was left alone. */
#define ADDR_SCAN_BASE    0x08U
#define ADDR_SCAN_SAMPLES 8U

static K_THREAD_STACK_DEFINE(second_stack, 1024);
static struct k_thread second_thread;
static K_SEM_DEFINE(second_done, 0, 1);
static int second_result;

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];

/* Counters for the concurrent probe loops of E6. */
static K_THREAD_STACK_DEFINE(concurrent_stack, 1024);
static struct k_thread concurrent_thread;
static K_SEM_DEFINE(concurrent_done, 0, 1);
static uint32_t concurrent_absent_answered;
static uint32_t concurrent_absent_rounds;

/* Job that writes to the absent address, the cheapest fault available. */
static int absent_write_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	return i2c_write(ctx->dut, tx_buf, sizeof(tx_buf), ctx->absent_addr);
} /* absent_write_job() */

ZTEST_SUITE(i2c_fault_liveness, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief E1 - every faulted transfer returns within a bounded time.
 *
 * @details
 * Stimulus:
 * - A faulted write, a faulted read and a faulted write-read to the absent
 *   address, each on the bounded harness, each timed.
 *
 * On the bus:
 * - Three address phases, each NACKed. The waveforms are almost identical;
 *   what differs is the driver path taken to unwind, which is why all three
 *   shapes are timed rather than one.
 *
 * Expected:
 * - Each returns within CONFIG_I2C_FAULT_TEST_TIMEOUT_MS.
 * - The three durations are printed, so a driver that returns but takes far
 *   longer for one shape than the others is visible in the log even though
 *   the condition passes.
 * - The bus is usable afterwards.
 *
 * Bounded failure is the through-line of the whole suite and every faulted
 * transfer everywhere runs on the same harness. This condition makes it
 * explicit and comparable across the three transfer shapes.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_liveness, test_liveness_bounded_failure)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xE1U);

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = msgs;

	msgs[0].buf = tx_buf;
	msgs[0].len = sizeof(tx_buf);
	msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;
	xfer.num_msgs = 1U;
	i2c_fault_transfer_bounded(&xfer, "E1 faulted write", &outcome);
	i2c_timing_report("E1 faulted write", outcome.elapsed_us, "us");

	msgs[0].buf = tx_buf;
	msgs[0].len = sizeof(tx_buf);
	msgs[0].flags = I2C_MSG_READ | I2C_MSG_STOP;
	xfer.num_msgs = 1U;
	i2c_fault_transfer_bounded(&xfer, "E1 faulted read", &outcome);
	i2c_timing_report("E1 faulted read", outcome.elapsed_us, "us");

	msgs[0].buf = tx_buf;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = tx_buf;
	msgs[1].len = sizeof(tx_buf);
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;
	xfer.num_msgs = ARRAY_SIZE(msgs);
	i2c_fault_transfer_bounded(&xfer, "E1 faulted write-read", &outcome);
	i2c_timing_report("E1 faulted write-read", outcome.elapsed_us, "us");

	i2c_fault_assert_healthy("E1");
} /* test_liveness_bounded_failure */

/* Entry point for the thread that transfers after a fault on another thread. */
static void second_entry(void *p1, void *p2, void *p3)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	second_result = i2c_fault_probe(ctx->dut, ctx->present_addr);
	k_sem_give(&second_done);
} /* second_entry() */

/**
 * @brief E2 - a fault must not leave the driver's bus lock held.
 *
 * @details
 * Stimulus:
 * - Perform a faulted write on the harness worker.
 * - Start a second thread that probes the known-good address, and wait on a
 *   semaphore for it.
 *
 * On the bus:
 * - A NACKed address phase, then an ordinary successful transaction from a
 *   different thread. Nothing unusual happens on the wire; the condition is
 *   about the driver's internal serialisation, not the bus.
 *
 * Expected:
 * - The second thread completes within CONFIG_I2C_FAULT_TEST_TIMEOUT_MS and
 *   its transfer succeeds.
 *
 * Almost every I2C driver takes a mutex or semaphore for the duration of a
 * transfer. An error path that returns without releasing it leaves the bus
 * permanently unavailable to every other caller while looking, from the
 * faulting thread's point of view, like a clean failure. The semaphore
 * timeout distinguishes that from a transfer that merely failed.
 *
 * Requires: one I2C controller and one device that answers. No fixture.
 */
ZTEST(i2c_fault_liveness, test_liveness_no_deadlock)
{
	struct i2c_fault_outcome outcome;
	k_tid_t tid;

	i2c_fault_require_present();

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xE2U);
	i2c_timing_run_or_fail(absent_write_job, NULL, "E2 fault before a "
			       "second thread transfers", &outcome);
	zassert_true(outcome.ret < 0, "E2: the faulted write succeeded");

	k_sem_reset(&second_done);
	tid = k_thread_create(&second_thread, second_stack,
			      K_THREAD_STACK_SIZEOF(second_stack),
			      second_entry, NULL, NULL, NULL,
			      k_thread_priority_get(k_current_get()), 0,
			      K_NO_WAIT);

	zassert_ok(k_sem_take(&second_done,
			      K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS)),
		   "E2: a second thread did not complete a transfer within "
		   "%d ms after a faulted one; the driver appears to have "
		   "kept its bus lock",
		   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);
	(void)k_thread_join(tid, K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS));

	zassert_ok(second_result,
		   "E2: the second thread's transfer failed (%d)",
		   second_result);
} /* test_liveness_no_deadlock */

/* Probe the absent address in a loop, counting any round that answered. */
static void concurrent_entry(void *p1, void *p2, void *p3)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t i = 0U; i < (uint32_t)CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS; i++) {
		if (i2c_fault_probe(ctx->dut, ctx->absent_addr) == 0) {
			concurrent_absent_answered++;
		}

		concurrent_absent_rounds++;
	}

	k_sem_give(&concurrent_done);
} /* concurrent_entry() */

/**
 * @brief E6 - concurrent transfers must not be delivered to each other's
 *        address.
 *
 * @details
 * Stimulus:
 * - Establish sequentially that the known-good address answers a zero-length
 *   probe and the absent address does not.
 * - Then run CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS probes of the absent
 *   address on a second thread while the test thread probes the known-good
 *   address the same number of times.
 *
 * On the bus:
 * - Ordinary address phases throughout, one per probe, serialised by whatever
 *   lock the driver holds. Nothing unusual is asked of the wire; the
 *   condition is about which address each caller's transfer was sent to.
 *
 * Expected:
 * - No round aimed at the known-good address fails.
 * - No round aimed at the absent address succeeds.
 *
 * A driver that stores the target address, or the buffer pointers, in state
 * shared between callers can have one thread overwrite another's before the
 * transfer is issued. That is invisible to a single-threaded test and
 * invisible to a two-controller contention test, because both callers here
 * are on the same controller. It shows up as a probe returning the answer
 * belonging to the other thread's address.
 *
 * The sequential baseline is taken first and the condition skips if it does
 * not hold. Where a controller cannot already distinguish the two addresses
 * one caller at a time, a failure under concurrency would say nothing about
 * concurrency, and reporting it as a race would be wrong.
 *
 * Nothing is asserted about timing, ordering or fairness between the two
 * threads. The API does not describe any of those, and it does not describe
 * concurrent use at all. Only the address each transfer reached is checked,
 * which is the weakest claim that still catches the defect.
 *
 * Requires: one I2C controller and one device that answers. No fixture.
 */
ZTEST(i2c_fault_liveness, test_liveness_concurrent_addressing)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t present_failed = 0U;
	k_tid_t tid;
	int ret;

	i2c_fault_require_present();

	ret = i2c_fault_probe(ctx->dut, ctx->present_addr);
	if (ret != 0) {
		TC_PRINT("  the known-good address 0x%02x did not answer a "
			 "probe on its own (%d), so a concurrent result "
			 "could not be attributed to concurrency\n",
			 ctx->present_addr, ret);
		ztest_test_skip();
		return;
	}

	ret = i2c_fault_probe(ctx->dut, ctx->absent_addr);
	if (ret == 0) {
		TC_PRINT("  the absent address 0x%02x answered a probe on its "
			 "own, so the two addresses are not distinguishable "
			 "one caller at a time\n",
			 ctx->absent_addr);
		ztest_test_skip();
		return;
	}

	concurrent_absent_answered = 0U;
	concurrent_absent_rounds = 0U;
	k_sem_reset(&concurrent_done);

	tid = k_thread_create(&concurrent_thread, concurrent_stack,
			      K_THREAD_STACK_SIZEOF(concurrent_stack),
			      concurrent_entry, NULL, NULL, NULL,
			      k_thread_priority_get(k_current_get()), 0,
			      K_NO_WAIT);

	for (uint32_t i = 0U; i < (uint32_t)CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS; i++) {
		if (i2c_fault_probe(ctx->dut, ctx->present_addr) != 0) {
			present_failed++;
		}
	}

	zassert_ok(k_sem_take(&concurrent_done,
			      K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS)),
		   "E6: the concurrent thread completed %u of %d rounds "
		   "before the bound expired",
		   concurrent_absent_rounds,
		   CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS);
	(void)k_thread_join(tid, K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS));

	TC_PRINT("  E6 %d rounds each: known-good failed %u, absent answered "
		 "%u\n",
		 CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS, present_failed,
		 concurrent_absent_answered);

	zassert_equal(present_failed, 0U,
		      "E6: %u probes of the known-good address 0x%02x failed "
		      "while another thread probed 0x%02x; the two callers "
		      "are not isolated from one another",
		      present_failed, ctx->present_addr, ctx->absent_addr);

	zassert_equal(concurrent_absent_answered, 0U,
		      "E6: %u probes of the absent address 0x%02x reported an "
		      "answer while another thread probed 0x%02x; the two "
		      "callers are not isolated from one another",
		      concurrent_absent_answered, ctx->absent_addr,
		      ctx->present_addr);
} /* test_liveness_concurrent_addressing */

#ifdef CONFIG_I2C_CALLBACK
static void async_cb(const struct device *dev, int result, void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);

	async_result = result;
	k_sem_give(&async_done);
} /* async_cb() */
#endif

/**
 * @brief E3 - a fault met by an asynchronous transfer.
 *
 * @details
 * Stimulus:
 * - Submit a write to the absent address with i2c_transfer_cb() and wait on a
 *   semaphore the completion callback gives.
 *
 * On the bus:
 * - A NACKed address phase, identical to A1. What differs is entirely on the
 *   driver side: the failure is discovered in an interrupt rather than by the
 *   thread that submitted it, and has to be delivered to a callback.
 *
 * Expected:
 * - The submission returns 0, or -ENOSYS on a driver without the API, in
 *   which case the condition skips.
 * - The callback runs within CONFIG_I2C_FAULT_TEST_TIMEOUT_MS and carries a
 *   negative result.
 * - The bus is usable afterwards.
 *
 * The failure this guards against is a driver that abandons the transfer on
 * its error path without invoking the callback, leaving the caller waiting
 * forever with no error to react to. The message descriptor is at file scope
 * because the API permits the transfer to remain pending after the wait gives
 * up.
 *
 * Requires: CONFIG_I2C_CALLBACK and a driver implementing it. No fixture.
 */
ZTEST(i2c_fault_liveness, test_liveness_async_fault)
{
#ifdef CONFIG_I2C_CALLBACK
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xE3U);

	/*
	 * The message descriptor is file scope because the API permits the
	 * transfer to remain pending after the wait below gives up, and the
	 * driver may still be walking it.
	 */
	async_msg.buf = tx_buf;
	async_msg.len = sizeof(tx_buf);
	async_msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	k_sem_reset(&async_done);
	async_result = 0;

	ret = i2c_transfer_cb(ctx->dut, &async_msg, 1U, ctx->absent_addr,
			      async_cb, NULL);
	if (ret == -ENOSYS) {
		TC_PRINT("  the driver implements no asynchronous transfer\n");
		ztest_test_skip();
		return;
	}

	if (ret < 0) {
		i2c_fault_record("E3 asynchronous submission", ret);
		i2c_fault_assert_healthy("E3");
		return;
	}

	zassert_ok(k_sem_take(&async_done,
			      K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS)),
		   "E3: the completion callback did not run within %d ms; "
		   "the transfer is still pending and the run cannot "
		   "continue safely",
		   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);
	i2c_fault_record("E3 asynchronous completion", async_result);
	zassert_true(async_result < 0,
		     "E3: the callback reported success for a transfer to "
		     "absent 0x%02x",
		     ctx->absent_addr);
	i2c_fault_assert_healthy("E3");
#else
	TC_PRINT("  CONFIG_I2C_CALLBACK is not enabled\n");
	ztest_test_skip();
#endif
} /* test_liveness_async_fault */

#if defined(CONFIG_I2C_RTIO) && \
	(I2C_FAULT_HAS_TARGET || (CONFIG_I2C_FAULT_TEST_ABSENT_ADDR != 0))

#include <zephyr/rtio/rtio.h>

#if I2C_FAULT_HAS_TARGET
I2C_DT_IODEV_DEFINE(fault_iodev, I2C_FAULT_TARGET_NODE);
#else
I2C_IODEV_DEFINE(fault_iodev, I2C_FAULT_DUT_NODE,
		 CONFIG_I2C_FAULT_TEST_ABSENT_ADDR);
#endif

RTIO_DEFINE(fault_rtio, 4, 4);
#define I2C_FAULT_RTIO_AVAILABLE 1
#else
#define I2C_FAULT_RTIO_AVAILABLE 0
#endif

/**
 * @brief E4 - a fault met by an RTIO transfer.
 *
 * @details
 * Stimulus:
 * - Where a fault target is present, arm it to refuse every byte and submit
 *   an RTIO write to it. Otherwise submit to a pinned absent address.
 * - Submit with a wait count of zero and poll the completion queue against a
 *   deadline.
 *
 * On the bus:
 * - Either an acknowledged address followed by a NACKed first data byte, or a
 *   NACKed address phase, depending on which form is available. Both are
 *   faults the RTIO path must surface through a completion event.
 *
 * Expected:
 * - A completion is posted within CONFIG_I2C_FAULT_TEST_TIMEOUT_MS and
 *   carries a negative result.
 * - The bus is usable afterwards.
 *
 * The submission deliberately does not wait, because rtio_submit() with a
 * non-zero wait count blocks until that many completions arrive, and a driver
 * that never completes would hang there before the test could apply any
 * deadline of its own.
 *
 * The iodev binds its address at build time, so a runtime-discovered address
 * cannot be used; without a fault target or a pinned
 * CONFIG_I2C_FAULT_TEST_ABSENT_ADDR the condition skips.
 *
 * Requires: CONFIG_I2C_RTIO, and either a fault target or a pinned absent
 * address.
 */
ZTEST(i2c_fault_liveness, test_liveness_rtio_fault)
{
#if I2C_FAULT_RTIO_AVAILABLE
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe = NULL;
	int64_t deadline;
	int result;
	int ret;

#if I2C_FAULT_HAS_TARGET
	struct i2c_target_fault_config fault = {
		.kind = I2C_TARGET_FAULT_NACK_WRITE_DATA,
		.trigger_index = I2C_TARGET_FAULT_TRIGGER_EVERY,
		.repeat_count = I2C_TARGET_FAULT_REPEAT_FOREVER,
		.refuse_errno = -EIO,
	};

	i2c_fault_require_target();
	zassert_ok(i2c_target_fault_arm(i2c_fault_ctx()->target, &fault),
		   "could not arm the fault target");
#endif

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xE4U);

	sqe = rtio_sqe_acquire(&fault_rtio);
	zassert_not_null(sqe, "E4: the SQE pool was exhausted");

	rtio_sqe_prep_write(sqe, &fault_iodev, RTIO_PRIO_NORM, tx_buf,
			    sizeof(tx_buf), NULL);
	sqe->iodev_flags |= RTIO_IODEV_I2C_STOP;

	/*
	 * Submitted without waiting, so that a driver which never completes
	 * is caught by the deadline below rather than blocking inside
	 * rtio_submit().
	 */
	ret = rtio_submit(&fault_rtio, 0);
	zassert_ok(ret, "E4: rtio_submit() failed (%d)", ret);

	deadline = k_uptime_get() + CONFIG_I2C_FAULT_TEST_TIMEOUT_MS;
	do {
		cqe = rtio_cqe_consume(&fault_rtio);
		if (cqe != NULL) {
			break;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	zassert_not_null(cqe,
			 "E4: no completion was posted within %d ms for the "
			 "faulted transfer",
			 CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);
	result = cqe->result;
	rtio_cqe_release(&fault_rtio, cqe);

	i2c_fault_record("E4 RTIO transfer meeting a fault", result);
	zassert_true(result < 0,
		     "E4: the RTIO transfer reported success despite the "
		     "fault");
	i2c_fault_assert_healthy("E4");
#else
	TC_PRINT("  RTIO is disabled, or no fault target and no pinned "
		 "CONFIG_I2C_FAULT_TEST_ABSENT_ADDR to bind an iodev to\n");
	ztest_test_skip();
#endif
} /* test_liveness_rtio_fault */

#define I2C_FAULT_HAS_OTHER_BUS DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_other))

/**
 * @brief E5 - a fault on one bus must not disturb another.
 *
 * @details
 * Stimulus:
 * - Probe a fixed set of addresses on the bus named by the i2c-other alias
 *   and record what each returns.
 * - Provoke a fault on the bus under test.
 * - Probe the same addresses on the other bus again.
 *
 * On the bus:
 * - Two independent buses. Nothing the fault does should be visible on the
 *   second one at all; the probes there are ordinary address phases whose
 *   results are compared before and after.
 *
 * Expected:
 * - Every address on the other bus returns exactly what it returned before.
 *
 * The failure this catches is cross-bus coupling that is invisible on a
 * single bus: a shared clock gate switched off during error handling, a
 * shared interrupt line left masked, or driver state held in a file-scope
 * variable rather than per-instance. All of those pass every other condition
 * in the suite.
 *
 * Requires: a third controller named by the i2c-other alias. No fixture.
 */
ZTEST(i2c_fault_liveness, test_liveness_other_bus_undisturbed)
{
#if I2C_FAULT_HAS_OTHER_BUS
	const struct device *other = DEVICE_DT_GET(DT_ALIAS(i2c_other));
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	int before[ADDR_SCAN_SAMPLES];
	int after;

	zassert_true(device_is_ready(other),
		     "E5: the bus named by the i2c-other alias is not ready");

	if (other == ctx->dut) {
		TC_PRINT("  the i2c-other alias names the bus under test\n");
		ztest_test_skip();
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(before); i++) {
		before[i] = i2c_fault_probe(other,
					    (uint16_t)(ADDR_SCAN_BASE + i));
	}

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xE5U);
	i2c_timing_run_or_fail(absent_write_job, NULL,
			       "E5 fault on the bus under test", &outcome);
	zassert_true(outcome.ret < 0, "E5: the faulted write succeeded");

	for (size_t i = 0U; i < ARRAY_SIZE(before); i++) {
		after = i2c_fault_probe(other,
					(uint16_t)(ADDR_SCAN_BASE + i));
		zassert_equal(after, before[i],
			      "E5: address 0x%02x on the other bus returned "
			      "%d before the fault and %d after",
			      ADDR_SCAN_BASE + i, before[i], after);
	}
#else
	TC_PRINT("  no i2c-other alias; this condition needs a second bus "
		 "that is not the one under test\n");
	ztest_test_skip();
#endif
} /* test_liveness_other_bus_undisturbed */
