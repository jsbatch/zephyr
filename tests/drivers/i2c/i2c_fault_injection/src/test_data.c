/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Data-phase faults, conditions B1 to B8 of the suite.
 *
 * Every condition here needs a target that answers and then misbehaves
 * partway through the transaction, which no absent address can produce. They
 * therefore run only on the controllers that implement target mode and skip
 * elsewhere.
 *
 * The target treats the first byte of a write as a buffer offset and stores
 * the rest, so a NACK at index zero refuses the offset byte and a NACK at a
 * higher index refuses payload.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "i2c_fault.h"
#include "i2c_timing.h"

#if I2C_FAULT_HAS_TARGET
#include <zephyr/drivers/i2c/target/fault.h>

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t rx_buf[I2C_FAULT_PAYLOAD_LEN];

/*
 * Buffers handed to the bounded harness must outlive the test that submitted
 * them, because a transfer that outruns the bound is left running and the
 * controller may still be walking them.
 */
static uint8_t offset_byte;
static uint8_t expected_buf[I2C_FAULT_PAYLOAD_LEN];
#endif

#if I2C_FAULT_HAS_TARGET

static void arm(enum i2c_target_fault_kind kind, uint16_t index,
		uint16_t repeat)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_target_fault_config fault = {
		.kind = kind,
		.trigger_index = index,
		.repeat_count = repeat,
		.refuse_errno = -EIO,
	};

	zassert_ok(i2c_target_fault_arm(ctx->target, &fault),
		   "could not arm fault kind %d at index %u", (int)kind, index);
} /* arm() */

/* Write the payload to the target and return what the controller reported. */
static void write_payload(size_t len, const char *condition,
			  struct i2c_fault_outcome *out)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	i2c_fault_fill(tx_buf, len, 0xB0U);
	tx_buf[0] = 0U;

	msg.buf = tx_buf;
	msg.len = len;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, condition, out);
} /* write_payload() */

/*
 * A refused byte that the target never saw would make the controller's
 * return value meaningless, so every data-phase condition checks that the
 * injection actually happened before asserting anything about the result.
 */
static void assert_injected(const char *condition)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();

	zassert_true(i2c_target_fault_injections(ctx->target) > 0U,
		     "%s: the fault was never injected", condition);
} /* assert_injected() */

/* Arm one write NACK, perform the write, and require a clean failure. */
static void expect_write_nack(uint16_t index, const char *condition)
{
	struct i2c_fault_outcome outcome;

	arm(I2C_TARGET_FAULT_NACK_WRITE_DATA, index,
	    I2C_TARGET_FAULT_REPEAT_FOREVER);
	write_payload(sizeof(tx_buf), condition, &outcome);
	assert_injected(condition);
	i2c_fault_assert_error(outcome.ret, condition);
} /* expect_write_nack() */

#endif /* I2C_FAULT_HAS_TARGET */

ZTEST_SUITE(i2c_fault_data, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief B1 - the target refuses the first byte of a write.
 *
 * @details
 * Stimulus:
 * - Arm the target to refuse the byte at index 0, then write
 *   I2C_FAULT_PAYLOAD_LEN bytes to it.
 *
 * On the bus:
 * - START and the address are acknowledged, so the transaction begins
 *   normally. The controller clocks out the first data byte; the target's
 *   callback returns an error, so the peripheral leaves SDA high on the
 *   acknowledge clock and the controller reads NACK. The transaction is
 *   abandoned at its very first payload byte, which is the earliest point a
 *   data-phase fault can occur.
 *
 * Expected:
 * - The fault was injected, confirmed from the target's counter.
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_first)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	expect_write_nack(0U, "B1 data NACK on first byte");
	i2c_fault_assert_healthy("B1");
#endif
} /* test_data_nack_first */

/**
 * @brief B2 - the target refuses a byte in the middle of a write.
 *
 * @details
 * Stimulus:
 * - Arm the target to refuse the byte at index len/2, then write
 *   I2C_FAULT_PAYLOAD_LEN bytes.
 *
 * On the bus:
 * - The address and the first half of the payload are acknowledged and
 *   stored. The refused byte is NACKed, and the controller must abandon the
 *   transaction with bytes still queued. This is the case where a driver has
 *   to unwind partially completed work, which the first-byte and last-byte
 *   variants do not exercise.
 *
 * Expected:
 * - The fault was injected.
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_middle)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	expect_write_nack((uint16_t)(sizeof(tx_buf) / 2U),
			  "B2 data NACK mid-transfer");
	i2c_fault_assert_healthy("B2");
#endif
} /* test_data_nack_middle */

