/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Arbitration and bus contention, conditions F1 and F2 of the suite.
 *
 * Arbitration loss is tested two ways because neither alone is sufficient.
 * F1a forces the condition by holding SDA low while the controller drives a
 * logical one, which is deterministic but is only one of the ways the
 * condition arises. F1b lets two controllers genuinely contend. A START
 * forced through the line GPIOs holds both at their bus-idle check, so the
 * matching STOP releases them together, and the address each sends decides
 * which one loses.
 *
 * Drivers report arbitration loss inconsistently. The values returned are
 * recorded, and that record is a deliverable in its own right: it is the
 * evidence a future discussion about standardising I2C error codes would
 * need.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "bus_force.h"
#include "i2c_fault.h"
#include "i2c_timing.h"

#if I2C_FAULT_HAS_TARGET
#include <zephyr/drivers/i2c/target/fault.h>

static uint8_t contender_byte = 0xFFU;
#endif

#define HAS_GATED_CONTENTION (I2C_FAULT_HAS_TARGET && I2C_FAULT_HAS_CONTENDER)

#if HAS_GATED_CONTENTION
static K_THREAD_STACK_DEFINE(contender_stack, 1024);
static struct k_thread contender_thread;
static K_SEM_DEFINE(contender_done, 0, 1);
static int contender_result;

/* Offset byte, then a marker that changes every round. */
static uint8_t gated_write[2];
#endif

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];

/* Job that writes an all-ones payload, so every data bit is a driven one. */
static int ones_write_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	(void)memset(tx_buf, 0xFFU, sizeof(tx_buf));

	return i2c_write(ctx->dut, tx_buf, sizeof(tx_buf), ctx->present_addr);
} /* ones_write_job() */

ZTEST_SUITE(i2c_fault_arbitration, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief F1a - SDA held low for the whole attempt.
 *
 * @details
 * Stimulus:
 * - Pull SDA low, submit a write of all-ones to the known-good address on the
 *   bounded harness, collect, then release.
 *
 * On the bus:
 * - SDA is low before the controller begins, so it cannot form a START. The
 *   payload is 0xFF throughout so that every data bit the controller would
 *   drive is a one; a controller that drives a one and reads back the zero a
 *   held line presents has, by definition, lost arbitration. Whether it gets
 *   far enough to notice depends on whether it checks the bus is free first.
 *
 * Expected:
 * - The transfer fails, within the time bound.
 * - The value returned is recorded, not asserted. Drivers report this as
 *   arbitration loss, as a generic bus error, or as a timeout, and the API
 *   mandates none of them. That record is a deliverable in its own right: it
 *   is the evidence a discussion about standardising I2C error codes needs.
 * - The line is released and the worker given a further bound before the
 *   assertion.
 *
 * D9 provides the mid-data-phase variant of this stimulus, where the line
 * goes low after the transaction is already running.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_arbitration, test_arbitration_forced)
{
	struct i2c_fault_outcome outcome;
	bool recovered;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	zassert_ok(bus_force_low(BUS_FORCE_SDA), "could not hold SDA low");
	zassert_ok(i2c_timing_submit(ones_write_job, NULL),
		   "could not submit the transfer");

	(void)i2c_timing_collect(&outcome);

	zassert_ok(bus_force_release(), "could not release SDA");
	recovered = i2c_timing_harness_recover();

	zassert_false(outcome.timed_out,
		      "F1a: the transfer had not returned after %d ms and %s "
		      "once SDA was released",
		      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
		      recovered ? "only returned" : "never returned");

	i2c_fault_record("F1a forced arbitration loss", outcome.ret);
	zassert_true(outcome.ret < 0,
		     "F1a: the write succeeded although SDA was held low "
		     "throughout");
	i2c_fault_assert_healthy("F1a");
} /* test_arbitration_forced */

#if HAS_GATED_CONTENTION
/*
 * Probe the contender device from the second controller. Started while the
 * gate holds the bus busy, so the probe waits at the controller's bus-idle
 * check until the gate opens.
 */
static void contender_entry(void *p1, void *p2, void *p3)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	contender_result = i2c_fault_probe(ctx->target_bus,
					   I2C_FAULT_CONTENDER_ADDR);
	k_sem_give(&contender_done);
} /* contender_entry() */

/*
 * How long the gate holds the bus busy. It only has to outlast both threads
 * reaching their controller's bus-idle wait, and stays far inside any
 * driver's transfer timeout.
 */
#define CONTENTION_GATE_MS 2
#endif /* HAS_GATED_CONTENTION */

