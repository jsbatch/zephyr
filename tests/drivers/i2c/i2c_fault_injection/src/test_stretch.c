/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Clock stretching, conditions C1 to C9 of the suite.
 *
 * Every stretch here is produced the same way: SCL is pulled low through a
 * spare GPIO jumpered to the bus, and released by a timer. What differs
 * between the two groups is when the pull happens.
 *
 * C1 to C6 position the stretch at a chosen byte by starting it from the
 * fault target's callback, which the controller invokes mid-transaction. The
 * hook does one pin write and returns. These need both fixtures and reach
 * only the controllers that deliver byte callbacks during the transfer.
 *
 * C7 to C9 pull the line from the test thread, which cannot pick a byte but
 * needs no target at all and so reaches every controller in the tree.
 *
 * Holding SCL by declining to return from a target callback would produce the
 * same waveform and would not need the GPIO. It is not done, because those
 * callbacks run in interrupt context on most controllers and waiting there is
 * not permissible: it would make the CPU unavailable to every thread for the
 * length of the stretch.
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

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t expected_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t stretch_rx[I2C_FAULT_PAYLOAD_LEN];
static uint8_t offset_byte;

/* Byte index the positioned stretch starts on, and how long it lasts. */
static uint16_t stretch_index;
static uint32_t stretch_span_us;
static enum i2c_target_fault_event_type stretch_event;
static uint32_t stretch_count;
static bool stretch_every;
#endif

/*
 * Buffers handed to the bounded harness must outlive the test that submitted
 * them, because a transfer that outruns the bound is left running and the
 * controller may still be walking them.
 */
static uint8_t probe_byte;

#if I2C_FAULT_HAS_TARGET

/*
 * Begin a positioned stretch. Runs in the controller's target callback
 * context, which is interrupt context on most drivers, so it starts the pulse
 * and returns; the timer ends it.
 */
static void stretch_hook(const struct device *dev,
			 enum i2c_target_fault_event_type type, uint16_t index,
			 void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (type != stretch_event) {
		return;
	}

	if (stretch_every || (index == stretch_index)) {
		stretch_count++;
		bus_force_pulse_low(BUS_FORCE_SCL, stretch_span_us);
	}
} /* stretch_hook() */

/* Arm a stretch of @p span_us at @p index of the phase named by @p type. */
static void arm_stretch(enum i2c_target_fault_event_type type, uint16_t index,
			uint32_t span_us)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	stretch_event = type;
	stretch_index = index;
	stretch_span_us = span_us;
	stretch_every = (index == I2C_TARGET_FAULT_TRIGGER_EVERY);
	stretch_count = 0U;

	zassert_ok(bus_force_arm(BUS_FORCE_SCL),
		   "could not arm the SCL forcing pin");
	i2c_target_fault_set_hook(ctx->target, stretch_hook, NULL);
} /* arm_stretch() */

/* Remove the stretch hook and make sure the line is not left held. */
static void disarm_stretch(void)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	i2c_target_fault_set_hook(ctx->target, NULL, NULL);

	if (stretch_count > 0U) {
		zassert_ok(bus_force_pulse_wait(
				   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS),
			   "the stretch timer did not release SCL");
	}

	zassert_ok(bus_force_release(), "could not release SCL");
} /* disarm_stretch() */

/* Skip unless both a usable target and the forcing GPIOs are present. */
static void require_positioned_stretch(void)
{
	i2c_fault_require_target();
	i2c_fault_require_bus_force();
} /* require_positioned_stretch() */

/* Write the payload to the target and time how long the controller took. */
static void write_stretched(const char *condition,
			    struct i2c_fault_outcome *out)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xC0U);
	tx_buf[0] = 0U;

	msg.buf = tx_buf;
	msg.len = sizeof(tx_buf);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, condition, out);
	i2c_timing_report(condition, out->elapsed_us, "us");
} /* write_stretched() */

/*
 * Measure how long an unstretched write of the same shape takes, so the
 * stretch conditions can assert that the controller actually waited rather
 * than assuming a fixed baseline that varies with bus speed.
 */