/**
 * @brief B3 - the target refuses the last byte of a write.
 *
 * @details
 * Stimulus:
 * - Arm the target to refuse the byte at index len-1, then write
 *   I2C_FAULT_PAYLOAD_LEN bytes.
 *
 * On the bus:
 * - Every byte but the last is acknowledged. The final byte is NACKed, and
 *   the controller then issues the STOP it was going to issue anyway.
 *
 * The distinction matters because the waveform is nearly identical to a
 * successful write, differing only in the level of one acknowledge bit. A
 * driver that decides success from having sent every byte, rather than from
 * the acknowledge it read back, passes B1 and B2 and fails here.
 *
 * Expected:
 * - The fault was injected.
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_final)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	expect_write_nack((uint16_t)(sizeof(tx_buf) - 1U),
			  "B3 data NACK on final byte");
	i2c_fault_assert_healthy("B3");
#endif
} /* test_data_nack_final */

/**
 * @brief B4 - the same refusal repeated across transactions.
 *
 * @details
 * Stimulus:
 * - Arm a refusal at index 1 that applies to every transaction, then perform
 *   CONFIG_I2C_FAULT_TEST_REPEAT_COUNT identical writes.
 *
 * On the bus:
 * - Each transaction is an identical START, address, one acknowledged byte,
 *   one NACKed byte, STOP. Nothing about the bus differs between rounds.
 *
 * Expected:
 * - Every attempt fails, and every attempt returns the same value as the
 *   first. A different value on a later round means the driver carried state
 *   out of the earlier failure.
 * - The fault was injected at least once.
 * - The bus is usable afterwards.
 *
 * B1 to B3 place a single fault. This one asks whether recovering from one
 * leaves the driver able to meet the next.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_repeated)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;
	int first_ret = 0;

	arm(I2C_TARGET_FAULT_NACK_WRITE_DATA, 1U,
	    I2C_TARGET_FAULT_REPEAT_FOREVER);

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		write_payload(sizeof(tx_buf), "B4 repeated data NACK",
			      &outcome);
		zassert_true(outcome.ret < 0,
			     "B4: attempt %u succeeded despite the refusal",
			     i);

		if (i == 0U) {
			first_ret = outcome.ret;
			i2c_fault_record("B4 repeated data NACK", first_ret);
		} else {
			zassert_equal(outcome.ret, first_ret,
				      "B4: attempt %u returned %d after the "
				      "first returned %d",
				      i, outcome.ret, first_ret);
		}
	}

	assert_injected("B4 repeated data NACK");
	i2c_fault_assert_healthy("B4");
#endif
} /* test_data_nack_repeated */

/*
 * B5: the refusal lands in the second message of a multi-message transfer.
 *
 * The two messages are written back to back without a repeated START, so the
 * target sees one continuous byte stream and the trigger index counts across
 * the boundary. That is deliberate: the condition under test is the
 * controller's handling of a fault that arrives after it has already
 * completed one message.
 */
