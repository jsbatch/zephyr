/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bus hang and recovery, conditions D1 to D9 of the suite.
 *
 * This is the gap the suite exists to close. Sixteen drivers call
 * i2c_bitbang_recover_bus(), which implements the nine-pulse recovery
 * sequence of NXP UM10204 rev. 6 section 3.1.16, and the two upstream tests
 * that touch recovery call i2c_recover_bus() on an idle, healthy bus. Neither
 * holds a line first, so no test demonstrates that recovery frees a bus that
 * is actually stuck, and the -EBUSY path taken when SDA is still held after
 * nine clock pulses has no coverage at all.
 *
 * The stimulus is a spare GPIO jumpered to the bus and held low. Because the
 * bus is open drain the low wins whatever the controller drives, and the hold
 * is a millisecond-scale operation that needs no timing precision.
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
#endif

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];

/* Job that attempts a write to the known-good address. */
static int write_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	return i2c_write(ctx->dut, tx_buf, sizeof(tx_buf), ctx->present_addr);
} /* write_job() */

/* Job that attempts a bus recovery. */
static int recover_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	return i2c_recover_bus(ctx->dut);
} /* recover_job() */

/* Skip the running test when the driver implements no recovery. */
static void require_recovery(void)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	if (i2c_recover_bus(ctx->dut) == -ENOSYS) {
		TC_PRINT("  the driver implements no bus recovery\n");
		ztest_test_skip();
	}
} /* require_recovery() */

/*
 * Attempt a transfer with one or both lines held low, then release them.
 *
 * The release happens before the assertion so that a driver which only
 * unwinds once the line is freed does not take the rest of the run with it.
 */
static void transfer_while_held(enum bus_force_line line,
				const char *condition,
				struct i2c_fault_outcome *out)
{
	bool recovered;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xD0U);

	zassert_ok(bus_force_low(line), "%s: could not hold the line low",
		   condition);
	zassert_ok(i2c_timing_submit(write_job, NULL),
		   "%s: could not submit the transfer", condition);

	(void)i2c_timing_collect(out);

	zassert_ok(bus_force_release(), "%s: could not release the line",
		   condition);
	recovered = i2c_timing_harness_recover();

	zassert_false(out->timed_out,
		      "%s: the transfer had not returned after %d ms and %s "
		      "once the line was released",
		      condition, CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
		      recovered ? "only returned" : "never returned");
} /* transfer_while_held() */

ZTEST_SUITE(i2c_fault_recovery, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief D1 - a transfer attempted with SDA held must fail within the bound.
 *
 * @details
 * Stimulus:
 * - Pull SDA low, submit a write to the known-good address, collect under the
 *   time bound, then release.
 *
 * On the bus:
 * - SDA is low while the bus is idle, which is the signature of a hung bus: a
 *   target that lost sync mid-byte and is holding the line. A controller
 *   cannot issue a START, because a START is a falling edge on SDA while SCL
 *   is high and SDA is already low. Nothing can be transmitted.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value in relaxed mode, within
 *   the bound.
 * - The line is released and the worker given a further bound to unwind
 *   before the assertion, so a driver that only returns once SDA is freed
 *   does not take the rest of the run with it.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_sda_held)
{
	struct i2c_fault_outcome outcome;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	transfer_while_held(BUS_FORCE_SDA, "D1 SDA held low", &outcome);
	i2c_fault_assert_error(outcome.ret, "D1 SDA held low");
} /* test_recovery_sda_held */

/**
 * @brief D2 - the same with SCL held.
 *
 * @details
 * Stimulus:
 * - As D1, but SCL is the line pulled low.
 *
 * On the bus:
 * - The controller cannot produce a clock edge, so it stalls before the
 *   START rather than at it. The failure mode differs from D1 in a way that
 *   matters to a driver: a held SDA is usually detected by a bus-free check
 *   before the transfer, while a held SCL is often only noticed as a timeout
 *   part way through, and the two take different paths through the code.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value in relaxed mode, within
 *   the bound.
 * - The line is released before the assertion, as in D1.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_scl_held)
{
	struct i2c_fault_outcome outcome;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	transfer_while_held(BUS_FORCE_SCL, "D2 SCL held low", &outcome);
	i2c_fault_assert_error(outcome.ret, "D2 SCL held low");
} /* test_recovery_scl_held */

