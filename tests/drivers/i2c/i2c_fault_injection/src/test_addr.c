/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Address-phase faults, conditions A1 to A15 of the suite.
 *
 * Most of these need nothing but a controller and an address that nothing
 * answers, which is why they run on every driver in the tree. Existing
 * suites already cover the read case (tests/drivers/i2c/i2c_at24 and
 * tests/drivers/i2c/i2c_bme688 both assert -EIO from a read to a derived bad
 * address); what is added here is the write and write-read variants, the
 * repeated and alternating sequences, and a bound on how long a failure may
 * take.
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

static uint8_t tx_buf[I2C_FAULT_PAYLOAD_LEN];
static uint8_t rx_buf[I2C_FAULT_PAYLOAD_LEN];

/*
 * Buffers handed to the bounded harness must outlive the test that submitted
 * them, because a transfer that outruns the bound is left running and the
 * controller may still be walking them. They are file scope for that reason.
 */
static uint8_t probe_byte;

#if I2C_FAULT_HAS_TARGET
static uint8_t offset_byte;
#endif

/* Run a transfer under the time bound and assert it failed cleanly. */
static void expect_error(struct i2c_msg *msgs, uint8_t num_msgs, uint16_t addr,
			 const char *condition)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer = {
		.bus = ctx->dut,
		.addr = addr,
		.msgs = msgs,
		.num_msgs = num_msgs,
	};

	i2c_fault_transfer_bounded(&xfer, condition, &outcome);
	i2c_fault_assert_error(outcome.ret, condition);
} /* expect_error() */

ZTEST_SUITE(i2c_fault_addr, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief A15 - an unchanged stimulus must produce an unchanged result.
 *
 * @details
 * Stimulus:
 * - CONFIG_I2C_FAULT_TEST_REPEAT_COUNT zero-length writes to the absent
 *   address, issued back to back with no other bus traffic between them.
 *
 * On the bus:
 * - Each round is an ordinary START and address phase ending in a NACK and a
 *   STOP. Every round presents the controller with the same stimulus.
 *
 * Expected:
 * - Every round returns the same value as the first.
 *
 * This asserts nothing about which value is correct. The Zephyr API documents
 * only "0 on success" and "-EIO General input / output error" for
 * i2c_transfer(), and says nothing about whether success implies that a
 * target acknowledged, so requiring a particular value here would test one
 * implementation rather than the contract. Requiring only that the answer is
 * repeatable needs no such statement, and every conforming controller
 * satisfies it.
 *
 * It runs first in the suite because every later condition reads a return
 * value and infers something from it. A controller whose answer to an
 * unchanged stimulus varies makes all of those inferences unsound, so the
 * property is worth establishing before anything is built on it.
 *
 * The rounds are deliberately not separated by a health check, unlike the
 * other repeated conditions. This condition injects no fault, so there is
 * nothing to recover from, and a probe between rounds would add the very bus
 * traffic whose absence the condition depends on.
 *
 * A device coming out of reset partway through would also change the answer
 * and fail this. The suite cannot tell that apart from a controller defect,
 * so the failure reports the observation and not a cause.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_determinism)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	int first_ret = 0;

	msg.buf = &probe_byte;
	msg.len = 0U;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		i2c_fault_transfer_bounded(&xfer, "A15 repeated probe",
					   &outcome);

		if (i == 0U) {
			first_ret = outcome.ret;
			i2c_fault_record("A15 repeated probe", outcome.ret);
		} else {
			zassert_equal(outcome.ret, first_ret,
				      "A15: round %u of an unchanged "
				      "zero-length write to 0x%02x returned "
				      "%d where round 0 returned %d; the "
				      "controller does not answer an "
				      "unchanged stimulus consistently",
				      i, ctx->absent_addr, outcome.ret,
				      first_ret);
		}
	}
} /* test_addr_determinism */

/**
 * @brief A1 - a write to an address nothing answers must report an error.
 *
 * @details
 * Stimulus:
 * - One write of I2C_FAULT_PAYLOAD_LEN bytes to the absent address the suite
 *   discovered at setup, run on the bounded harness.
 *
 * On the bus:
 * - START, then seven address bits with R/W low. Nothing pulls SDA down
 *   during the ninth clock, so the controller samples a high and reads it as
 *   NACK. It issues STOP. No payload byte is ever clocked out.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value under
 *   CONFIG_I2C_FAULT_TEST_RELAXED_ERRNO, within the time bound.
 * - The bus is usable afterwards.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_write)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_msg msg;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA1U);

	msg.buf = tx_buf;
	msg.len = sizeof(tx_buf);
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	expect_error(&msg, 1U, ctx->absent_addr, "A1 address NACK on write");
	i2c_fault_assert_healthy("A1");
} /* test_addr_nack_write */