static uint32_t baseline_us(void)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;

	i2c_target_fault_set_hook(ctx->target, NULL, NULL);
	write_stretched("C baseline write", &outcome);
	zassert_ok(outcome.ret, "the unstretched baseline write failed (%d)",
		   outcome.ret);

	return outcome.elapsed_us;
} /* baseline_us() */

#endif /* I2C_FAULT_HAS_TARGET */

ZTEST_SUITE(i2c_fault_stretch, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief C1 - the stretch begins at the address acknowledgement.
 *
 * @details
 * Stimulus:
 * - Measure an unstretched write to the target as a baseline.
 * - Arm the hook on the callback the controller runs at address match, with a
 *   CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US pulse, then repeat the write.
 *
 * On the bus:
 * - START and the address are acknowledged. As the target's callback runs,
 *   the hook pulls SCL low and returns. The controller has released SCL
 *   expecting it to rise; it does not, so the controller waits. A timer
 *   releases the line and the transaction resumes from where it stopped. The
 *   stretch lands between the acknowledge and the first data byte, which is
 *   the earliest point in a transaction a target can stall a controller.
 *
 * Expected:
 * - The stretch was actually applied, confirmed from the hook's counter.
 * - The transfer succeeds. A stretch of this length is well within any
 *   plausible driver tolerance.
 * - It takes measurably longer than the baseline, which is what shows the
 *   controller waited rather than ignoring the held clock.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_address)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;
	uint32_t baseline = baseline_us();

	arm_stretch(I2C_TARGET_FAULT_EV_WRITE_REQUESTED, 0U,
		    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US);
	write_stretched("C1 stretch during address acknowledgement",
			&outcome);
	disarm_stretch();

	zassert_true(stretch_count > 0U,
		     "C1: the stretch was never started, so the result says "
		     "nothing about the controller");
	zassert_ok(outcome.ret,
		   "C1: a %u us stretch must be tolerated, got %d",
		   CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US, outcome.ret);
	zassert_true(outcome.elapsed_us > baseline,
		     "C1: the transfer took %u us against a %u us baseline, "
		     "so the controller did not wait",
		     outcome.elapsed_us, baseline);
	i2c_fault_assert_healthy("C1");
#endif
} /* test_stretch_address */

/**
 * @brief C2 - a stretch within any reasonable tolerance must still succeed.
 *
 * @details
 * Stimulus:
 * - As C1, but the pulse begins on the data byte at index 1 rather than at
 *   the address phase.
 *
 * On the bus:
 * - The address and the first data byte are acknowledged normally. SCL is
 *   then held low between two data bytes, which is where a real device
 *   stretches when it needs time to store what it just received. The
 *   controller waits, the timer releases, and the remaining bytes follow.
 *
 * Expected:
 * - The stretch was applied.
 * - The transfer succeeds. Clock stretching is a normal part of the protocol
 *   and a controller that fails here is broken, not merely intolerant.
 * - It takes measurably longer than the unstretched baseline.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_short)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;
	uint32_t baseline = baseline_us();

	arm_stretch(I2C_TARGET_FAULT_EV_WRITE_RECEIVED, 1U,
		    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US);
	write_stretched("C2 short stretch within tolerance", &outcome);
	disarm_stretch();

	zassert_true(stretch_count > 0U,
		     "C2: the stretch was never started");
	zassert_ok(outcome.ret,
		   "C2: a %u us stretch must be tolerated, got %d",
		   CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US, outcome.ret);
	zassert_true(outcome.elapsed_us > baseline,
		     "C2: the transfer took %u us against a %u us baseline, "
		     "so the controller did not wait",
		     outcome.elapsed_us, baseline);
	i2c_fault_assert_healthy("C2");
#endif
} /* test_stretch_short */