/**
 * @brief F1b - two controllers released onto the bus by the same STOP.
 *
 * @details
 * Stimulus, repeated CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS times:
 * - Pull SDA low through the forcing GPIO while SCL is high. Every controller
 *   on the wire sees a START and treats the bus as busy.
 * - Submit a write to the fault target on the controller under test, and
 *   start a thread that probes the contender device from the second
 *   controller. Both wait for the bus to go idle.
 * - Release SDA. The STOP frees the bus for both at once.
 *
 * On the bus:
 * - Both controllers drive a START and their address. The contender device
 *   sits below the fault target, so at the first differing address bit the
 *   controller under test drives a one, reads back the zero the second
 *   controller drives, and has lost arbitration.
 *
 * Expected:
 * - The controller under test fails its transfer and returns, within the
 *   time bound. The value it returns is recorded, not asserted, as for F1a.
 * - The second controller completes its probe on every round. The loser
 *   must withdraw from the bus without disturbing the winner.
 * - A round where both succeed ran one after the other and produced no
 *   arbitration. If the controller under test reports success, the fault
 *   target must hold the marker it wrote.
 * - The bus is usable afterwards. If no round lost arbitration the
 *   condition skips instead, since no contention was produced.
 *
 * Catches a controller that reports success after losing arbitration, one
 * that keeps driving the bus after losing and corrupts the winner's
 * transfer, and one that never returns once it has lost.
 *
 * Requires: the forcing GPIOs, a fault target on a second controller that
 * answers the controller under test, and the i2c-fault-contender alias.
 * Fixtures: i2c_bus_fault and i2c_bus_short together.
 */
ZTEST(i2c_fault_arbitration, test_arbitration_contention)
{
	i2c_fault_require_bus_force();
	i2c_fault_require_target_reachable();

#if HAS_GATED_CONTENTION
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	uint32_t lost = 0U;
	uint32_t serialised = 0U;
	uint32_t early = 0U;
	int early_ret = 0;
	bool contender_early;
	bool can_verify;
	uint32_t gate_open;
	uint8_t stored;
	k_tid_t tid;
	int ret;

	if (!ctx->target_is_second_controller) {
		TC_PRINT("  the fault target shares the controller under "
			 "test, so there is no second controller to contend "
			 "with\n");
		ztest_test_skip();
		return;
	}

	if (I2C_FAULT_CONTENDER_ADDR >= ctx->target_addr) {
		TC_PRINT("  the contender device at 0x%02x is not below the "
			 "fault target at 0x%02x, so the controller under test "
			 "would win arbitration rather than lose it\n",
			 I2C_FAULT_CONTENDER_ADDR, ctx->target_addr);
		ztest_test_skip();
		return;
	}

	/* The second controller must reach its device unopposed. */
	ret = i2c_fault_probe(ctx->target_bus, I2C_FAULT_CONTENDER_ADDR);
	if (ret != 0) {
		i2c_fault_record("F1b second controller unopposed", ret);
		TC_PRINT("  the second controller cannot address 0x%02x on "
			 "its own, so it cannot contend\n",
			 I2C_FAULT_CONTENDER_ADDR);
		ztest_test_skip();
		return;
	}

	msg.buf = gated_write;
	msg.len = sizeof(gated_write);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;
	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	/* Uncontended write, which also shows whether delivery is checkable. */
	gated_write[0] = 0U;
	gated_write[1] = 0xB1U;
	stored = (uint8_t)~gated_write[1];
	(void)i2c_target_fault_set_data(ctx->target, &stored, 1U);
	i2c_fault_transfer_bounded(&xfer, "F1b uncontended write", &outcome);
	zassert_ok(outcome.ret,
		   "F1b: the uncontended write to the fault target failed (%d)",
		   outcome.ret);
	(void)i2c_target_fault_get_data(ctx->target, &stored, 1U);
	can_verify = (stored == gated_write[1]);
	if (!can_verify) {
		TC_PRINT("  F1b: the fault target did not store the "
			 "uncontended write, so a success from the controller "
			 "under test cannot be checked for delivery\n");
	}
	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS;
	     i++) {
		gated_write[1] = (uint8_t)i;
		stored = (uint8_t)~gated_write[1];
		(void)i2c_target_fault_set_data(ctx->target, &stored, 1U);
		k_sem_reset(&contender_done);
		contender_result = 0;

		/* Close the gate before either controller is handed work. */
		zassert_ok(bus_force_low(BUS_FORCE_SDA),
			   "could not hold SDA low");

		ret = i2c_fault_transfer_submit(&xfer);
		if (ret != 0) {
			(void)bus_force_release();
		}
		zassert_ok(ret, "could not submit the transfer");

		tid = k_thread_create(&contender_thread, contender_stack,
				      K_THREAD_STACK_SIZEOF(contender_stack),
				      contender_entry, NULL, NULL, NULL,
				      k_thread_priority_get(k_current_get()),
				      0, K_NO_WAIT);

		k_msleep(CONTENTION_GATE_MS);

		/* Open the gate. */
		contender_early = (k_sem_count_get(&contender_done) != 0U);
		gate_open = k_cycle_get_32();
		zassert_ok(bus_force_release(), "could not release SDA");

		(void)i2c_timing_collect(&outcome);
		zassert_false(outcome.timed_out,
			      "F1b: the controller under test had not returned "
			      "%d ms after the gate opened on round %u",
			      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS, i);
		zassert_ok(k_sem_take(&contender_done,
				      K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS)),
			   "F1b: the second controller did not return within "
			   "%d ms on round %u",
			   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS, i);
		(void)k_thread_join(tid,
				    K_MSEC(CONFIG_I2C_FAULT_TEST_TIMEOUT_MS));

		/*
		 * A controller that returned before the gate opened did not
		 * wait for the bus, so that round produced no contention.
		 */
		if (contender_early ||
		    ((int32_t)(outcome.end_cycles - gate_open) < 0)) {
			early++;
			early_ret = contender_early ? contender_result
						    : outcome.ret;
			continue;
		}

		zassert_ok(contender_result,
			   "F1b: the second controller, which sends the lower "
			   "address and must win, failed with %d on round %u",
			   contender_result, i);

		if (outcome.ret == 0) {
			serialised++;
			if (can_verify) {
				(void)i2c_target_fault_get_data(ctx->target,
								&stored, 1U);
				zassert_equal(stored, gated_write[1],
					      "F1b: round %u reported success "
					      "but the fault target never "
					      "received the write",
					      i);
			}
		} else {
			lost++;
			if (lost == 1U) {
				i2c_fault_record("F1b arbitration loss",
						 outcome.ret);
			}
		}
	}

	TC_PRINT("  F1b: of %u rounds, %u lost arbitration, %u ran one after "
		 "the other, %u returned before the gate opened\n",
		 (uint32_t)CONFIG_I2C_FAULT_TEST_CONTENTION_ROUNDS, lost,
		 serialised, early);

	if (early != 0U) {
		i2c_fault_record("F1b returned while the bus was busy",
				 early_ret);
	}

	if (lost == 0U) {
		TC_PRINT("  F1b: the two controllers never started together, "
			 "so no arbitration was produced\n");
		ztest_test_skip();
		return;
	}

	i2c_fault_assert_healthy("F1b");