/**
 * @brief D5 - both lines held at once.
 *
 * @details
 * Stimulus:
 * - As D1 and D2, but both SDA and SCL are pulled low together.
 *
 * On the bus:
 * - Both lines are low, which is what a board looks like when a target has
 *   been reset mid-transaction, or when the bus is unpowered while the
 *   controller is not. Neither a START nor a clock edge is possible.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value in relaxed mode, within
 *   the bound.
 * - Both lines are released before the assertion.
 *
 * D1 and D2 are each a single-line fault. This is included because a driver
 * that checks the two lines in sequence, and returns a different error
 * depending on which it noticed first, behaves differently when both are down.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_both_held)
{
	struct i2c_fault_outcome outcome;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	transfer_while_held(BUS_FORCE_BOTH, "D5 both lines held low",
			    &outcome);
	i2c_fault_assert_error(outcome.ret, "D5 both lines held low");
} /* test_recovery_both_held */

/*
 * Recovery that cannot succeed must say so.
 *
 * The reference implementation, i2c_bitbang_recover_bus() in
 * drivers/i2c/i2c_bitbang.c, returns -EBUSY when SDA is still held after
 * nine clock pulses, and that is what the strict mode requires. The relaxed
 * mode accepts any negative value, for bringing up a driver that reports the
 * condition differently.
 */
static void assert_recovery_failed(int ret, const char *condition)
{
	i2c_fault_record(condition, ret);

	if (IS_ENABLED(CONFIG_I2C_FAULT_TEST_RELAXED_ERRNO)) {
		zassert_true(ret < 0,
			     "%s: recovery reported %d with SDA still held",
			     condition, ret);
	} else {
		zassert_equal(ret, -EBUSY,
			      "%s: the reference recovery implementation "
			      "returns -EBUSY when SDA is still held after "
			      "nine clock pulses, got %d",
			      condition, ret);
	}
} /* assert_recovery_failed() */

/**
 * @brief D3 - recovery attempted against a bus that is genuinely stuck.
 *
 * @details
 * Stimulus:
 * - Pull SDA low, submit i2c_recover_bus() to the bounded harness, collect,
 *   then release.
 *
 * On the bus:
 * - Recovery is the nine-pulse sequence of NXP UM10204 rev. 6 section 3.1.16:
 *   the controller toggles SCL up to nine times so that a target stuck
 *   mid-byte clocks out its remaining bits and releases SDA, then issues a
 *   STOP. Here SDA is held by a GPIO, not by a confused target, so the nine
 *   pulses change nothing and SDA is still low at the end.
 *
 * Expected:
 * - Recovery returns -EBUSY, which is what the reference implementation
 *   i2c_bitbang_recover_bus() returns when SDA is still held after nine
 *   pulses; relaxed mode accepts any negative value.
 * - The line is released and the worker given a further bound to unwind
 *   before the assertion.
 *
 * No upstream test reaches this path. Both existing recovery tests call
 * i2c_recover_bus() on an idle, healthy bus, so the failure branch has never
 * been exercised.
 *
 * Requires: the forcing GPIOs and a driver implementing recovery. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_fails_while_sda_held)
{
	struct i2c_fault_outcome outcome;
	bool recovered;

	i2c_fault_require_bus_force();
	require_recovery();

	zassert_ok(bus_force_low(BUS_FORCE_SDA), "could not hold SDA low");
	zassert_ok(i2c_timing_submit(recover_job, NULL),
		   "could not submit the recovery attempt");

	(void)i2c_timing_collect(&outcome);

	zassert_ok(bus_force_release(), "could not release SDA");
	recovered = i2c_timing_harness_recover();

	zassert_false(outcome.timed_out,
		      "D3: i2c_recover_bus() had not returned after %d ms and "
		      "%s once SDA was released",
		      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
		      recovered ? "only returned" : "never returned");
	assert_recovery_failed(outcome.ret, "D3 recovery with SDA held");
} /* test_recovery_fails_while_sda_held */

