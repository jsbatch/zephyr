/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Timing measurements, conditions I1 to I5 of the suite.
 *
 * These are diagnostic. Bounds are deliberately loose and, unless
 * CONFIG_I2C_FAULT_TEST_TIMING_ASSERT is set, a measurement outside them is
 * reported rather than failed. A suite that fails intermittently on timing
 * across a dozen platforms is worse than no suite; what these produce is a
 * record in the log that a maintainer can read.
 *
 * Measurement uses k_cycle_get_32() and sys_clock_hw_cycles_per_sec(), which
 * are available on every platform and need no peripheral. At transaction
 * scale they resolve a 100 kHz bus well enough to tell one configured speed
 * from another.
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

/* Bits on the wire for a write of n payload bytes: START, address, data. */
#define BITS_PER_BYTE_ON_WIRE 9U
#define ADDRESS_BYTES         1U

static uint8_t tx_buf[CONFIG_I2C_FAULT_TEST_LONG_PAYLOAD_LEN];

/* Report a measurement, and fail on it only when asked to. */
static void check_bound(const char *what, uint32_t value, uint32_t low,
			uint32_t high)
{
	bool within = (value >= low) && (value <= high);

	TC_PRINT("  %s: %u (expected %u to %u)%s\n", what, value, low, high,
		 within ? "" : "  OUT OF RANGE");

	if (IS_ENABLED(CONFIG_I2C_FAULT_TEST_TIMING_ASSERT)) {
		zassert_true(within, "%s: %u is outside %u to %u", what, value,
			     low, high);
	}
} /* check_bound() */

/* Time a write of len bytes to the address the suite settled on. */
static uint32_t time_write(uint16_t addr, size_t len)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	i2c_fault_fill(tx_buf, len, 0x10U);
	tx_buf[0] = 0U;

	msg.buf = tx_buf;
	msg.len = len;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, "timed write", &outcome);
	zassert_ok(outcome.ret, "the timed write failed (%d)", outcome.ret);

	return outcome.elapsed_us;
} /* time_write() */

ZTEST_SUITE(i2c_fault_timing, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief I1 - the rate the bus runs at against the rate configured.
 *
 * @details
 * Stimulus:
 * - Read the configured speed code, time a write of
 *   CONFIG_I2C_FAULT_TEST_LONG_PAYLOAD_LEN bytes to the target, and divide
 *   the bits on the wire by the time taken.
 *
 * On the bus:
 * - One ordinary addressed write. The bit count includes nine clocks per
 *   byte, for the eight data bits and the acknowledge, plus the address byte.
 *   The measured interval also contains the START, the STOP and any gap the
 *   controller inserts between bytes, so the figure is always lower than the
 *   nominal rate.
 *
 * Expected:
 * - The measured rate lies between half the configured rate and the
 *   configured rate. Half is the loosest bound that still catches a divider
 *   programmed to the wrong value, which is the defect worth finding here.
 * - Reported, and failed only under CONFIG_I2C_FAULT_TEST_TIMING_ASSERT.
 *
 * Derived from a transfer rather than an edge count, so it measures what a
 * caller experiences rather than what a scope would show.
 *
 * Requires: a registered fault target and i2c_get_config(). Fixture:
 * i2c_bus_short.
 */
ZTEST(i2c_fault_timing, test_timing_bus_rate)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t bits;
	uint32_t elapsed_us;
	uint32_t rate_khz;
	uint32_t configured_khz;
	uint32_t config;

	i2c_fault_require_target();

	if (i2c_get_config(ctx->dut, &config) != 0) {
		TC_PRINT("  the driver does not report its configuration\n");
		ztest_test_skip();
		return;
	}

	switch (I2C_SPEED_GET(config)) {
	case I2C_SPEED_STANDARD:
		configured_khz = 100U;
		break;
	case I2C_SPEED_FAST:
		configured_khz = 400U;
		break;
	case I2C_SPEED_FAST_PLUS:
		configured_khz = 1000U;
		break;
	case I2C_SPEED_HIGH:
		configured_khz = 3400U;
		break;
	case I2C_SPEED_ULTRA:
		configured_khz = 5000U;
		break;
	default:
		TC_PRINT("  the controller reports speed code %u, which has "
			 "no defined rate\n", I2C_SPEED_GET(config));
		ztest_test_skip();
		return;
	}

	elapsed_us = time_write(ctx->target_addr, sizeof(tx_buf));
	zassert_true(elapsed_us > 0U, "I1: the transfer measured as zero");

	bits = (uint32_t)(sizeof(tx_buf) + ADDRESS_BYTES) *
	       BITS_PER_BYTE_ON_WIRE;
	rate_khz = (bits * 1000U) / elapsed_us;

	TC_PRINT("  I1: %u bits in %u us gives %u kHz against a configured "
		 "%u kHz\n", bits, elapsed_us, rate_khz, configured_khz);

	/*
	 * The measured rate is always lower than the configured one, because
	 * the interval includes the START, the STOP and any inter-byte gap
	 * the controller inserts. Half the configured rate is the loosest
	 * bound that still catches a divider programmed to the wrong value.
	 */
	check_bound("I1 effective bus rate in kHz", rate_khz,
		    configured_khz / 2U, configured_khz);
} /* test_timing_bus_rate */