/**
 * @brief B5 - the refusal lands in the second message of a transfer.
 *
 * @details
 * Stimulus, in two passes:
 * - Pass one arms nothing, performs a two-message write (one offset byte,
 *   then the payload), and reads the target's event trace back.
 * - Pass two arms a refusal at the index the trace showed, and repeats the
 *   transfer.
 *
 * On the bus:
 * - The API lets a driver place two adjacent same-direction writes as one
 *   addressed transaction, in which case the target sees a single continuous
 *   byte stream and the second message begins partway through it. Or the
 *   driver may issue a repeated START or a STOP and a fresh START, in which
 *   case the target sees a second address phase and its byte index restarts.
 * - The two produce different waveforms and put the same logical byte at
 *   different indices, which is why the index cannot be chosen in advance.
 *
 * Expected:
 * - Pass one succeeds and records callbacks.
 * - Pass two injects the fault and the transfer returns -EIO, or any negative
 *   value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * The condition is what it is because a fixed index would be wrong on half
 * the drivers, and skipping on those would turn a legitimate driver
 * difference into missing coverage.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_second_message)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_target_fault_event event;
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];
	size_t events;
	uint16_t trigger;
	bool split = false;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xB5U);

	msgs[0].buf = &offset_byte;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = tx_buf;
	msgs[1].len = sizeof(tx_buf);
	msgs[1].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	/* First pass: observe how the driver placed the two messages. */
	i2c_target_fault_disarm(ctx->target);
	i2c_target_fault_reset(ctx->target);
	i2c_fault_transfer_bounded(&xfer, "B5 observation pass", &outcome);
	zassert_ok(outcome.ret, "B5: the observation pass failed (%d)",
		   outcome.ret);

	events = i2c_target_fault_event_count(ctx->target);
	zassert_true(events > 0U,
		     "B5: the target recorded no callbacks, so the transfer "
		     "never reached it");
	zassert_false(i2c_target_fault_overflowed(ctx->target),
		      "B5: the trace overflowed during the observation pass; "
		      "raise CONFIG_I2C_TARGET_FAULT_TRACE_DEPTH");

	/* A second write_requested means the driver started a new operation. */
	for (size_t i = 1U; i < events; i++) {
		zassert_ok(i2c_target_fault_event(ctx->target, i, &event),
			   "B5: could not read trace entry %zu", i);

		if (event.type == I2C_TARGET_FAULT_EV_WRITE_REQUESTED) {
			split = true;
			break;
		}
	}

	if (split) {
		/* Indices restart, so the second message's payload is at 0. */
		trigger = (uint16_t)(sizeof(tx_buf) / 2U);
		TC_PRINT("  B5: the driver started a separate operation for "
			 "the second message; arming at index %u of it\n",
			 trigger);
	} else {
		/* One stream: index 0 is the offset byte from message one. */
		trigger = (uint16_t)(1U + (sizeof(tx_buf) / 2U));
		TC_PRINT("  B5: the driver merged the two writes; arming at "
			 "index %u of the combined stream\n", trigger);
	}

	/* Second pass: inject at the index the first pass established. */
	arm(I2C_TARGET_FAULT_NACK_WRITE_DATA, trigger,
	    I2C_TARGET_FAULT_REPEAT_FOREVER);
	i2c_target_fault_reset(ctx->target);

	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].flags = I2C_MSG_WRITE | I2C_MSG_STOP;
	i2c_fault_transfer_bounded(&xfer, "B5 NACK in the second message",
				   &outcome);

	assert_injected("B5 NACK in the second message");
	i2c_fault_assert_error(outcome.ret, "B5 NACK in the second message");
	i2c_fault_assert_healthy("B5");
#endif
} /* test_data_nack_second_message */

/**
 * @brief B6 - the target refuses every byte it is offered.
 *
 * @details
 * Stimulus:
 * - Arm a refusal with I2C_TARGET_FAULT_TRIGGER_EVERY, so no byte index is
 *   spared, then write I2C_FAULT_PAYLOAD_LEN bytes.
 *
 * On the bus:
 * - The address is acknowledged by hardware, then the very first data byte is
 *   NACKed, so in practice the transaction ends there and the later refusals
 *   never get the chance to apply. The value of the condition is that the
 *   target is in a state where nothing it is offered can succeed, which is
 *   what a full buffer or a wedged device looks like.
 *
 * Expected:
 * - The fault was injected.
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_nack_every_byte)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	struct i2c_fault_outcome outcome;

	arm(I2C_TARGET_FAULT_NACK_WRITE_DATA, I2C_TARGET_FAULT_TRIGGER_EVERY,
	    I2C_TARGET_FAULT_REPEAT_FOREVER);
	write_payload(sizeof(tx_buf), "B6 target NACKs every byte", &outcome);
	assert_injected("B6 target NACKs every byte");
	i2c_fault_assert_error(outcome.ret, "B6 target NACKs every byte");
	i2c_fault_assert_healthy("B6");
#endif
} /* test_data_nack_every_byte */