/**
 * @brief A2 - a read from an address nothing answers must report an error.
 *
 * @details
 * Stimulus:
 * - One read of I2C_FAULT_PAYLOAD_LEN bytes from the absent address.
 *
 * On the bus:
 * - START, then seven address bits with R/W high. Nothing acknowledges, so
 *   the controller reads NACK on the ninth clock and issues STOP without
 *   clocking any data in. The receive buffer must not be touched.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * This extends the coverage already in tests/drivers/i2c/i2c_at24 and
 * tests/drivers/i2c/i2c_bme688, which assert the same value but derive the
 * bad address arithmetically instead of discovering it.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_read)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_msg msg;

	msg.buf = rx_buf;
	msg.len = sizeof(rx_buf);
	msg.flags = I2C_MSG_READ | I2C_MSG_STOP;

	expect_error(&msg, 1U, ctx->absent_addr, "A2 address NACK on read");
	i2c_fault_assert_healthy("A2");
} /* test_addr_nack_read */

/**
 * @brief A3 - a write-read must report an error rather than stale data.
 *
 * @details
 * Stimulus:
 * - A two-message transfer to the absent address: a one-byte write, then a
 *   repeated START and a read of I2C_FAULT_PAYLOAD_LEN bytes. The receive
 *   buffer is pre-filled with 0x5A so that untouched memory is recognisable.
 *
 * On the bus:
 * - START and the address with R/W low are NACKed, so the transaction ends at
 *   the first address phase. The repeated START and the read phase never
 *   happen, and no byte is ever clocked in.
 *
 * Expected:
 * - The transfer returns -EIO, or any negative value in relaxed mode.
 * - The bus is usable afterwards.
 *
 * This is the shape PR #114443 corrected upstream, where a write-read to a
 * NACKing address returned data left over from an earlier transfer on
 * alternating attempts.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_write_read)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_msg msgs[2];
	struct i2c_fault_xfer xfer;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA3U);
	(void)memset(rx_buf, 0x5AU, sizeof(rx_buf));

	msgs[0].buf = tx_buf;
	msgs[0].len = 1U;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = rx_buf;
	msgs[1].len = sizeof(rx_buf);
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	i2c_fault_transfer_bounded(&xfer, "A3 address NACK on write-read",
				   &outcome);
	i2c_fault_assert_error(outcome.ret, "A3 address NACK on write-read");
	i2c_fault_assert_healthy("A3");
} /* test_addr_nack_write_read */

/**
 * @brief A4 - the target refuses the read that follows a repeated START.
 *
 * @details
 * Stimulus:
 * - The fault target is preloaded with a known pattern and armed to refuse
 *   the read from its address-match callback.
 * - A write-read to the target: one offset byte, repeated START, then a read
 *   of I2C_FAULT_PAYLOAD_LEN bytes.
 *
 * On the bus:
 * - The write phase is acknowledged normally and the offset byte is stored.
 * - The repeated START and the read address are acknowledged by hardware, but
 *   the refusal makes the target stop driving SDA. The controller then clocks
 *   in whatever the idle bus presents, which the pull-ups hold high, so it
 *   reads 0xFF rather than the stored pattern.
 *
 * Expected:
 * - The fault was injected at least once, otherwise the result means nothing.
 * - Either the transfer reports an error, or it reports success and the data
 *   differs from what the target holds. Reporting success *and* returning the
 *   refused data is the failure this guards against.
 * - The value returned is recorded, because the API mandates no particular
 *   one for this case.
 *
 * Requires: a fault target that can refuse a byte. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_addr, test_addr_nack_read_after_restart)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_target_fault_config fault = {
		.kind = I2C_TARGET_FAULT_NACK_READ_ADDR,
		.repeat_count = I2C_TARGET_FAULT_REPEAT_FOREVER,
		.refuse_errno = -EIO,
	};
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA4U);
	zassert_ok(i2c_target_fault_set_data(ctx->target, tx_buf,
					     sizeof(tx_buf)),
		   "could not preload the target buffer");
	(void)memset(rx_buf, 0x00U, sizeof(rx_buf));

	zassert_ok(i2c_target_fault_arm(ctx->target, &fault),
		   "could not arm the read-phase fault");

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

	i2c_fault_transfer_bounded(&xfer, "A4 read NACK after repeated START",
				   &outcome);
	i2c_fault_record("A4 read NACK after repeated START", outcome.ret);

	zassert_true(i2c_target_fault_injections(ctx->target) > 0U,
		     "A4: the fault was never injected, so the result says "
		     "nothing about the controller");

	if (outcome.ret == 0) {
		zassert_true(memcmp(rx_buf, tx_buf, sizeof(rx_buf)) != 0,
			     "A4: the transfer reported success and returned "
			     "the data the target refused to supply");
	}

	i2c_target_fault_disarm(ctx->target);
	i2c_fault_assert_healthy("A4");
#endif
} /* test_addr_nack_read_after_restart */