/**
 * @brief D4 - once SDA is released, recovery must succeed and free the bus.
 *
 * @details
 * Stimulus:
 * - Hold SDA low for CONFIG_I2C_FAULT_TEST_HOLD_MS so the controller meets a
 *   hung bus, release it, then call i2c_recover_bus() on the bounded harness,
 *   then probe the known-good address.
 *
 * On the bus:
 * - By the time recovery runs the lines are free, so the nine clock pulses
 *   find SDA already high and the sequence completes. The STOP that follows
 *   leaves the bus in the idle state a controller expects before a new
 *   transaction.
 *
 * Expected:
 * - Recovery returns 0.
 * - A transfer afterwards succeeds, which is the part that matters: recovery
 *   returning 0 while leaving the bus unusable would be worse than failing.
 *
 * D3 and D4 are the two halves of the recovery contract, and neither is
 * covered upstream today.
 *
 * Requires: the forcing GPIOs and a driver implementing recovery. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_succeeds_after_release)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	int ret;

	i2c_fault_require_bus_force();
	require_recovery();

	/* Hang the bus, let the controller meet it, then free the line. */
	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xD4U);
	zassert_ok(bus_force_low(BUS_FORCE_SDA), "could not hold SDA low");
	k_msleep(CONFIG_I2C_FAULT_TEST_HOLD_MS);
	zassert_ok(bus_force_release(), "could not release SDA");

	i2c_timing_run_or_fail(recover_job, NULL,
			       "D4 recovery after SDA release", &outcome);
	i2c_fault_record("D4 recovery after SDA release", outcome.ret);
	zassert_ok(outcome.ret,
		   "D4: recovery failed on a bus with no line held (%d)",
		   outcome.ret);

	if (ctx->have_present) {
		ret = i2c_fault_probe(ctx->dut, ctx->present_addr);
		zassert_ok(ret,
			   "D4: the bus was not usable after a successful "
			   "recovery (%d)",
			   ret);
	}
} /* test_recovery_succeeds_after_release */

/**
 * @brief D6 - recovery on a healthy bus must be harmless and repeatable.
 *
 * @details
 * Stimulus:
 * - Call i2c_recover_bus() twice in succession on an idle, working bus.
 *
 * On the bus:
 * - Nine clock pulses and a STOP are emitted onto a bus where nothing is
 *   stuck. Any device that happens to be mid-transaction would be disturbed,
 *   but on an idle bus the pulses are harmless: no device is addressed, so
 *   none of them act on the clocking.
 *
 * Expected:
 * - Both calls return 0, and the bus is usable afterwards.
 *
 * This extends the smoke tests in tests/drivers/i2c/i2c_at24 and
 * tests/drivers/i2c/i2c_bme688, which each call recovery once. Calling it
 * twice catches a driver that leaves its pins reconfigured after the first.
 *
 * Requires: a driver implementing recovery. No fixture.
 */
ZTEST(i2c_fault_recovery, test_recovery_idempotent)
{
	struct i2c_fault_outcome outcome;

	require_recovery();

	i2c_timing_run_or_fail(recover_job, NULL, "D6 recovery when healthy",
			       &outcome);
	zassert_ok(outcome.ret, "D6: the first recovery failed (%d)",
		   outcome.ret);

	i2c_timing_run_or_fail(recover_job, NULL, "D6 recovery when healthy",
			       &outcome);
	zassert_ok(outcome.ret, "D6: the second recovery failed (%d)",
		   outcome.ret);

	i2c_fault_assert_healthy("D6");
} /* test_recovery_idempotent */

/**
 * @brief D7 - a driver without recovery must say -ENOSYS.
 *
 * @details
 * Stimulus:
 * - Call i2c_recover_bus() once on a healthy bus and inspect the value.
 *
 * On the bus:
 * - Either nine clock pulses and a STOP, or nothing at all, depending on
 *   whether the driver implements the operation.
 *
 * Expected:
 * - Exactly 0 or -ENOSYS. Those are the two values the API defines for this
 *   call: success, or a driver that does not implement it.
 *
 * The point is the distinction between "not implemented" and "failed". A
 * caller can react usefully to -ENOSYS by falling back to a power cycle or a
 * bit-banged sequence of its own; it cannot react to a generic error. This
 * extends the skip path in tests/drivers/i2c/i2c_at24 into an assertion.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_recovery, test_recovery_unsupported_reports_enosys)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret = i2c_recover_bus(ctx->dut);

	i2c_fault_record("D7 recovery support", ret);
	zassert_true((ret == 0) || (ret == -ENOSYS),
		     "D7: recovery on a healthy bus returned %d; the API "
		     "defines 0 for success and -ENOSYS for a driver that "
		     "does not implement it",
		     ret);
} /* test_recovery_unsupported_reports_enosys */

/**
 * @brief D8 - repeated consecutive recovery calls must behave identically.
 *
 * @details
 * Stimulus:
 * - Call i2c_recover_bus() CONFIG_I2C_FAULT_TEST_REPEAT_COUNT times in a row
 *   on an idle bus.
 *
 * On the bus:
 * - The same nine-pulse sequence and STOP, repeated. Nothing about the bus
 *   differs between calls.
 *
 * Expected:
 * - Every call returns 0.
 * - The bus is usable afterwards.
 *
 * D6 checks two calls; this checks a series. Recovery typically reconfigures
 * the SDA and SCL pins from their peripheral function to GPIO and back, and a
 * driver that leaks a pin configuration does so cumulatively, which shows up
 * over several calls rather than two.
 *
 * Requires: a driver implementing recovery. No fixture.
 */