#else
	TC_PRINT("  the board declares no i2c-fault-contender alias, so there "
		 "is no device for a second controller to win with\n");
	ztest_test_skip();
#endif
} /* test_arbitration_contention */

#if I2C_FAULT_HAS_TARGET

static uint8_t long_buf[CONFIG_I2C_FAULT_TEST_LONG_PAYLOAD_LEN];
static K_SEM_DEFINE(mid_transaction, 0, 1);
static uint16_t mid_transaction_index;

/* Job that keeps the bus busy long enough for a second controller to try it. */
static int long_write_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	return i2c_write(ctx->dut, long_buf, sizeof(long_buf),
			 ctx->target_addr);
} /* long_write_job() */

/*
 * Announce that the first controller has reached the data phase.
 *
 * Sleeping for a fixed span instead would give a window whose width depends
 * on the bus rate, and at 400 kHz or above the transfer can be over before
 * the sleep expires. The target callback fires at a known byte, so the
 * overlap is arranged rather than hoped for.
 */
static void mid_transaction_hook(const struct device *dev,
				 enum i2c_target_fault_event_type type,
				 uint16_t index, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if ((type == I2C_TARGET_FAULT_EV_WRITE_RECEIVED) &&
	    (index == mid_transaction_index)) {
		k_sem_give(&mid_transaction);
	}
} /* mid_transaction_hook() */

#endif /* I2C_FAULT_HAS_TARGET */