/**
 * @brief A5 - a NACK must not latch the controller state machine.
 *
 * @details
 * Stimulus:
 * - CONFIG_I2C_FAULT_TEST_REPEAT_COUNT consecutive one-byte writes to the
 *   absent address, with nothing else in between.
 *
 * On the bus:
 * - Each attempt is an identical START, address, NACK, STOP. Nothing about
 *   the bus changes between them, so nothing about the result should either.
 *
 * Expected:
 * - Every attempt fails.
 * - Every attempt returns the same value as the first. A driver that fails
 *   once and then succeeds, or that reports a different error the second
 *   time, has carried state out of the first failure.
 * - The bus is usable afterwards.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_persistent)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	int first_ret = 0;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA5U);

	msg.buf = tx_buf;
	msg.len = 1U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		i2c_fault_transfer_bounded(&xfer, "A5 persistent address NACK",
					   &outcome);
		zassert_true(outcome.ret < 0,
			     "A5: attempt %u to absent 0x%02x succeeded",
			     i, ctx->absent_addr);

		if (i == 0U) {
			first_ret = outcome.ret;
			i2c_fault_record("A5 persistent address NACK",
					 first_ret);
		} else {
			zassert_equal(outcome.ret, first_ret,
				      "A5: attempt %u returned %d after the "
				      "first returned %d",
				      i, outcome.ret, first_ret);
		}
	}

	i2c_fault_assert_healthy("A5");
} /* test_addr_nack_persistent */

/**
 * @brief A6 - a NACK must not disturb the transfer that follows it.
 *
 * @details
 * Stimulus, repeated CONFIG_I2C_FAULT_TEST_REPEAT_COUNT times:
 * - a one-byte write to the absent address, which must fail,
 * - a health check against the known-good address, which must succeed,
 * - a write-read to the absent address, which must fail,
 * - a second health check.
 *
 * On the bus:
 * - The failing transactions end at their address phase with a NACK and a
 *   STOP. The successful ones are a full addressed transaction to a device
 *   that answers. The two alternate, so any state the controller keeps from a
 *   failure has to survive only one good transaction to be caught.
 *
 * Expected:
 * - Every faulted attempt fails and every health check succeeds, on every
 *   round.
 *
 * tests/drivers/i2c/i2c_bme688 checks one read after one NACK. This adds the
 * write and write-read shapes and repeats them, because the defect class is
 * state carried from a failed transfer into the next one.
 *
 * Requires: one I2C controller and one device that answers. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_alternating)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msgs[2];

	i2c_fault_require_present();

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA6U);

	xfer.bus = ctx->dut;
	xfer.addr = ctx->absent_addr;
	xfer.msgs = msgs;

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		msgs[0].buf = tx_buf;
		msgs[0].len = 1U;
		msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;
		xfer.num_msgs = 1U;

		i2c_fault_transfer_bounded(&xfer, "A6 alternating write",
					   &outcome);
		zassert_true(outcome.ret < 0,
			     "A6: write to absent 0x%02x succeeded on round %u",
			     ctx->absent_addr, i);

		i2c_fault_assert_healthy("A6 after a failed write");

		msgs[0].buf = tx_buf;
		msgs[0].len = 1U;
		msgs[0].flags = I2C_MSG_WRITE;
		msgs[1].buf = rx_buf;
		msgs[1].len = sizeof(rx_buf);
		msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;
		xfer.num_msgs = ARRAY_SIZE(msgs);

		i2c_fault_transfer_bounded(&xfer, "A6 alternating write-read",
					   &outcome);
		zassert_true(outcome.ret < 0,
			     "A6: write-read to absent 0x%02x succeeded on "
			     "round %u",
			     ctx->absent_addr, i);

		i2c_fault_assert_healthy("A6 after a failed write-read");
	}
} /* test_addr_nack_alternating */