/**
 * @brief I2 - transfer duration against payload length.
 *
 * @details
 * Stimulus:
 * - Time a write of a quarter of the long payload, then one of the full
 *   payload, and take the ratio.
 *
 * On the bus:
 * - Two writes to the same address differing only in how many data bytes
 *   follow. Everything else, the START, the address, the STOP, is identical,
 *   so the difference between them is close to pure per-byte cost.
 *
 * Expected:
 * - The ratio lies between 2.0 and 6.0 for a fourfold length increase.
 * - Reported, and failed only under CONFIG_I2C_FAULT_TEST_TIMING_ASSERT.
 *
 * The bounds are loose at both ends on purpose. Below 2.0 means fixed
 * per-transaction overhead dominates, which is worth knowing but is not a
 * defect. Above 6.0 means cost is growing faster than payload, which is: it
 * is the signature of a driver that inserts a fixed delay per byte or that
 * re-scans a message list for every byte it sends.
 *
 * Requires: a registered fault target. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_timing, test_timing_linearity)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	size_t short_len = sizeof(tx_buf) / 4U;
	uint32_t short_us;
	uint32_t long_us;
	uint32_t ratio_x100;

	i2c_fault_require_target();

	zassert_true(short_len >= 2U,
		     "I2: CONFIG_I2C_FAULT_TEST_LONG_PAYLOAD_LEN is too small "
		     "to compare two lengths");

	short_us = time_write(ctx->target_addr, short_len);
	long_us = time_write(ctx->target_addr, sizeof(tx_buf));

	zassert_true(short_us > 0U, "I2: the short transfer measured as zero");

	ratio_x100 = (long_us * 100U) / short_us;

	TC_PRINT("  I2: %zu bytes took %u us, %zu bytes took %u us\n",
		 short_len, short_us, sizeof(tx_buf), long_us);
	check_bound("I2 duration ratio, hundredths", ratio_x100, 200U, 600U);
} /* test_timing_linearity */

#if I2C_FAULT_HAS_TARGET
static uint32_t timing_stretch_count;

/* Begin a positioned stretch. ISR context: starts a pulse and returns. */
static void timing_stretch_hook(const struct device *dev,
				enum i2c_target_fault_event_type type,
				uint16_t index, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if ((type == I2C_TARGET_FAULT_EV_WRITE_RECEIVED) && (index == 1U)) {
		timing_stretch_count++;
		bus_force_pulse_low(BUS_FORCE_SCL,
				    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US);
	}
} /* timing_stretch_hook() */
#endif

/**
 * @brief I3 - the cost of a known clock stretch.
 *
 * @details
 * Stimulus:
 * - Time an unstretched write to the target as a baseline.
 * - Install a hook that pulses SCL low for
 *   CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US at data byte index 1, repeat the
 *   write, and take the difference.
 *
 * On the bus:
 * - Two identical writes, the second with SCL held low for a known span
 *   between two data bytes. The difference in duration is the cost the
 *   controller paid for honouring the stretch.
 *
 * Expected:
 * - The stretch was actually applied.
 * - The stretched transfer took longer than the baseline; a difference of
 *   zero means the controller ignored the held clock, which is a correctness
 *   failure rather than a timing one and is asserted unconditionally.
 * - The measured cost lies between one and four times the stretch span.
 *   Reported, and failed only under CONFIG_I2C_FAULT_TEST_TIMING_ASSERT.
 *
 * The upper bound is generous because a controller may only re-examine the
 * line on a timer tick or an interrupt, so it can wait somewhat longer than
 * the line was actually held. Waiting much longer than that suggests it is
 * polling slowly rather than reacting.
 *
 * The pulse is begun from the callback and ended by a timer so that nothing
 * waits in interrupt context; see the file header of test_stretch.c.
 *
 * Requires: a fault target with byte callbacks, and the forcing GPIOs.
 * Fixtures: i2c_bus_short and i2c_bus_fault.
 */