/**
 * @brief C3 - a stretch long enough to approach a driver timeout.
 *
 * @details
 * Stimulus:
 * - As C2, but with CONFIG_I2C_FAULT_TEST_STRETCH_LONG_US, which is long
 *   enough to be near the boundary for some drivers and comfortably inside it
 *   for others.
 *
 * On the bus:
 * - SCL is held between two data bytes for milliseconds rather than
 *   microseconds. A real device doing this would be one performing an
 *   internal write cycle.
 *
 * Expected:
 * - No particular outcome. Drivers set their stretch timeouts differently and
 *   both tolerating this and timing out on it are defensible, so the value is
 *   recorded rather than asserted.
 * - The transfer returns within the time bound and the bus is usable
 *   afterwards. That much is required of every driver.
 *
 * The condition earns its place by populating the error-code survey with the
 * ambiguous case, between C2 which must succeed and C4 which must not hang.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_long)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;

	arm_stretch(I2C_TARGET_FAULT_EV_WRITE_RECEIVED, 1U,
		    CONFIG_I2C_FAULT_TEST_STRETCH_LONG_US);
	write_stretched("C3 long stretch near the driver timeout", &outcome);
	disarm_stretch();
	i2c_fault_record("C3 long stretch near the driver timeout",
			 outcome.ret);
	i2c_fault_assert_healthy("C3");
#endif
} /* test_stretch_long */

/**
 * @brief C4 - a stretch beyond any plausible timeout must not hang.
 *
 * @details
 * Stimulus:
 * - As C3, but with CONFIG_I2C_FAULT_TEST_STRETCH_EXCESSIVE_US, tens of
 *   milliseconds, which is past any reasonable driver's patience.
 *
 * On the bus:
 * - SCL is held low for long enough that the transaction is, for practical
 *   purposes, stalled indefinitely. This is what a device that has crashed
 *   mid-transaction looks like from the controller's side.
 *
 * Expected:
 * - The transfer returns, one way or the other, within
 *   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS. An error is the expected outcome; a
 *   driver with no stretch timeout may instead succeed after waiting, which
 *   is recorded rather than failed.
 * - What is not acceptable is not returning at all. That is caught by the
 *   bounded harness and reported as a hang, rather than showing up as a
 *   twister timeout indistinguishable from a build or flash problem.
 * - The bus is usable afterwards.
 *
 * This is the condition the suite exists for.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_excessive)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;

	arm_stretch(I2C_TARGET_FAULT_EV_WRITE_RECEIVED, 1U,
		    CONFIG_I2C_FAULT_TEST_STRETCH_EXCESSIVE_US);
	write_stretched("C4 excessive stretch", &outcome);
	disarm_stretch();
	i2c_fault_record("C4 excessive stretch", outcome.ret);
	i2c_fault_assert_healthy("C4");
#endif
} /* test_stretch_excessive */

/**
 * @brief C5 - the stretch begins while a read byte is being fetched.
 *
 * @details
 * Stimulus:
 * - Preload the target with a known pattern, arm the hook on the read
 *   callback at index len/2, then perform a write-read.
 *
 * On the bus:
 * - The write phase and the repeated START complete, and the read runs until
 *   the chosen index. There the target holds SCL low while it would be
 *   fetching the next byte, which is the direction a real device stretches
 *   most often: it needs time to produce data, not to store it. The
 *   controller waits mid-read with bytes still outstanding.
 *
 * Expected:
 * - The stretch was applied.
 * - The transfer succeeds.
 * - Every byte read back matches the pattern. A stretch must delay a read,
 *   not corrupt it, and a controller that resynchronises badly after waiting
 *   mid-read returns shifted or duplicated bytes.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_read)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];

	offset_byte = 0U;
	i2c_fault_fill(expected_buf, sizeof(expected_buf), 0xC5U);
	zassert_ok(i2c_target_fault_set_data(ctx->target, expected_buf,
					     sizeof(expected_buf)),
		   "could not preload the target buffer");
	(void)memset(stretch_rx, 0x00U, sizeof(stretch_rx));

	arm_stretch(I2C_TARGET_FAULT_EV_READ_PROCESSED,
		    (uint16_t)(sizeof(stretch_rx) / 2U),
		    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US);

	msgs[0].buf = &offset_byte;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = stretch_rx;
	msgs[1].len = sizeof(stretch_rx);
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	i2c_fault_transfer_bounded(&xfer,
				   "C5 stretch while a read byte is fetched",
				   &outcome);
	disarm_stretch();
	i2c_timing_report("C5 stretch while a read byte is fetched",
			  outcome.elapsed_us, "us");

	zassert_true(stretch_count > 0U,
		     "C5: the stretch was never started");
	zassert_ok(outcome.ret,
		   "C5: a %u us stretch during a read must be tolerated, "
		   "got %d",
		   CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US, outcome.ret);
	zassert_mem_equal(stretch_rx, expected_buf, sizeof(expected_buf),
			  "C5: the stretched read returned the wrong data");
	i2c_fault_assert_healthy("C5");
#endif
} /* test_stretch_read */