ZTEST(i2c_fault_recovery, test_recovery_repeated)
{
	struct i2c_fault_outcome outcome;

	require_recovery();

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		i2c_timing_run_or_fail(recover_job, NULL,
				       "D8 repeated recovery", &outcome);

		if (i == 0U) {
			i2c_fault_record("D8 repeated recovery", outcome.ret);
		}

		zassert_ok(outcome.ret,
			   "D8: call %u of %u on a healthy bus failed (%d)",
			   i + 1U, CONFIG_I2C_FAULT_TEST_REPEAT_COUNT,
			   outcome.ret);
	}

	i2c_fault_assert_healthy("D8");
} /* test_recovery_repeated */

#if I2C_FAULT_HAS_TARGET

static uint16_t sda_pull_index;

/*
 * Pull SDA low from inside a target callback.
 *
 * The callback gives a trigger that lands in the middle of a live
 * transaction at a byte index the test chooses, which is otherwise difficult
 * to arrange. It runs in whatever context the controller services target
 * callbacks in, so it does no more than set a pin.
 */
static void sda_pull_hook(const struct device *dev,
			  enum i2c_target_fault_event_type type,
			  uint16_t index, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if ((type == I2C_TARGET_FAULT_EV_WRITE_RECEIVED) &&
	    (index == sda_pull_index)) {
		bus_force_assert_low(BUS_FORCE_SDA);
	}
} /* sda_pull_hook() */

#endif /* I2C_FAULT_HAS_TARGET */

/**
 * @brief D9 - SDA is pulled low partway through a healthy transfer.
 *
 * @details
 * Stimulus:
 * - Arm the SDA forcing pin as a released output, install a hook that pulls
 *   it low when the target reports the byte at index len/2, then write to the
 *   target.
 * - Afterwards release the line, attempt recovery, and check the bus.
 *
 * On the bus:
 * - The transaction begins and proceeds correctly for half its payload. SDA
 *   is then pulled low and stays low, in the middle of a byte, while the
 *   controller is actively driving. The controller is transmitting, so it
 *   drives SDA itself; on the bits where it drives a one it will read back a
 *   zero, which is the arbitration-loss condition, and on the acknowledge
 *   clocks it sees a permanent ACK.
 *
 * This is the hardest stimulus in the suite to arrange, because it must land
 * inside a live transaction. The target callback provides the trigger at no
 * cost, at a byte index the test chooses.
 *
 * Expected:
 * - The transfer returns within the time bound; the value is recorded, since
 *   drivers classify this variously as arbitration loss, a bus error or a
 *   timeout.
 * - Recovery afterwards succeeds, or reports -ENOSYS.
 * - The bus is usable at the end.
 *
 * The hook runs in interrupt context on most drivers, so the pin is armed
 * beforehand and the hook does nothing but write it.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_recovery, test_recovery_sda_pulled_mid_transfer)
{
	i2c_fault_require_target();
	i2c_fault_require_bus_force();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	bool recovered;

	sda_pull_index = (uint16_t)(sizeof(tx_buf) / 2U);
	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xD9U);
	tx_buf[0] = 0U;

	/*
	 * The hook runs in the controller's target callback context, which is
	 * interrupt context on most drivers, and reconfiguring a pin there is
	 * not generically safe. Arming the pin first leaves the hook with
	 * nothing to do but write it.
	 */
	zassert_ok(bus_force_arm(BUS_FORCE_SDA),
		   "could not arm the SDA forcing pin");
	i2c_target_fault_set_hook(ctx->target, sda_pull_hook, NULL);

	msg.buf = tx_buf;
	msg.len = sizeof(tx_buf);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	zassert_ok(i2c_fault_transfer_submit(&xfer),
		   "could not submit the transfer");

	(void)i2c_timing_collect(&outcome);

	i2c_target_fault_set_hook(ctx->target, NULL, NULL);
	zassert_ok(bus_force_release(), "could not release SDA");
	recovered = i2c_timing_harness_recover();

	zassert_false(outcome.timed_out,
		      "D9: the transfer had not returned after %d ms and %s "
		      "once SDA was released",
		      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
		      recovered ? "only returned" : "never returned");
	i2c_fault_record("D9 SDA pulled low mid-transfer", outcome.ret);

	if (i2c_recover_bus(ctx->dut) == -ENOSYS) {
		TC_PRINT("  the driver implements no bus recovery\n");
	} else {
		zassert_ok(i2c_recover_bus(ctx->dut),
			   "D9: recovery failed after SDA was released");
	}

	i2c_fault_assert_healthy("D9");
#endif
} /* test_recovery_sda_pulled_mid_transfer */