ZTEST(i2c_fault_timing, test_timing_stretch_cost)
{
	i2c_fault_require_target();
	i2c_fault_require_bus_force();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t baseline_us;
	uint32_t stretched_us;
	uint32_t cost_us;

	baseline_us = time_write(ctx->target_addr, sizeof(tx_buf));

	timing_stretch_count = 0U;
	zassert_ok(bus_force_arm(BUS_FORCE_SCL),
		   "could not arm the SCL forcing pin");
	i2c_target_fault_set_hook(ctx->target, timing_stretch_hook, NULL);

	stretched_us = time_write(ctx->target_addr, sizeof(tx_buf));

	i2c_target_fault_set_hook(ctx->target, NULL, NULL);
	if (timing_stretch_count > 0U) {
		zassert_ok(bus_force_pulse_wait(
				   CONFIG_I2C_FAULT_TEST_TIMEOUT_MS),
			   "the stretch timer did not release SCL");
	}
	zassert_ok(bus_force_release(), "could not release SCL");

	zassert_true(timing_stretch_count > 0U,
		     "I3: the stretch was never started");
	zassert_true(stretched_us > baseline_us,
		     "I3: the stretched transfer took %u us against a %u us "
		     "baseline, so the controller did not wait",
		     stretched_us, baseline_us);

	cost_us = stretched_us - baseline_us;
	TC_PRINT("  I3: a %u us stretch cost %u us (%u us against a %u us "
		 "baseline)\n", (uint32_t)CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US,
		 cost_us, stretched_us, baseline_us);

	check_bound("I3 stretch cost in us", cost_us,
		    CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US,
		    (uint32_t)CONFIG_I2C_FAULT_TEST_STRETCH_SHORT_US * 4U);
#endif
} /* test_timing_stretch_cost */

/**
 * @brief I4 - how long a recovery attempt takes on a healthy bus.
 *
 * @details
 * Stimulus:
 * - Call i2c_recover_bus() once to confirm it is implemented, then time a
 *   second call.
 *
 * On the bus:
 * - Nine SCL pulses and a STOP, on a bus where nothing is stuck.
 *
 * Expected:
 * - The call succeeds, the bus is still usable afterwards, and the duration
 *   is printed. No bound is applied to the duration itself.
 *
 * The figure is reported rather than checked because there is no defensible
 * bound: the sequence is nine clock periods plus whatever the driver spends
 * reconfiguring pins, and both vary by orders of magnitude across drivers.
 * The value of recording it is that a caller deciding whether to attempt
 * recovery on a hot path has no other source for the number.
 *
 * The bus is checked either side of the measurement. Without the check
 * before, a duration can be reported for a bus that was already unusable,
 * which answers a question nobody asked. Without the check after, a driver
 * whose recovery returns success and leaves the controller stranded is
 * recorded as a clean measurement.
 *
 * Requires: a driver implementing recovery, and a bus still usable when the
 * condition starts. No fixture.
 */
ZTEST(i2c_fault_timing, test_timing_recovery_duration)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_timing_span span;
	int ret;

	if (!i2c_fault_bus_healthy()) {
		TC_PRINT("  the bus was already unusable when this condition "
			 "started, so a recovery duration measured on it would "
			 "mean nothing\n");
		ztest_test_skip();
		return;
	}

	if (i2c_recover_bus(ctx->dut) == -ENOSYS) {
		TC_PRINT("  the driver implements no bus recovery\n");
		ztest_test_skip();
		return;
	}

	i2c_timing_begin(&span);
	ret = i2c_recover_bus(ctx->dut);
	i2c_timing_end(&span);

	zassert_ok(ret, "I4: recovery on a healthy bus failed (%d)", ret);
	i2c_timing_report("I4 recovery duration", i2c_timing_us(&span), "us");
	i2c_fault_assert_healthy("I4");
} /* test_timing_recovery_duration */

/**
 * @brief I5 - how long the driver takes to report a fault.
 *
 * @details
 * Stimulus:
 * - Time a faulted write, a faulted read and a faulted write-read to the
 *   absent address, each on the bounded harness.
 *
 * On the bus:
 * - Three NACKed address phases. In each case the fault is known to the
 *   controller within one bit period of the acknowledge clock, so anything
 *   beyond that is time the driver spent deciding what to do about it.
 *
 * Expected:
 * - The three latencies are printed. No bounds are applied.
 *
 * Recorded for each shape separately because they are not equivalent: a
 * driver may report an address NACK on a write promptly and take much longer
 * over the same NACK on a write-read, where it has a second message queued to
 * unwind. A caller with a real-time budget needs the worst of the three, not
 * an average.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_timing, test_timing_fault_latency)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0x15U);

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = msgs;

	msgs[0].buf = tx_buf;
	msgs[0].len = sizeof(tx_buf);
	msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;
	xfer.num_msgs = 1U;
	i2c_fault_transfer_bounded(&xfer, "I5 write", &outcome);
	i2c_timing_report("I5 latency to report a faulted write",
			  outcome.elapsed_us, "us");

	msgs[0].flags = I2C_MSG_READ | I2C_MSG_STOP;
	xfer.num_msgs = 1U;
	i2c_fault_transfer_bounded(&xfer, "I5 read", &outcome);
	i2c_timing_report("I5 latency to report a faulted read",
			  outcome.elapsed_us, "us");

	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = tx_buf;
	msgs[1].len = sizeof(tx_buf);
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;
	xfer.num_msgs = ARRAY_SIZE(msgs);
	i2c_fault_transfer_bounded(&xfer, "I5 write-read", &outcome);
	i2c_timing_report("I5 latency to report a faulted write-read",
			  outcome.elapsed_us, "us");
} /* test_timing_fault_latency */