/**
 * @brief C6 - several stretches inside one transaction.
 *
 * @details
 * Stimulus:
 * - Arm the hook with I2C_TARGET_FAULT_TRIGGER_EVERY so a pulse begins on
 *   every byte, then perform one write.
 *
 * On the bus:
 * - SCL is held briefly between each pair of data bytes, so the transaction
 *   proceeds in stop-start fashion from beginning to end. A real device with
 *   a slow internal bus behaves this way for a whole burst rather than once.
 *
 * Expected:
 * - More than one stretch was applied, otherwise the condition reduces to C2.
 * - The transfer succeeds, and takes measurably longer than the baseline.
 *
 * C1 to C5 each apply one stretch. This asks whether a driver that recovers
 * from one recovers from a series, which catches state that is set on the
 * first wait and never cleared.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_repeated)
{
	require_positioned_stretch();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;
	uint32_t baseline = baseline_us();

	arm_stretch(I2C_TARGET_FAULT_EV_WRITE_RECEIVED,
		    I2C_TARGET_FAULT_TRIGGER_EVERY,
		    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US);
	write_stretched("C6 repeated stretches in one transaction", &outcome);
	disarm_stretch();

	zassert_true(stretch_count > 1U,
		     "C6: only %u stretch was applied, so the condition was "
		     "not produced",
		     stretch_count);
	zassert_ok(outcome.ret,
		   "C6: %u stretches of %u us each must be tolerated, got %d",
		   stretch_count, CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US,
		   outcome.ret);
	zassert_true(outcome.elapsed_us > baseline,
		     "C6: the transfer took %u us against a %u us baseline, "
		     "so the controller did not wait",
		     outcome.elapsed_us, baseline);
	i2c_fault_assert_healthy("C6");
#endif
} /* test_stretch_repeated */

/*
 * C7 to C9 hold SCL through a spare GPIO instead of stretching from inside a
 * target callback. That cannot pick a bit position, but it needs no target
 * mode and so reaches every controller in the tree.
 */

/* Describe a zero-length probe of the known-good address for the harness. */
static void probe_xfer(struct i2c_fault_xfer *xfer, struct i2c_msg *msg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	msg->buf = &probe_byte;
	msg->len = 0U;
	msg->flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer->bus = ctx->dut;
	xfer->addr = ctx->present_addr;
	xfer->msgs = msg;
	xfer->num_msgs = 1U;
} /* probe_xfer() */

/**
 * @brief C7 - a transient hold before a transaction must not break it.
 *
 * @details
 * Stimulus:
 * - Pull SCL low for CONFIG_I2C_FAULT_TEST_HOLD_MS, release it, and only then
 *   probe the known-good address.
 *
 * On the bus:
 * - The line is low while the bus is idle, which looks to any controller like
 *   another master holding the clock. It is released before this controller
 *   attempts anything, so by the time the transaction starts the bus is
 *   entirely normal.
 *
 * Expected:
 * - The probe succeeds.
 *
 * This is distinct from C9, where the hold overlaps the transaction. Here
 * nothing should have been affected at all, and a failure means the driver
 * latched a bus-busy condition it never needed to notice.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_hold_before_transfer)
{
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	zassert_ok(bus_force_low(BUS_FORCE_SCL), "could not hold SCL low");
	k_msleep(CONFIG_I2C_FAULT_TEST_HOLD_MS);
	zassert_ok(bus_force_release(), "could not release SCL");

	probe_xfer(&xfer, &msg);
	i2c_fault_transfer_bounded(&xfer, "C7 SCL held before the transfer",
				   &outcome);

	zassert_ok(outcome.ret,
		   "C7: the transfer after a %d ms hold failed (%d)",
		   CONFIG_I2C_FAULT_TEST_HOLD_MS, outcome.ret);
} /* test_stretch_hold_before_transfer */

