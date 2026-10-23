/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * State integrity after a fault, conditions G1 to G5 of the suite.
 *
 * Each condition provokes a fault and then asks whether the driver is still
 * the object it was beforehand: does it still transfer, does it still report
 * its configuration, can it still be reconfigured, and does it still accept a
 * target registration. A driver that fails one of these has kept state from
 * the failure.
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

static uint8_t rx_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t expected_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t offset_byte;
#endif

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];

/* Job that writes to the absent address, the fault used throughout this file. */
static int absent_write_job(void *arg)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	ARG_UNUSED(arg);

	return i2c_write(ctx->dut, tx_buf, sizeof(tx_buf), ctx->absent_addr);
} /* absent_write_job() */

/* Provoke a fault and confirm it was met. */
static void provoke_fault(const char *condition)
{
	struct i2c_fault_outcome outcome;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0x60U);
	i2c_timing_run_or_fail(absent_write_job, NULL, condition, &outcome);
	zassert_true(outcome.ret < 0,
		     "%s: the write to absent 0x%02x succeeded",
		     condition, i2c_fault_ctx()->absent_addr);
} /* provoke_fault() */

ZTEST_SUITE(i2c_fault_integrity, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief G1 - a full round trip must still work after a fault.
 *
 * @details
 * Stimulus:
 * - Provoke a fault, then preload the target with a known pattern and perform
 *   a write-read on the bounded harness.
 *
 * On the bus:
 * - A NACKed address phase, then a complete correct transaction: address,
 *   offset byte, repeated START, and the payload read back.
 *
 * Expected:
 * - The round trip succeeds and returns exactly the preloaded bytes.
 *
 * G2 to G4 examine particular pieces of driver state. This asks the question
 * a caller actually cares about: after a failure, does the driver still work?
 * It runs on the bounded harness because it directly follows a fault.
 *
 * Requires: a fault target that answers the controller under test. Fixture:
 * i2c_bus_short.
 */
ZTEST(i2c_fault_integrity, test_integrity_transfer)
{
	i2c_fault_require_target_reachable();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;

	provoke_fault("G1 fault before a known-good transfer");

	offset_byte = 0U;
	i2c_fault_fill(expected_buf, sizeof(expected_buf), 0x51U);
	zassert_ok(i2c_target_fault_set_data(ctx->target, expected_buf,
					     sizeof(expected_buf)),
		   "could not preload the target buffer");
	(void)memset(rx_buf, 0x00U, sizeof(rx_buf));

	i2c_fault_write_read_bounded(ctx->target_addr, &offset_byte, 1U,
				     rx_buf, sizeof(rx_buf),
				     "G1 round trip after a fault", &outcome);
	zassert_ok(outcome.ret,
		   "G1: the round trip after a fault failed (%d)",
		   outcome.ret);
	zassert_mem_equal(rx_buf, expected_buf, sizeof(expected_buf),
			  "G1: the round trip after a fault returned the "
			  "wrong data");
#endif
} /* test_integrity_transfer */

/**
 * @brief G2 - the controller must still report its configured speed.
 *
 * @details
 * Stimulus:
 * - Read the configuration with i2c_get_config(), provoke a fault, read it
 *   again and compare.
 *
 * On the bus:
 * - One NACKed address phase between the two reads. i2c_get_config() itself
 *   touches nothing on the wire.
 *
 * Expected:
 * - The two values are identical.
 *
 * A driver that reprograms its peripheral during error handling, for instance
 * by reinitialising it wholesale, can come back with a different bit rate or
 * with target mode disabled. Nothing on the bus reveals that until the next
 * transfer goes out at the wrong speed, so it is checked directly.
 *
 * Requires: a driver implementing i2c_get_config(). No fixture.
 */
ZTEST(i2c_fault_integrity, test_integrity_get_config)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t before;
	uint32_t after;
	int ret;

	ret = i2c_get_config(ctx->dut, &before);
	if (ret == -ENOSYS) {
		TC_PRINT("  the driver does not implement i2c_get_config()\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(ret, "G2: i2c_get_config() failed before the fault (%d)",
		   ret);

	provoke_fault("G2 fault before reading the configuration back");

	zassert_ok(i2c_get_config(ctx->dut, &after),
		   "G2: i2c_get_config() failed after the fault");
	zassert_equal(after, before,
		      "G2: the configuration changed from 0x%08x to 0x%08x "
		      "across a faulted transfer",
		      before, after);
} /* test_integrity_get_config */

/**
 * @brief G3 - the controller must still accept a reconfiguration.
 *
 * @details
 * Stimulus:
 * - Read the current configuration, provoke a fault, then write the same
 *   configuration back with i2c_configure().
 *
 * On the bus:
 * - One NACKed address phase. Reprogramming the controller may briefly
 *   release and reacquire the pins, which on an idle bus is invisible.
 *
 * Expected:
 * - i2c_configure() returns 0, and the bus is usable afterwards.
 *
 * G2 asks whether the driver still knows its configuration; this asks whether
 * it will still act on one. A driver holding an internal "busy" or "error"
 * flag from the fault typically refuses the call, which strands a caller that
 * wanted to change speed as part of its own recovery.
 *
 * Requires: a driver implementing i2c_get_config(). No fixture.
 */
ZTEST(i2c_fault_integrity, test_integrity_reconfigure)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t original;
	int ret;

	ret = i2c_get_config(ctx->dut, &original);
	if (ret == -ENOSYS) {
		TC_PRINT("  the driver does not implement i2c_get_config()\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(ret, "G3: i2c_get_config() failed (%d)", ret);

	provoke_fault("G3 fault before reconfiguring");

	ret = i2c_configure(ctx->dut, original);
	i2c_fault_record("G3 reconfiguration after a fault", ret);
	zassert_ok(ret,
		   "G3: the controller refused its own configuration "
		   "0x%08x after a faulted transfer (%d)",
		   original, ret);
	i2c_fault_assert_healthy("G3");
} /* test_integrity_reconfigure */

/**
 * @brief G4 - target registration must still work after a fault.
 *
 * @details
 * Stimulus:
 * - Register a no-op target at the unused address and unregister it, to show
 *   the operation works at all.
 * - Provoke a fault, then repeat the register and unregister pair.
 *
 * On the bus:
 * - A NACKed address phase between the two pairs. Registration itself is a
 *   local operation; the address chosen is one nothing answers, so even if
 *   something addressed it during the window nothing else would respond.
 *
 * Expected:
 * - Both pairs succeed, or the controller reports -ENOSYS for target mode, in
 *   which case the condition skips.
 *
 * A controller keeps its target address match and its target-mode enable in
 * the same peripheral the controller-mode fault just occurred in. A driver
 * that resets that peripheral to clear the fault silently drops any
 * registered target with it, which nothing else in the suite would notice.
 *
 * The callback table is a working no-op set rather than NULL pointers,
 * because some controllers call into it unconditionally on any bus event.
 *
 * Requires: a controller implementing target mode. No fixture.
 */
ZTEST(i2c_fault_integrity, test_integrity_target_registration)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_target_config cfg = {
		.callbacks = &i2c_fault_noop_callbacks,
	};
	int ret;

	cfg.address = ctx->absent_addr;

	ret = i2c_target_register(ctx->dut, &cfg);
	if (ret == -ENOSYS) {
		TC_PRINT("  the controller implements no target mode\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(ret, "G4: target registration failed before the fault (%d)",
		   ret);
	zassert_ok(i2c_target_unregister(ctx->dut, &cfg),
		   "G4: target unregistration failed before the fault");

	provoke_fault("G4 fault before re-registering a target");

	zassert_ok(i2c_target_register(ctx->dut, &cfg),
		   "G4: target registration failed after the fault");
	zassert_ok(i2c_target_unregister(ctx->dut, &cfg),
		   "G4: target unregistration failed after the fault");
	i2c_fault_assert_healthy("G4");
} /* test_integrity_target_registration */

/**
 * @brief G5 - repeated fault-and-recover cycles.
 *
 * @details
 * Stimulus, repeated CONFIG_I2C_FAULT_TEST_STRESS_CYCLES times:
 * - provoke an address-phase fault,
 * - where the forcing GPIOs are wired, also hold SDA low across a submitted
 *   transfer, release it, and attempt recovery,
 * - check the bus is usable.
 *
 * On the bus:
 * - An alternation of NACKed address phases, hung-bus attempts, nine-pulse
 *   recovery sequences and successful probes, repeated until any per-cycle
 *   leak has accumulated enough to be visible.
 *
 * Expected:
 * - Every cycle ends with a usable bus, and no transfer outruns the time
 *   bound.
 *
 * Every other condition in this file applies one fault and then looks. This
 * one exists because the failures worth catching are rarely visible after a
 * single fault: a semaphore given one too many times, a pin left
 * reconfigured, a queue index that creeps. Those need repetition to surface.
 *
 * Requires: one I2C controller. The GPIO half is skipped without the
 * i2c_bus_fault fixture.
 */
ZTEST(i2c_fault_integrity, test_integrity_stress)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	bool recovered;

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_STRESS_CYCLES; i++) {
		provoke_fault("G5 stress cycle");

		if (ctx->bus_force_ready) {
			i2c_fault_fill(tx_buf, sizeof(tx_buf), (uint8_t)i);

			zassert_ok(bus_force_low(BUS_FORCE_SDA),
				   "G5: could not hold SDA low on cycle %u", i);
			zassert_ok(i2c_timing_submit(absent_write_job, NULL),
				   "G5: could not submit on cycle %u", i);

			(void)i2c_timing_collect(&outcome);

			zassert_ok(bus_force_release(),
				   "G5: could not release SDA on cycle %u", i);
			recovered = i2c_timing_harness_recover();

			zassert_false(outcome.timed_out,
				      "G5: cycle %u did not return within %d "
				      "ms and %s once SDA was released",
				      i, CONFIG_I2C_FAULT_TEST_TIMEOUT_MS,
				      recovered ? "only returned"
						: "never returned");

			if (i == 0U) {
				i2c_fault_record("G5 recovery after the hold",
						 i2c_recover_bus(ctx->dut));
			} else {
				(void)i2c_recover_bus(ctx->dut);
			}
		}

		i2c_fault_assert_healthy("G5");
	}
} /* test_integrity_stress */
