/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * API misuse, conditions H1 to H5 of the suite.
 *
 * These need no fault target and no wiring, only a controller. They record
 * what a driver does when a caller gets the API wrong, which is the part of
 * driver behaviour least likely to be exercised anywhere else.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "i2c_fault.h"
#include "i2c_timing.h"

/* Speed code 0x6 sits between I2C_SPEED_ULTRA and the I2C_SPEED_DT sentinel. */
#define UNASSIGNED_SPEED 0x6U

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];

/*
 * A target registered purely to check that registration works still has to
 * survive being addressed, so the shared no-op callback table is used rather
 * than a table of NULL pointers.
 */

ZTEST_SUITE(i2c_fault_api_misuse, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/*
 * H1: a NULL buffer with a non-zero length.
 *
 * The I2C API performs no validation on this path outside of userspace, so a
 * driver is free to dereference the pointer or hand it to a DMA engine. That
 * can fault the CPU or corrupt memory, which would end the run rather than
 * report a result, so the condition is opt-in through
 * CONFIG_I2C_FAULT_TEST_UNSAFE_API_MISUSE and skips by default.
 */
/**
 * @brief H1 - a NULL buffer with a non-zero length.
 *
 * @details
 * Stimulus:
 * - A write of I2C_FAULT_PAYLOAD_LEN bytes whose msg.buf is NULL.
 *
 * On the bus:
 * - Ideally nothing. The call should be rejected before any waveform is
 *   generated. What actually happens depends on the configuration.
 *
 * Expected:
 * - With CONFIG_USERSPACE the syscall handler validates caller buffers before
 *   the driver is reached, so the call must fail, and that is asserted.
 * - Without it the API performs no validation on this path and the driver is
 *   free to dereference the pointer or hand it to a DMA engine, which can
 *   fault the CPU and end the run rather than report a result. The condition
 *   is then opt-in through CONFIG_I2C_FAULT_TEST_UNSAFE_API_MISUSE and skips
 *   by default.
 * - The bus is usable afterwards.
 *
 * The split is deliberate: coverage of the specified behaviour is retained,
 * and only the unspecified behaviour is made opt-in.
 *
 * Requires: one I2C controller, and either CONFIG_USERSPACE or the opt-in.
 */
ZTEST(i2c_fault_api_misuse, test_misuse_null_buffer)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	if (!IS_ENABLED(CONFIG_USERSPACE) &&
	    !IS_ENABLED(CONFIG_I2C_FAULT_TEST_UNSAFE_API_MISUSE)) {
		TC_PRINT("  without CONFIG_USERSPACE the API does not "
			 "validate this argument and the driver may "
			 "dereference it; enable "
			 "CONFIG_I2C_FAULT_TEST_UNSAFE_API_MISUSE to run "
			 "this condition anyway\n");
		ztest_test_skip();
		return;
	}

	msg.buf = NULL;
	msg.len = sizeof(tx_buf);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer,
				   "H1 NULL buffer with non-zero length",
				   &outcome);
	i2c_fault_record("H1 NULL buffer with non-zero length", outcome.ret);

	if (IS_ENABLED(CONFIG_USERSPACE)) {
		zassert_true(outcome.ret < 0,
			     "H1: with CONFIG_USERSPACE the syscall handler "
			     "must reject a NULL buffer, got %d",
			     outcome.ret);
	}

	i2c_fault_assert_healthy("H1");
} /* test_misuse_null_buffer */

/**
 * @brief H2 - a transfer with no messages.
 *
 * @details
 * Stimulus:
 * - i2c_transfer() with num_msgs of zero and an otherwise valid message
 *   array.
 *
 * On the bus:
 * - Nothing at all. No START is generated and the driver is never entered.
 *
 * Expected:
 * - Exactly 0.
 *
 * This is the documented contract, not an inference:
 * z_impl_i2c_transfer() in include/zephyr/drivers/i2c.h returns 0 immediately
 * when num_msgs is zero. Callers that build message arrays dynamically can
 * legitimately produce an empty one, and they rely on it being a no-op rather
 * than an error. The condition guards that behaviour against a future change.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_api_misuse, test_misuse_zero_messages)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_msg msg;
	int ret;

	msg.buf = tx_buf;
	msg.len = sizeof(tx_buf);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	ret = i2c_transfer(ctx->dut, &msg, 0U, ctx->absent_addr);
	i2c_fault_record("H2 transfer with zero messages", ret);
	zassert_ok(ret,
		   "H2: a transfer of no messages is defined as a no-op "
		   "returning 0, got %d",
		   ret);
	i2c_fault_assert_healthy("H2");
} /* test_misuse_zero_messages */