/**
 * @brief A7 - the bus-scan idiom must report an absent address as absent.
 *
 * @details
 * Stimulus:
 * - The canonical zero-length write with I2C_MSG_WRITE | I2C_MSG_STOP to the
 *   absent address, the same probe drivers/i2c/i2c_shell.c uses to scan.
 *
 * On the bus:
 * - START, address, ninth clock sampled high, STOP. Nothing follows, which is
 *   what makes the probe safe to point at an unknown address.
 *
 * Expected:
 * - The probe returns a negative value.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_probe_absent)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret = i2c_fault_probe(ctx->dut, ctx->absent_addr);

	i2c_fault_record("A7 zero-length probe, absent address", ret);
	zassert_true(ret < 0,
		     "A7: the zero-length probe reported 0x%02x present",
		     ctx->absent_addr);
} /* test_addr_probe_absent */

/**
 * @brief A8 - the same idiom must report a present address as present.
 *
 * @details
 * Stimulus:
 * - The same zero-length write, aimed at the known-good address instead.
 *
 * On the bus:
 * - START, address, and a device pulls SDA low through the ninth clock. The
 *   controller reads ACK and issues STOP.
 *
 * Expected:
 * - The probe returns 0.
 *
 * This is the counterpart to A7. Together they show the probe distinguishes
 * the two cases rather than always reporting one of them.
 *
 * Requires: one I2C controller and one device that answers. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_probe_present)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret;

	i2c_fault_require_present();

	ret = i2c_fault_probe(ctx->dut, ctx->present_addr);
	i2c_fault_record("A8 zero-length probe, present address", ret);
	zassert_ok(ret, "A8: the zero-length probe reported 0x%02x absent",
		   ctx->present_addr);
} /* test_addr_probe_present */

/**
 * @brief A9 - a 10-bit address nothing answers must fail like a 7-bit one.
 *
 * @details
 * Stimulus:
 * - A one-byte write carrying I2C_MSG_ADDR_10_BITS to
 *   CONFIG_I2C_FAULT_TEST_ABSENT_ADDR_10BIT.
 *
 * On the bus:
 * - START, then the 11110xx0 prefix byte carrying the top two address bits,
 *   then the low eight bits as a second byte. A 10-bit target acknowledges
 *   both; with none present the first byte is already NACKed and the
 *   controller stops there.
 *
 * Expected:
 * - The transfer returns a negative value, or -ENOTSUP / -ENOSYS on a
 *   controller without 10-bit support, in which case the condition skips.
 * - The bus is usable afterwards.
 *
 * The 10-bit space is effectively unpopulated in practice, so a fixed default
 * address is defensible here; it is validated by the same probe before use.
 *
 * Requires: one I2C controller with 10-bit addressing. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_nack_10bit)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xA9U);

	msg.buf = tx_buf;
	msg.len = 1U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP | I2C_MSG_ADDR_10_BITS;

	xfer.bus = ctx->dut;
	xfer.addr = (uint16_t)CONFIG_I2C_FAULT_TEST_ABSENT_ADDR_10BIT;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, "A9 10-bit address NACK", &outcome);
	i2c_fault_record("A9 10-bit address NACK", outcome.ret);

	if ((outcome.ret == -ENOTSUP) || (outcome.ret == -ENOSYS)) {
		TC_PRINT("  the controller does not support 10-bit "
			 "addressing\n");
		ztest_test_skip();
		return;
	}

	zassert_true(outcome.ret < 0,
		     "A9: the 10-bit address 0x%03x answered",
		     (unsigned int)CONFIG_I2C_FAULT_TEST_ABSENT_ADDR_10BIT);
	i2c_fault_assert_healthy("A9");
} /* test_addr_nack_10bit */