/**
 * @brief B7 - the target stops supplying bytes partway through a read.
 *
 * @details
 * Stimulus:
 * - Preload the target with a known pattern, arm it to refuse to supply the
 *   read byte at index len/2, then perform a write-read of
 *   I2C_FAULT_PAYLOAD_LEN bytes.
 *
 * On the bus:
 * - The read proceeds normally until the refused index. From there the target
 *   stops driving SDA, so the controller clocks in whatever the pull-ups
 *   hold, which is 0xFF. There is no way for the target to signal this: a
 *   read is acknowledged by the controller, not the target, so the controller
 *   receives no indication that anything is wrong. It simply reads ones.
 *
 * Expected:
 * - The fault was injected.
 * - If the transfer reports success, the byte at the refused index must not
 *   match the pattern the target holds. Reporting success and returning the
 *   refused data is the failure this guards against.
 * - The value returned is recorded, because a controller has no defined way
 *   to detect an under-supplying target.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_read_underrun)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];
	uint16_t trigger = (uint16_t)(sizeof(rx_buf) / 2U);

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xB7U);
	zassert_ok(i2c_target_fault_set_data(ctx->target, tx_buf,
					     sizeof(tx_buf)),
		   "could not preload the target buffer");
	(void)memset(rx_buf, 0x00U, sizeof(rx_buf));

	arm(I2C_TARGET_FAULT_FAIL_READ_DATA, trigger,
	    I2C_TARGET_FAULT_REPEAT_FOREVER);

	msgs[0].buf = &offset_byte;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = rx_buf;
	msgs[1].len = sizeof(rx_buf);
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	i2c_fault_transfer_bounded(&xfer, "B7 read underrun", &outcome);
	assert_injected("B7 read underrun");
	i2c_fault_record("B7 read underrun", outcome.ret);

	if (outcome.ret == 0) {
		zassert_not_equal(rx_buf[trigger], tx_buf[trigger],
				  "B7: the transfer reported success and "
				  "returned byte %u that the target refused "
				  "to supply",
				  trigger);
	}

	i2c_target_fault_disarm(ctx->target);
	i2c_fault_assert_healthy("B7");
#endif
} /* test_data_read_underrun */

/**
 * @brief B8 - a known-good transfer immediately after each refusal.
 *
 * @details
 * Stimulus, for every byte index in the payload in turn:
 * - arm a refusal at that index for one transaction only,
 * - perform the write, which must fail,
 * - preload the target with a fresh pattern,
 * - perform a write-read on the bounded harness and compare.
 *
 * On the bus:
 * - Each round is a faulted transaction ending in a NACK at a different byte
 *   position, immediately followed by a complete, correct transaction to the
 *   same address. Walking the fault across every index means no position is
 *   assumed to be equivalent to any other; a driver that unwinds correctly
 *   from a fault at index 0 may not from one at the last index.
 *
 * Expected:
 * - Every faulted write fails.
 * - Every following round trip succeeds and returns exactly the bytes
 *   preloaded, at every index.
 *
 * The round trip runs on the bounded harness because it directly follows a
 * fault, which is where a driver left holding its bus lock would stall.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_data, test_data_good_after_nack)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;

	offset_byte = 0U;

	for (uint16_t index = 0U; index < sizeof(tx_buf); index++) {
		arm(I2C_TARGET_FAULT_NACK_WRITE_DATA, index, 1U);
		write_payload(sizeof(tx_buf), "B8 refusal before a good "
			      "transfer", &outcome);
		zassert_true(outcome.ret < 0,
			     "B8: the write refused at index %u succeeded",
			     index);
		i2c_target_fault_disarm(ctx->target);

		i2c_fault_fill(expected_buf, sizeof(expected_buf),
			       (uint8_t)(0xC0U + index));
		zassert_ok(i2c_target_fault_set_data(ctx->target, expected_buf,
						     sizeof(expected_buf)),
			   "B8: could not preload the target buffer");
		(void)memset(rx_buf, 0x00U, sizeof(rx_buf));

		/*
		 * The read follows a refusal, so it is exactly where a driver
		 * left in a bad state would stall. It goes through the bounded
		 * harness for that reason.
		 */
		i2c_fault_write_read_bounded(ctx->target_addr, &offset_byte,
					     1U, rx_buf, sizeof(rx_buf),
					     "B8 read after a refusal",
					     &outcome);
		zassert_ok(outcome.ret,
			   "B8: the read after a refusal at index %u failed "
			   "(%d)",
			   index, outcome.ret);
		zassert_mem_equal(rx_buf, expected_buf, sizeof(expected_buf),
				  "B8: the read after a refusal at index %u "
				  "returned the wrong data",
				  index);
	}
#endif
} /* test_data_good_after_nack */