/**
 * @brief H3 - a zero-length message in the middle of a sequence.
 *
 * @details
 * Stimulus:
 * - A three-message write to the absent address where the middle message has
 *   a valid buffer but a length of zero.
 *
 * On the bus:
 * - One address phase, NACKed, so in practice nothing further is emitted. On
 *   a bus where the address answered, the empty message should contribute no
 *   bytes and the third message should follow the first as though it were not
 *   there.
 *
 * Expected:
 * - The transfer returns within the time bound and the bus is usable
 *   afterwards. The value is recorded; nothing in the API forbids an empty
 *   message, and nothing defines a particular result.
 *
 * The defect this catches is a driver that miscounts messages around an empty
 * one: dividing by the message length, treating zero as "use the whole
 * buffer", or advancing its index twice. Those show up as a hang or as the
 * following message being sent in the wrong direction.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_api_misuse, test_misuse_zero_length_middle)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[3];

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0x83U);

	msgs[0].buf = tx_buf;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = tx_buf;
	msgs[1].len = 0U;
	msgs[1].flags = I2C_MSG_WRITE;
	msgs[2].buf = tx_buf;
	msgs[2].len = 1U;
	msgs[2].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	i2c_fault_transfer_bounded(&xfer,
				   "H3 zero-length message mid-sequence",
				   &outcome);
	i2c_fault_record("H3 zero-length message mid-sequence", outcome.ret);
	i2c_fault_assert_healthy("H3");
} /* test_misuse_zero_length_middle */

/**
 * @brief H4 - a speed code the API does not assign.
 *
 * @details
 * Stimulus:
 * - Read the current configuration, rewrite its speed field with code 0x6,
 *   pass that to i2c_configure(), then restore the original.
 *
 * On the bus:
 * - Nothing directly. A driver that accepts the value may reprogram its clock
 *   divider to something unintended, which would only be visible on a later
 *   transfer, so the original configuration is restored immediately and the
 *   bus is checked afterwards.
 *
 * Expected:
 * - The value returned is recorded; the API does not say what an unassigned
 *   speed code must produce.
 * - Restoring the original configuration succeeds, and the bus is usable.
 *
 * Code 0x6 is the one value in the three-bit speed field that
 * include/zephyr/drivers/i2c.h leaves unassigned, sitting between
 * I2C_SPEED_ULTRA and the I2C_SPEED_DT sentinel. That makes it the only
 * genuinely invalid input available: every other code has a meaning.
 *
 * Requires: a driver implementing i2c_get_config(). No fixture.
 */
ZTEST(i2c_fault_api_misuse, test_misuse_invalid_speed)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	uint32_t original;
	uint32_t bad;
	int ret;

	ret = i2c_get_config(ctx->dut, &original);
	if (ret == -ENOSYS) {
		TC_PRINT("  the driver does not implement i2c_get_config()\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(ret, "H4: i2c_get_config() failed (%d)", ret);

	bad = (original & ~(uint32_t)I2C_SPEED_MASK) |
	      I2C_SPEED_SET(UNASSIGNED_SPEED);

	ret = i2c_configure(ctx->dut, bad);
	i2c_fault_record("H4 unassigned speed code", ret);

	zassert_ok(i2c_configure(ctx->dut, original),
		   "H4: the original configuration 0x%08x could not be "
		   "restored",
		   original);
	i2c_fault_assert_healthy("H4");
} /* test_misuse_invalid_speed */

/**
 * @brief H5 - the target API on a controller that may not implement it.
 *
 * @details
 * Stimulus:
 * - Register a no-op target at the unused address, then unregister it.
 *
 * On the bus:
 * - Nothing. The address chosen is the one the suite established nothing
 *   answers, so even if traffic arrived during the window it would collide
 *   with no real device.
 *
 * Expected:
 * - Either -ENOSYS, which is the documented value for a controller without
 *   target mode and ends the condition there, or 0 followed by a successful
 *   unregistration.
 *
 * Both halves of the contract are covered rather than skipping the capable
 * case: a controller that can register a target must also be able to remove
 * it, and a driver that registers successfully but cannot unregister leaves
 * the address claimed for the rest of the run.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_api_misuse, test_misuse_target_api_unsupported)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_target_config cfg = {
		.callbacks = &i2c_fault_noop_callbacks,
	};
	int ret;

	/*
	 * An address that answers already would collide with a real device,
	 * so the unused one the suite discovered is reused here.
	 */
	cfg.address = ctx->absent_addr;

	ret = i2c_target_register(ctx->dut, &cfg);
	i2c_fault_record("H5 target registration", ret);

	if (ret == -ENOSYS) {
		return;
	}

	zassert_ok(ret,
		   "H5: target registration returned %d; the API defines 0 "
		   "for success and -ENOSYS where target mode is absent",
		   ret);
	zassert_ok(i2c_target_unregister(ctx->dut, &cfg),
		   "H5: a target that registered could not be unregistered");
	i2c_fault_assert_healthy("H5");
} /* test_misuse_target_api_unsupported */