/**
 * @brief A10 - the general call address must not hang or corrupt the bus.
 *
 * @details
 * Stimulus:
 * - A zero-length write to address 0x00. Zero length is deliberate: a general
 *   call carries a command byte that any listening device acts on, and this
 *   condition must not reconfigure anything on the board.
 *
 * On the bus:
 * - START then eight zero bits. Whether anything acknowledges is a property
 *   of the board, not of the driver. STOP follows either way.
 *
 * Expected:
 * - The attempt returns within the time bound and the bus is usable
 *   afterwards.
 * - The value returned is recorded rather than asserted, because both
 *   outcomes are legitimate depending on what is fitted.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_general_call)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	/* Zero length keeps the attempt non-destructive to any listener. */
	msg.buf = &probe_byte;
	msg.len = 0U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = 0x00U;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, "A10 general call address",
				   &outcome);
	i2c_fault_record("A10 general call address", outcome.ret);
	i2c_fault_assert_healthy("A10");
} /* test_addr_general_call */

/**
 * @brief A11 - the addresses the I2C specification reserves.
 *
 * @details
 * Stimulus:
 * - A zero-length write to each of 0x01 to 0x07 and 0x78 to 0x7F in turn.
 *   These are the ranges NXP UM10204 rev. 6 section 3.1.12 reserves: CBUS,
 *   different bus formats, Hs-mode masters, device ID, and the 10-bit
 *   addressing prefixes. Address 0x00 is the general call and is A10.
 *
 * On the bus:
 * - Each is an ordinary START and address phase. The controller has no way to
 *   know the address is reserved, and nothing on a normal board answers, so
 *   each ends in a NACK and a STOP.
 *
 * Expected:
 * - Every attempt returns within the time bound and the bus is usable at the
 *   end.
 * - The values are recorded, not asserted. The Zephyr API places no
 *   requirement on a controller to reject a reserved address, so asserting
 *   rejection would test one implementation rather than the contract. The
 *   record feeds the error-code survey the suite exists to produce.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_addr, test_addr_reserved)
{
	static const uint16_t reserved[] = {
		0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
		0x78U, 0x79U, 0x7AU, 0x7BU, 0x7CU, 0x7DU, 0x7EU, 0x7FU,
	};
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	msg.buf = &probe_byte;
	msg.len = 0U;

	xfer.bus = ctx->dut;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	for (size_t i = 0U; i < ARRAY_SIZE(reserved); i++) {
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;
		xfer.addr = reserved[i];

		i2c_fault_transfer_bounded(&xfer, "A11 reserved address",
					   &outcome);
		TC_PRINT("  A11 reserved address 0x%02x: driver returned %d\n",
			 reserved[i], outcome.ret);
	}

	i2c_fault_assert_healthy("A11");
} /* test_addr_reserved */

#if I2C_FAULT_HAS_TARGET2
static const struct device *const target2 =
	DEVICE_DT_GET(DT_ALIAS(i2c_fault_target_2));
#endif

/**
 * @brief A12 - two targets answering one address.
 *
 * @details
 * Stimulus:
 * - A second fault target, named by the i2c-fault-target-2 alias and carrying
 *   the same reg as the first, is registered so both answer one address.
 * - A one-byte write is then addressed to it.
 *
 * On the bus:
 * - Both targets pull SDA low on the acknowledge clock. On an open-drain bus
 *   that is indistinguishable from one target answering, so the address phase
 *   looks normal. Any read would be the interesting case, because both would
 *   drive data and the wired-AND of two different bytes is neither.
 *
 * Expected:
 * - No particular return value is required; the API describes no behaviour
 *   for this arrangement.
 * - The transfer terminates within the time bound and the bus is usable
 *   afterwards. The second target is unregistered whatever happens.
 *
 * Requires: two fault targets on one address. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_addr, test_addr_two_targets)
{
	i2c_fault_require_target_reachable();

#if I2C_FAULT_HAS_TARGET2
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;
	uint16_t addr2 = (uint16_t)DT_REG_ADDR(DT_ALIAS(i2c_fault_target_2));
	int ret;

	if (addr2 != ctx->target_addr) {
		TC_PRINT("  the second fault target answers 0x%02x, not the "
			 "0x%02x this condition needs\n",
			 addr2, ctx->target_addr);
		ztest_test_skip();
		return;
	}

	ret = i2c_target_driver_register(target2);
	if (ret != 0) {
		TC_PRINT("  the second fault target could not be registered "
			 "(%d)\n", ret);
		ztest_test_skip();
		return;
	}

	i2c_fault_fill(tx_buf, sizeof(tx_buf), 0xACU);

	msg.buf = tx_buf;
	msg.len = 1U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx->dut;
	xfer.addr = ctx->target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	i2c_fault_transfer_bounded(&xfer, "A12 two targets on one address",
				   &outcome);
	i2c_fault_record("A12 two targets on one address", outcome.ret);

	(void)i2c_target_driver_unregister(target2);
	i2c_fault_assert_healthy("A12");
#else
	TC_PRINT("  no i2c-fault-target-2 alias; this condition needs a "
		 "second fault target on the same address\n");
	ztest_test_skip();
#endif
} /* test_addr_two_targets */