/**
 * @brief F2 - a second controller attempts a transfer mid-message.
 *
 * @details
 * Stimulus:
 * - Measure an uncontended long write to the fault target as a baseline.
 * - Submit the same write again on the harness worker.
 * - Wait for the target's mid-transaction callback, or failing that a quarter
 *   of the measured baseline, then attempt a transfer from the second
 *   controller and timestamp it.
 * - Collect the first transfer and check the two intervals intersect.
 *
 * On the bus:
 * - The first controller owns the bus for the length of its payload. The
 *   second finds SDA and SCL already in use and must either report the bus
 *   busy or wait for it, without emitting a START that would corrupt the
 *   transaction in progress.
 *
 * Expected:
 * - The long write returns within the time bound.
 * - If the two intervals overlapped, the target holds exactly the bytes the
 *   long write sent, and both return values are recorded.
 * - If they did not overlap the condition skips, because asserting on a run
 *   where the two transfers did not coincide would be asserting on nothing.
 *
 * The window is made by payload length rather than by a clock stretch. A
 * stretch produced from a target callback would have to occupy the CPU to
 * hold SCL, leaving no thread able to use the window it opened. A fixed sleep
 * was also rejected: its width is absolute while the transfer's is set by the
 * bus rate, so at 400 kHz and above the first transfer can finish first.
 *
 * Requires: a fault target on a second controller, answering the controller
 * under test. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_arbitration, test_arbitration_bus_busy)
{
	i2c_fault_require_target_reachable();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome baseline;
	struct i2c_fault_outcome outcome;
	struct i2c_timing_span contender_span;
	int contender_ret;

	if (!ctx->target_is_second_controller) {
		TC_PRINT("  the fault target shares the controller under "
			 "test, so there is no second controller to contend "
			 "with\n");
		ztest_test_skip();
		return;
	}

	i2c_fault_fill(long_buf, sizeof(long_buf), 0xF2U);
	long_buf[0] = 0U;

	/*
	 * Measure the uncontended duration first. The delay before the second
	 * controller starts is a fraction of it, which keeps the arrangement
	 * correct across bus rates and payload lengths where a fixed sleep
	 * would not.
	 */
	i2c_timing_run_or_fail(long_write_job, NULL, "F2 baseline long write",
			       &baseline);
	zassert_ok(baseline.ret, "F2: the uncontended long write failed (%d)",
		   baseline.ret);
	i2c_timing_report("F2 uncontended long write", baseline.elapsed_us,
			  "us");

	mid_transaction_index = (uint16_t)(sizeof(long_buf) / 4U);
	k_sem_reset(&mid_transaction);
	i2c_target_fault_set_hook(ctx->target, mid_transaction_hook, NULL);

	zassert_ok(i2c_timing_submit(long_write_job, NULL),
		   "could not submit the long write");

	/*
	 * A target that reports byte arrivals during the transaction gives a
	 * sharper start signal than the calibrated delay, so it is preferred
	 * where available. Controllers that only report at completion fall
	 * back to the delay, which is why the wait is short and its expiry is
	 * not an error.
	 */
	if (k_sem_take(&mid_transaction,
		       K_USEC(baseline.elapsed_us / 4U)) != 0) {
		k_usleep(baseline.elapsed_us / 4U);
	}

	i2c_timing_begin(&contender_span);
	contender_ret = i2c_write(ctx->target_bus, &contender_byte, 1U,
				  ctx->absent_addr);
	i2c_timing_end(&contender_span);

	(void)i2c_timing_collect(&outcome);
	i2c_target_fault_set_hook(ctx->target, NULL, NULL);

	zassert_false(outcome.timed_out,
		      "F2: the long write did not return within %d ms",
		      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);

	i2c_fault_record("F2 long write on the bus under test", outcome.ret);
	i2c_fault_record("F2 second controller during that write",
			 contender_ret);

	/*
	 * The overlap is checked rather than assumed. Asserting on a run in
	 * which the two transfers did not in fact coincide would be asserting
	 * on nothing.
	 */
	if (!i2c_timing_overlapped(outcome.start_cycles, outcome.end_cycles,
				   contender_span.start, contender_span.end)) {
		TC_PRINT("  F2: the two transfers did not overlap, so no "
			 "contention was produced on this run\n");
		ztest_test_skip();
		return;
	}

	if ((outcome.ret == 0) &&
	    (sizeof(long_buf) <= i2c_target_fault_size(ctx->target))) {
		uint8_t stored[CONFIG_I2C_FAULT_TEST_LONG_PAYLOAD_LEN];

		(void)i2c_target_fault_get_data(ctx->target, stored,
						sizeof(long_buf) - 1U);
		zassert_mem_equal(stored, &long_buf[1], sizeof(long_buf) - 1U,
				  "F2: the long write reported success but "
				  "the target stored different bytes");
	}

	i2c_fault_assert_healthy("F2");
#endif
} /* test_arbitration_bus_busy */