/**
 * @brief C8 - SCL held for the whole attempt must produce a bounded error.
 *
 * @details
 * Stimulus:
 * - Pull SCL low, submit a probe to the bounded harness, and leave the line
 *   held for the whole time bound.
 *
 * On the bus:
 * - The controller cannot generate a clock edge, so it cannot even issue a
 *   START. Nothing appears on the bus at all. This is the coarsest possible
 *   stall and it reaches every controller in the tree, including the 67 with
 *   no target mode.
 *
 * Expected:
 * - The transfer returns within the time bound, with an error.
 * - The line is released and the worker given a further bound to unwind
 *   before the assertion runs, so a driver that only returns once the clock
 *   is freed is reported as the defect it is without taking the rest of the
 *   run with it.
 * - The bus is usable afterwards.
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_hold_excessive)
{
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	bool recovered;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	probe_xfer(&xfer, &msg);

	zassert_ok(bus_force_low(BUS_FORCE_SCL), "could not hold SCL low");
	zassert_ok(i2c_fault_transfer_submit(&xfer),
		   "could not submit the transfer");

	(void)i2c_timing_collect(&outcome);

	zassert_ok(bus_force_release(), "could not release SCL");
	recovered = i2c_timing_harness_recover();

	zassert_false(outcome.timed_out,
		      "C8: the driver was still waiting on SCL after %d ms "
		      "and %s once the line was released",
		      CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
		      recovered ? "only returned" : "never returned");

	i2c_fault_record("C8 SCL held beyond any timeout", outcome.ret);
	i2c_fault_assert_healthy("C8");
} /* test_stretch_hold_excessive */

/**
 * @brief C9 - releasing SCL mid-attempt must let the transfer complete.
 *
 * @details
 * Stimulus:
 * - Pull SCL low, submit a probe to the bounded harness, wait
 *   CONFIG_I2C_FAULT_TEST_HOLD_MS, release the line, then collect the result.
 *
 * On the bus:
 * - The controller is stalled before it can begin, exactly as in C8. Part way
 *   through, the clock is freed and the transaction it was waiting to start
 *   proceeds normally to completion.
 *
 * Expected:
 * - The transfer returns and succeeds.
 * - It took at least as long as the line was held. That is what separates a
 *   genuine wait from a driver that failed for an unrelated reason and
 *   happened to succeed on a retry.
 *
 * C8 and C9 apply the same stimulus and differ only in whether it is
 * withdrawn, which isolates "the controller waits" from "the controller gives
 * up".
 *
 * Requires: the forcing GPIOs and one device that answers. Fixture:
 * i2c_bus_fault.
 */
ZTEST(i2c_fault_stretch, test_stretch_release_mid_attempt)
{
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	uint32_t held_us = (uint32_t)CONFIG_I2C_FAULT_TEST_HOLD_MS *
			   USEC_PER_MSEC;

	i2c_fault_require_bus_force();
	i2c_fault_require_present();

	probe_xfer(&xfer, &msg);

	zassert_ok(bus_force_low(BUS_FORCE_SCL), "could not hold SCL low");
	zassert_ok(i2c_fault_transfer_submit(&xfer),
		   "could not submit the transfer");

	k_msleep(CONFIG_I2C_FAULT_TEST_HOLD_MS);
	zassert_ok(bus_force_release(), "could not release SCL");

	(void)i2c_timing_collect(&outcome);
	i2c_timing_report("C9 transfer spanning an SCL hold",
			  outcome.elapsed_us, "us");

	zassert_false(outcome.timed_out,
		      "C9: the transfer did not return after SCL was "
		      "released");
	zassert_ok(outcome.ret,
		   "C9: the transfer failed even though SCL was released "
		   "after %d ms (%d)",
		   CONFIG_I2C_FAULT_TEST_HOLD_MS, outcome.ret);
	zassert_true(outcome.elapsed_us >= held_us,
		     "C9: the transfer took %u us but SCL was held for %u us, "
		     "so the controller did not wait for the line",
		     outcome.elapsed_us, held_us);
} /* test_stretch_release_mid_attempt */