/**
 * @brief A13 - a device that was present stops answering, then answers again.
 *
 * @details
 * Stimulus:
 * - Probe the fault target and confirm it answers.
 * - Unregister it with i2c_target_unregister(), then probe again.
 * - Re-register it and probe a third time.
 *
 * On the bus:
 * - The first probe gets an ACK. After the unregistration the peripheral no
 *   longer matches that address, so nothing pulls SDA down on the ninth clock
 *   and the same probe now gets a NACK. Re-registering restores the match.
 *   The address itself, the wiring and the controller are unchanged
 *   throughout; only the device's willingness to answer moved.
 *
 * Expected:
 * - ACK, then NACK, then ACK.
 *
 * This is the fault class an absent address cannot produce, and which Section
 * 1.2 of the plan names as a gap: a target that answers and then does not. It
 * needs nothing of the controller beyond the registration API, so it runs
 * even where the byte callbacks cannot refuse anything.
 *
 * Requires: a fault target that answers the controller under test. Fixture:
 * i2c_bus_short.
 */
ZTEST(i2c_fault_addr, test_addr_present_device_withdrawn)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret;

	i2c_fault_require_target_reachable();

	zassert_ok(i2c_fault_probe(ctx->dut, ctx->target_addr),
		   "A13: the target did not answer before being withdrawn");

	ret = i2c_fault_target_withdraw();
	if (ret == -ENOSYS) {
		TC_PRINT("  the controller does not support unregistering a "
			 "target\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(ret, "A13: could not withdraw the target (%d)", ret);

	ret = i2c_fault_probe(ctx->dut, ctx->target_addr);
	i2c_fault_record("A13 withdrawn target", ret);
	zassert_true(ret < 0,
		     "A13: address 0x%02x still answered after its target was "
		     "unregistered",
		     ctx->target_addr);

	zassert_ok(i2c_fault_target_restore(),
		   "A13: could not restore the target");
	zassert_ok(i2c_fault_probe(ctx->dut, ctx->target_addr),
		   "A13: the target did not answer again after being "
		   "restored");
} /* test_addr_present_device_withdrawn */

/**
 * @brief A14 - one address alternates between answering and not.
 *
 * @details
 * Stimulus, repeated CONFIG_I2C_FAULT_TEST_REPEAT_COUNT times:
 * - probe the target, which must ACK,
 * - withdraw it, probe again, which must NACK,
 * - restore it.
 *
 * On the bus:
 * - Successive identical address phases to one address produce alternating
 *   ACK and NACK. Nothing else about the transaction changes.
 *
 * Expected:
 * - Every round shows ACK then NACK.
 *
 * A6 alternates between two different addresses, which a driver caching
 * per-address state could still pass. This alternates at a single address, so
 * a driver that remembers the outcome of an address phase is caught.
 *
 * Requires: a fault target that answers the controller under test. Fixture:
 * i2c_bus_short.
 */
ZTEST(i2c_fault_addr, test_addr_present_alternating)
{
	const struct i2c_fault_ctx *ctx = i2c_fault_ctx();
	int ret;

	i2c_fault_require_target_reachable();

	if (i2c_fault_target_withdraw() == -ENOSYS) {
		TC_PRINT("  the controller does not support unregistering a "
			 "target\n");
		ztest_test_skip();
		return;
	}

	zassert_ok(i2c_fault_target_restore(),
		   "A14: could not restore the target");

	for (uint32_t i = 0U; i < CONFIG_I2C_FAULT_TEST_REPEAT_COUNT; i++) {
		zassert_ok(i2c_fault_probe(ctx->dut, ctx->target_addr),
			   "A14: the target did not answer on round %u", i);

		zassert_ok(i2c_fault_target_withdraw(),
			   "A14: could not withdraw on round %u", i);

		ret = i2c_fault_probe(ctx->dut, ctx->target_addr);
		zassert_true(ret < 0,
			     "A14: 0x%02x answered while withdrawn on round "
			     "%u",
			     ctx->target_addr, i);

		zassert_ok(i2c_fault_target_restore(),
			   "A14: could not restore on round %u", i);
	}
} /* test_addr_present_alternating */
