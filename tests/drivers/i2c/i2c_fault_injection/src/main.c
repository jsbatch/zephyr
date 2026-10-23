/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * I2C fault injection suite: setup, capability probe and injector self-test.
 *
 * Zephyr's existing I2C tests validate that transfers succeed. This suite
 * validates what happens when they fail. It is vendor neutral: no board, SoC
 * or vendor appears in the test logic, and a board adopts it by contributing
 * a devicetree overlay.
 *
 * Three tiers of hardware are recognised, each a superset of the one above.
 * A single controller reaches the address-phase, API-misuse and timing
 * conditions. Two spare GPIOs jumpered to the bus add the hang, recovery and
 * clock-hold conditions. A second controller shorted to the same SDA and SCL
 * adds the data-phase and target-driven conditions. Conditions that the
 * present tier cannot produce are skipped, never silently passed.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#ifdef CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE
#ifndef CONFIG_CONSOLE_GETCHAR
#error "I2C_FAULT_TEST_SCOPE_PAUSE needs CONSOLE_GETCHAR; use scope_pause.conf"
#endif
#include <zephyr/console/console.h>
#endif /* CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE */

#include "bus_force.h"
#include "i2c_fault.h"
#include "i2c_timing.h"

#if I2C_FAULT_HAS_TARGET
#include <zephyr/drivers/i2c/target/fault.h>
#endif

/* Lowest and highest addresses the I2C specification leaves for general use. */
#define ADDR_SCAN_FIRST 0x08U
#define ADDR_SCAN_LAST  0x77U

static struct i2c_fault_ctx ctx;
static bool ctx_valid;

const struct i2c_fault_ctx *i2c_fault_ctx(void)
{
	return &ctx;
} /* i2c_fault_ctx() */

int i2c_fault_probe(const struct device *bus, uint16_t addr)
{
	struct i2c_msg msg;
	uint8_t dummy = 0U;

	msg.buf = &dummy;
	msg.len = 0U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	return i2c_transfer(bus, &msg, 1U, addr);
} /* i2c_fault_probe() */

void i2c_fault_fill(uint8_t *buf, size_t len, uint8_t seed)
{
	for (size_t i = 0U; i < len; i++) {
		buf[i] = (uint8_t)(seed + (uint8_t)i);
	}
} /* i2c_fault_fill() */

int i2c_fault_xfer_job(void *arg)
{
	const struct i2c_fault_xfer *xfer = arg;

	return i2c_transfer(xfer->bus, xfer->msgs, xfer->num_msgs, xfer->addr);
} /* i2c_fault_xfer_job() */

/*
 * Storage the bounded harness owns.
 *
 * A job that outruns the bound is deliberately left running, so anything the
 * controller may still touch has to outlive the test thread's stack frame.
 * The message array is copied here; the buffers the messages point at are the
 * caller's responsibility and are file-scope throughout the suite.
 */
static struct i2c_msg pinned_msgs[I2C_FAULT_MAX_MSGS];
static struct i2c_fault_xfer pinned_xfer;
static uint8_t pinned_scratch;

static void pin_transfer(const struct i2c_fault_xfer *xfer)
{
	zassert_true(xfer->num_msgs <= ARRAY_SIZE(pinned_msgs),
		     "a transfer of %u messages exceeds I2C_FAULT_MAX_MSGS",
		     xfer->num_msgs);

	(void)memcpy(pinned_msgs, xfer->msgs,
		     (size_t)xfer->num_msgs * sizeof(pinned_msgs[0]));

	pinned_xfer.bus = xfer->bus;
	pinned_xfer.addr = xfer->addr;
	pinned_xfer.msgs = pinned_msgs;
	pinned_xfer.num_msgs = xfer->num_msgs;
} /* pin_transfer() */

void i2c_fault_transfer_bounded(const struct i2c_fault_xfer *xfer,
				const char *condition,
				struct i2c_fault_outcome *out)
{
	pin_transfer(xfer);
	i2c_timing_run_or_fail(i2c_fault_xfer_job, &pinned_xfer, condition,
			       out);
} /* i2c_fault_transfer_bounded() */

int i2c_fault_transfer_submit(const struct i2c_fault_xfer *xfer)
{
	pin_transfer(xfer);

	return i2c_timing_submit(i2c_fault_xfer_job, &pinned_xfer);
} /* i2c_fault_transfer_submit() */

void i2c_fault_write_read_bounded(uint16_t addr, uint8_t *wr, size_t wr_len,
				  uint8_t *rd, size_t rd_len,
				  const char *condition,
				  struct i2c_fault_outcome *out)
{
	struct i2c_msg msgs[2];
	struct i2c_fault_xfer xfer;

	msgs[0].buf = wr;
	msgs[0].len = wr_len;
	msgs[0].flags = I2C_MSG_WRITE;
	msgs[1].buf = rd;
	msgs[1].len = rd_len;
	msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

	xfer.bus = ctx.dut;
	xfer.addr = addr;
	xfer.msgs = msgs;
	xfer.num_msgs = ARRAY_SIZE(msgs);

	i2c_fault_transfer_bounded(&xfer, condition, out);
} /* i2c_fault_write_read_bounded() */

/* Probe an address through the bounded harness. */
static int probe_bounded(uint16_t addr, struct i2c_fault_outcome *out)
{
	struct i2c_msg msg;
	struct i2c_fault_xfer xfer;

	msg.buf = &pinned_scratch;
	msg.len = 0U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx.dut;
	xfer.addr = addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	pin_transfer(&xfer);

	return i2c_timing_run_bounded(i2c_fault_xfer_job, &pinned_xfer, out);
} /* probe_bounded() */

/*
 * Distinguish an answer from a broken bus.
 *
 * A NACK means the address is absent, which is a result. -ETIMEDOUT means the
 * transfer never completed, which is a wiring or bus fault: nothing is
 * getting through, so no address can be classified and no later condition
 * would mean anything. Consecutive timeouts are counted so the suite can give
 * up rather than spend CONFIG_I2C_TRANSFER_TIMEOUT_MS on every remaining
 * address and every remaining test.
 */
static uint32_t consecutive_timeouts;

static void note_bus_result(int ret)
{
	if (ret == -ETIMEDOUT) {
		consecutive_timeouts++;
		if (consecutive_timeouts >=
		    (uint32_t)CONFIG_I2C_FAULT_TEST_MAX_BUS_TIMEOUTS) {
			ctx.bus_unusable = true;
		}
	} else {
		consecutive_timeouts = 0U;
	}
} /* note_bus_result() */

/*
 * A target registered purely to check that registration works still has to
 * survive being addressed, because some controllers call into the callback
 * table unconditionally when an event arrives. These do nothing and refuse
 * nothing.
 */
static int noop_write_requested(struct i2c_target_config *config)
{
	ARG_UNUSED(config);

	return 0;
} /* noop_write_requested() */

static int noop_write_received(struct i2c_target_config *config, uint8_t val)
{
	ARG_UNUSED(config);
	ARG_UNUSED(val);

	return 0;
} /* noop_write_received() */

static int noop_read_requested(struct i2c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);

	*val = 0xFFU;

	return 0;
} /* noop_read_requested() */

static int noop_read_processed(struct i2c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);

	*val = 0xFFU;

	return 0;
} /* noop_read_processed() */

static int noop_stop(struct i2c_target_config *config)
{
	ARG_UNUSED(config);

	return 0;
} /* noop_stop() */

const struct i2c_target_callbacks i2c_fault_noop_callbacks = {
	.write_requested = noop_write_requested,
	.read_requested = noop_read_requested,
	.write_received = noop_write_received,
	.read_processed = noop_read_processed,
	.stop = noop_stop,
};

void i2c_fault_record(const char *condition, int ret)
{
	TC_PRINT("  %s: driver returned %d\n", condition, ret);
} /* i2c_fault_record() */

void i2c_fault_assert_error(int ret, const char *condition)
{
	i2c_fault_record(condition, ret);

	if (IS_ENABLED(CONFIG_I2C_FAULT_TEST_RELAXED_ERRNO)) {
		zassert_true(ret < 0,
			     "%s: expected a negative errno, got %d",
			     condition, ret);
	} else {
		/*
		 * i2c.h documents -EIO for a failed transfer, and the I2C
		 * documentation's "Transfer Timeout" section documents
		 * -ETIMEDOUT for one that ran out of time.
		 */
		zassert_true((ret == -EIO) || (ret == -ETIMEDOUT),
			     "%s: the I2C API documents -EIO for a failed "
			     "transfer and -ETIMEDOUT for one that timed out, "
			     "got %d",
			     condition, ret);
	}
} /* i2c_fault_assert_error() */

void i2c_fault_assert_healthy(const char *condition)
{
	struct i2c_fault_outcome outcome;
	uint16_t addr = ctx.have_present ? ctx.present_addr : ctx.absent_addr;

	/*
	 * This check runs after a fault, so it is exactly where a driver left
	 * holding its bus lock would stall the run. It goes through the
	 * bounded harness for that reason.
	 */
	(void)probe_bounded(addr, &outcome);
	zassert_false(outcome.timed_out,
		      "%s: a probe of 0x%02x did not return within %d ms, so "
		      "the fault left the driver wedged",
		      condition, addr, CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);

	if (ctx.have_present) {
		zassert_ok(outcome.ret,
			   "%s: the bus was left unusable, address 0x%02x "
			   "stopped answering (%d)",
			   condition, addr, outcome.ret);
		return;
	}

	/*
	 * With nothing on the bus to answer, the weaker check is that a
	 * transfer to the absent address still reports the same failure it
	 * did before the fault, rather than a new one or a hang.
	 */
	zassert_true(outcome.ret < 0,
		     "%s: absent address 0x%02x began answering after the "
		     "fault (%d)",
		     condition, addr, outcome.ret);
} /* i2c_fault_assert_healthy() */

bool i2c_fault_bus_healthy(void)
{
	struct i2c_fault_outcome outcome;
	uint16_t addr = ctx.have_present ? ctx.present_addr : ctx.absent_addr;

	(void)probe_bounded(addr, &outcome);

	if (outcome.timed_out) {
		return false;
	}

	if (ctx.have_present) {
		return outcome.ret == 0;
	}

	return outcome.ret < 0;
} /* i2c_fault_bus_healthy() */

void i2c_fault_scope_pause(const char *what)
{
#ifdef CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE
	/*
	 * No thread drives the bus while this waits, so the first edge after
	 * the keypress is the event under investigation and an ordinary
	 * single-shot edge trigger will catch it.
	 */
	TC_PRINT("  paused before %s: arm the scope, then press any key\n",
		 what);
	(void)console_getchar();
	TC_PRINT("  resuming\n");
#else
	ARG_UNUSED(what);
#endif /* CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE */
} /* i2c_fault_scope_pause() */

void i2c_fault_require_target(void)
{
	if (!ctx.target_ready) {
		TC_PRINT("  no usable fault target; the i2c_bus_short fixture "
			 "and a zephyr,i2c-target-fault node are required\n");
		ztest_test_skip();
	}
} /* i2c_fault_require_target() */

void i2c_fault_require_target_reachable(void)
{
	if (!ctx.target_registered) {
		TC_PRINT("  no registered fault target; the i2c_bus_short "
			 "fixture and a zephyr,i2c-target-fault node are "
			 "required\n");
		ztest_test_skip();
		return;
	}

	if (!ctx.target_reachable) {
		TC_PRINT("  the fault target at 0x%02x is registered but does "
			 "not answer the controller under test; the "
			 "i2c_bus_short wiring is required for this "
			 "condition\n",
			 ctx.target_addr);
		ztest_test_skip();
	}
} /* i2c_fault_require_target_reachable() */

void i2c_fault_require_bus_force(void)
{
	if (!ctx.bus_force_ready) {
		TC_PRINT("  no line-forcing GPIOs; the i2c_bus_fault fixture "
			 "and sda-gpios/scl-gpios under zephyr,user are "
			 "required\n");
		ztest_test_skip();
	}
} /* i2c_fault_require_bus_force() */

void i2c_fault_require_present(void)
{
	if (!ctx.have_present) {
		TC_PRINT("  no device answers on the bus, so there is no "
			 "known-good transfer to compare against\n");
		ztest_test_skip();
	}
} /* i2c_fault_require_present() */

#if I2C_FAULT_HAS_TARGET

static const struct device *const target_dev =
	DEVICE_DT_GET(I2C_FAULT_TARGET_NODE);
static const struct device *const target_bus =
	DEVICE_DT_GET(DT_BUS(I2C_FAULT_TARGET_NODE));

/* Set while the target is unregistered by mechanism M7. */
static bool target_withdrawn;

/* Single-byte write to the fault target, the shape the self-test uses. */
static uint8_t poke_byte;

static int target_poke(void)
{
	struct i2c_fault_outcome outcome;
	struct i2c_fault_xfer xfer;
	struct i2c_msg msg;

	msg.buf = &poke_byte;
	msg.len = 1U;
	msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	xfer.bus = ctx.dut;
	xfer.addr = ctx.target_addr;
	xfer.msgs = &msg;
	xfer.num_msgs = 1U;

	pin_transfer(&xfer);

	if (i2c_timing_run_bounded(i2c_fault_xfer_job, &pinned_xfer,
				   &outcome) != 0) {
		return -ETIMEDOUT;
	}

	return outcome.ret;
} /* target_poke() */

/*
 * Confirm that arming a fault changes what the controller under test sees.
 *
 * A fault injector that silently does nothing would let every dependent test
 * report a pass while proving nothing, so the suite refuses to run those
 * tests unless this sequence holds: quiet, then faulted, then quiet again.
 *
 * The check uses a one-byte write rather than the zero-length probe because
 * the target callbacks act on data bytes; see the note on the address-phase
 * fault kinds in <zephyr/drivers/i2c/target/fault.h>.
 */
static bool injector_self_test(void)
{
	struct i2c_target_fault_config fault = {
		.kind = I2C_TARGET_FAULT_NACK_WRITE_DATA,
		.trigger_index = 0U,
		.repeat_count = I2C_TARGET_FAULT_REPEAT_FOREVER,
		.refuse_errno = -EIO,
	};
	size_t injections;
	int quiet_ret;
	int armed_ret;
	int ret;

	i2c_target_fault_disarm(target_dev);
	i2c_target_fault_reset(target_dev);

	quiet_ret = target_poke();
	ctx.target_reachable = (quiet_ret == 0);
	if (quiet_ret != 0) {
		TC_PRINT("  fault target at 0x%02x did not answer (%d); check "
			 "the i2c_bus_short wiring\n",
			 ctx.target_addr, quiet_ret);
		return false;
	}

	ret = i2c_target_fault_arm(target_dev, &fault);
	if (ret != 0) {
		TC_PRINT("  could not arm the fault target (%d)\n", ret);
		return false;
	}

	i2c_target_fault_reset(target_dev);
	armed_ret = target_poke();
	injections = i2c_target_fault_injections(target_dev);
	i2c_target_fault_disarm(target_dev);

	if (injections == 0U) {
		TC_PRINT("  the target callbacks never ran; the controller "
			 "under test is not reaching address 0x%02x\n",
			 ctx.target_addr);
		return false;
	}

	if (armed_ret == 0) {
		TC_PRINT("  the controller acknowledged a byte the target "
			 "refused; this driver pair cannot inject data-phase "
			 "faults\n");
		return false;
	}

	quiet_ret = target_poke();
	if (quiet_ret != 0) {
		TC_PRINT("  the target stopped answering after disarming "
			 "(%d)\n", quiet_ret);
		return false;
	}

	return true;
} /* injector_self_test() */

static void target_setup(void)
{
	int ret;

	ctx.target = target_dev;
	ctx.target_bus = target_bus;
	ctx.target_addr = (uint16_t)DT_REG_ADDR(I2C_FAULT_TARGET_NODE);
	ctx.target_is_second_controller = (target_bus != ctx.dut);

	if (!device_is_ready(target_dev)) {
		TC_PRINT("  fault target device is not ready\n");
		return;
	}

	ret = i2c_target_driver_register(target_dev);
	if (ret == -ENOSYS) {
		TC_PRINT("  controller %s implements no target mode\n",
			 target_bus->name);
		return;
	}

	if (ret != 0) {
		TC_PRINT("  fault target registration failed (%d)\n", ret);
		return;
	}

	ctx.target_registered = true;
	ctx.target_ready = injector_self_test();

	if (!ctx.target_ready) {
		TC_PRINT("  the target is registered but cannot refuse a "
			 "byte, so the data-phase conditions will skip; the "
			 "target-withdrawal conditions still run\n");
	}
} /* target_setup() */

/* Return the target to answering normally, with an empty trace and no hook. */
static void target_quiesce(void)
{
	if (ctx.target == NULL) {
		return;
	}

	if (target_withdrawn) {
		(void)i2c_target_driver_register(target_dev);
		target_withdrawn = false;
	}

	i2c_target_fault_set_hook(target_dev, NULL, NULL);
	i2c_target_fault_disarm(target_dev);
	i2c_target_fault_reset(target_dev);
} /* target_quiesce() */

int i2c_fault_target_withdraw(void)
{
	int ret;

	if (ctx.target == NULL) {
		return -ENOSYS;
	}

	if (target_withdrawn) {
		return 0;
	}

	ret = i2c_target_driver_unregister(target_dev);
	if (ret == 0) {
		target_withdrawn = true;
	}

	return ret;
} /* i2c_fault_target_withdraw() */

int i2c_fault_target_restore(void)
{
	int ret;

	if (ctx.target == NULL) {
		return -ENOSYS;
	}

	if (!target_withdrawn) {
		return 0;
	}

	ret = i2c_target_driver_register(target_dev);
	if (ret == 0) {
		target_withdrawn = false;
	}

	return ret;
} /* i2c_fault_target_restore() */

#else /* I2C_FAULT_HAS_TARGET */

static void target_setup(void)
{
	TC_PRINT("  no zephyr,i2c-target-fault node; the target-driven "
		 "conditions will skip\n");
} /* target_setup() */

static void target_quiesce(void)
{
} /* target_quiesce() */

int i2c_fault_target_withdraw(void)
{
	return -ENOSYS;
} /* i2c_fault_target_withdraw() */

int i2c_fault_target_restore(void)
{
	return -ENOSYS;
} /* i2c_fault_target_restore() */

#endif /* I2C_FAULT_HAS_TARGET */

/*
 * Settle on the two addresses the suite works with.
 *
 * Both are discovered rather than derived, because a derived one is correct
 * only by accident on a given board. Each is verified against the bus before
 * it is trusted, and the absent one is verified again before every test, so
 * that a device coming out of reset partway through the run is reported as
 * such instead of appearing to be a driver defect.
 *
 * Nothing here may be settled on a fact about software. That a fault target
 * registered says only that some controller accepted a target configuration;
 * whether the controller under test can reach it is a separate question, and
 * only the answer to that makes an address usable here.
 */
static void discover_addresses(void)
{
	bool absent_found = false;
	int ret;

	ctx.absent_addr = (uint16_t)CONFIG_I2C_FAULT_TEST_ABSENT_ADDR;
	absent_found = (ctx.absent_addr != 0U);

	ctx.present_addr = (uint16_t)CONFIG_I2C_FAULT_TEST_PRESENT_ADDR;
	ctx.have_present = (ctx.present_addr != 0U);

	if (!ctx.have_present && ctx.target_registered) {
		/*
		 * The fault target is the natural known-good address where the
		 * controller under test can reach it, which on most rigs means
		 * the two controllers are wired together. Where they are not,
		 * it answers nothing and the scan below must find a real
		 * device instead.
		 */
		ret = i2c_fault_probe(ctx.dut, ctx.target_addr);
		note_bus_result(ret);

		if (ret == 0) {
			ctx.present_addr = ctx.target_addr;
			ctx.have_present = true;
		} else {
			TC_PRINT("  the fault target at 0x%02x is registered "
				 "but does not answer the controller under "
				 "test (%d), so it cannot be the known-good "
				 "address\n",
				 ctx.target_addr, ret);
		}
	}

	for (uint16_t addr = ADDR_SCAN_FIRST;
	     (addr <= ADDR_SCAN_LAST) && (!absent_found || !ctx.have_present);
	     addr++) {
		/* The fault target belongs to neither role. */
		if ((ctx.target != NULL) && (addr == ctx.target_addr)) {
			continue;
		}

		ret = i2c_fault_probe(ctx.dut, addr);
		note_bus_result(ret);

		if (ctx.bus_unusable) {
			TC_PRINT("  address 0x%02x is the %d th consecutive "
				 "probe to time out rather than answer\n",
				 addr, CONFIG_I2C_FAULT_TEST_MAX_BUS_TIMEOUTS);
			TC_PRINT("  a zero-length write is the canonical bus "
				 "scan; an absent address should refuse it "
				 "immediately, not time out\n");
			TC_PRINT("  this is either a bus fault, or a driver "
				 "that does not complete zero-length writes; "
				 "check the lines with a meter to tell them "
				 "apart\n");
			break;
		}

		if (ret == 0) {
			if (!ctx.have_present) {
				ctx.present_addr = addr;
				ctx.have_present = true;
			}
		} else if (!absent_found && (ret != -ETIMEDOUT)) {
			/*
			 * Only a refusal makes an address absent. A timeout
			 * says nothing about the address, so it must not be
			 * accepted as one or every later condition would be
			 * aimed at an address that was never classified.
			 */
			ctx.absent_addr = addr;
			absent_found = true;
		} else {
			/* Neither role is still open for this address. */
		}
	}

	if (ctx.bus_unusable) {
		return;
	}

	zassert_true(absent_found,
		     "every address from 0x%02x to 0x%02x answered; the suite "
		     "needs one that does not",
		     ADDR_SCAN_FIRST, ADDR_SCAN_LAST);

	ret = i2c_fault_probe(ctx.dut, ctx.absent_addr);
	zassert_true(ret < 0,
		     "address 0x%02x answers and cannot serve as the absent "
		     "address; set CONFIG_I2C_FAULT_TEST_ABSENT_ADDR to one "
		     "that does not",
		     ctx.absent_addr);

	/*
	 * Confirm the known-good address a second time, for the same reason
	 * the absent one is confirmed: an address is only usable here if the
	 * bus agrees about it twice.
	 *
	 * A disagreement is not fatal and must not abort the run. Every
	 * condition that needs this address skips without it, and a
	 * controller that answers an unchanged probe differently is exactly
	 * what condition A15 exists to diagnose. Failing setup here would
	 * stop A15 ever running and replace its clear report with a confusing
	 * one.
	 */
	if (ctx.have_present) {
		ret = i2c_fault_probe(ctx.dut, ctx.present_addr);
		if (ret != 0) {
			TC_PRINT("  address 0x%02x answered once and then "
				 "refused (%d), so it cannot be the known-good "
				 "address; the conditions needing one will "
				 "skip\n",
				 ctx.present_addr, ret);
			ctx.have_present = false;
			ctx.present_addr = 0U;
		}
	}
} /* discover_addresses() */

/* Name the tier the run is executing at, so a skipped test is explicable. */
static void report_environment(void)
{
	TC_PRINT("I2C fault injection environment\n");
	TC_PRINT("  controller under test: %s\n", ctx.dut->name);
	TC_PRINT("  absent address:        0x%02x\n", ctx.absent_addr);

	if (ctx.have_present) {
		TC_PRINT("  known-good address:    0x%02x\n", ctx.present_addr);
	} else {
		TC_PRINT("  known-good address:    none found\n");
	}

	TC_PRINT("  line forcing:          %s\n",
		 ctx.bus_force_ready ? "available" : "not wired");

	/*
	 * Name the specific reason a target is unusable. "Cannot refuse a
	 * byte" and "does not answer" call for different remedies, the first
	 * a driver fix and the second a jumper, so reporting one for the
	 * other sends a reader after the wrong thing.
	 */
	if (ctx.target_ready) {
		TC_PRINT("  fault target:          ready\n");
	} else if (!ctx.target_registered) {
		TC_PRINT("  fault target:          unavailable\n");
	} else if (!ctx.target_reachable) {
		TC_PRINT("  fault target:          registered, does not "
			 "answer the controller under test\n");
	} else {
		TC_PRINT("  fault target:          registered, cannot refuse "
			 "a byte\n");
	}

	if (ctx.target_ready && !ctx.target_is_second_controller) {
		TC_PRINT("  the fault target shares the controller under "
			 "test, so the contention conditions will skip\n");
	}
} /* report_environment() */

void *i2c_fault_setup(void)
{
	int ret;

	/*
	 * Every file in the suite declares its own ztest suite and so calls
	 * this hook, but the discovery scan and the injector self-test drive
	 * the bus and need to happen once.
	 */
	if (ctx_valid) {
		return NULL;
	}

	ctx.dut = DEVICE_DT_GET(I2C_FAULT_DUT_NODE);
	zassert_true(device_is_ready(ctx.dut),
		     "controller under test is not ready");

#ifdef CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE
	(void)console_init();
#endif /* CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE */

	i2c_timing_harness_start();

	ret = bus_force_init();
	zassert_ok(ret, "could not configure the line-forcing GPIOs (%d)", ret);
	ctx.bus_force_ready = bus_force_available();

	/*
	 * An idle I2C bus is pulled high on both lines. Reading either as low
	 * before anything has run means the bus is already stuck or the
	 * forcing pins are jumpered somewhere they should not be, and every
	 * result that followed would be misleading.
	 */
	if (ctx.bus_force_ready) {
		ret = bus_force_level(BUS_FORCE_SCL);
		zassert_equal(ret, 1,
			      "SCL reads low on an idle bus (%d); check the "
			      "i2c_bus_fault jumpers and the pull-ups", ret);

		ret = bus_force_level(BUS_FORCE_SDA);
		zassert_equal(ret, 1,
			      "SDA reads low on an idle bus (%d); check the "
			      "i2c_bus_fault jumpers and the pull-ups", ret);
	}

	target_setup();
	discover_addresses();
	report_environment();

	ctx_valid = true;

	return NULL;
} /* i2c_fault_setup() */

void i2c_fault_before(void *fixture)
{
	struct i2c_fault_outcome outcome;

	ARG_UNUSED(fixture);

	ctx.hang_detected = i2c_timing_harness_stuck();
	if (ctx.hang_detected) {
		TC_PRINT("  an earlier transfer never returned, so the bus "
			 "state is unknown\n");
		ztest_test_skip();
		return;
	}

	if (ctx.bus_unusable) {
		TC_PRINT("  the bus did not complete %d consecutive transfers "
			 "during setup, so no result here could be trusted\n",
			 CONFIG_I2C_FAULT_TEST_MAX_BUS_TIMEOUTS);
		ztest_test_skip();
		return;
	}

	if (ctx.bus_force_ready) {
		(void)bus_force_release();
	}

	target_quiesce();

	(void)probe_bounded(ctx.absent_addr, &outcome);
	zassert_false(outcome.timed_out,
		      "a probe of the absent address 0x%02x did not return "
		      "within %d ms before the test even started",
		      ctx.absent_addr, CONFIG_I2C_FAULT_TEST_TIMEOUT_MS);
	zassert_true(outcome.ret < 0,
		     "address 0x%02x started answering partway through the "
		     "run; a device came out of reset and the absent-address "
		     "conditions are no longer valid",
		     ctx.absent_addr);
} /* i2c_fault_before() */

void i2c_fault_after(void *fixture)
{
	ARG_UNUSED(fixture);

	if (ctx.bus_force_ready) {
		(void)bus_force_release();
	}

	target_quiesce();
} /* i2c_fault_after() */

ZTEST_SUITE(i2c_fault_harness, NULL, i2c_fault_setup, i2c_fault_before,
	    i2c_fault_after, NULL);

/**
 * @brief The environment the rest of the suite depends on.
 *
 * @details
 * Stimulus:
 * - Check the controller under test is ready, and probe the absent address.
 *
 * On the bus:
 * - One zero-length write to the address discovery settled on, which must go
 *   unanswered.
 *
 * Expected:
 * - The controller is ready and the absent address is silent.
 *
 * Every address-phase condition in the suite is built on the assumption that
 * this address answers nothing. Asserting it as a test of its own means a
 * board where the assumption is false reports one clear failure rather than a
 * scattering of confusing ones.
 *
 * Requires: one I2C controller. No fixture.
 */
ZTEST(i2c_fault_harness, test_environment)
{
	zassert_true(device_is_ready(ctx.dut),
		     "controller under test is not ready");
	zassert_true(i2c_fault_probe(ctx.dut, ctx.absent_addr) < 0,
		     "absent address 0x%02x answers", ctx.absent_addr);
} /* test_environment */

/**
 * @brief Arming a fault must visibly change what the controller sees.
 *
 * @details
 * Stimulus:
 * - Write one byte to the fault target with nothing armed.
 * - Arm a refusal of byte zero, reset the counters, and repeat the write.
 * - Disarm and write once more.
 *
 * On the bus:
 * - An acknowledged single-byte write, then the same write with the data byte
 *   NACKed, then an acknowledged one again.
 *
 * Expected:
 * - Quiet, then faulted, then quiet.
 * - The target's callbacks ran, so the transfer reached it at all.
 * - The armed write failed. A controller that acknowledged a byte the target
 *   refused cannot inject data-phase faults, and the suite skips every
 *   condition that depends on it rather than reporting passes.
 *
 * A fault injector that silently does nothing would let every dependent test
 * report a pass while proving nothing, which is the worst outcome available
 * to a suite like this. It is checked at setup and again here so that an
 * injector that stops working partway through the run is caught.
 *
 * Requires: a registered fault target. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_harness, test_injector_self_test)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	zassert_true(injector_self_test(),
		     "the fault injector stopped working partway through the "
		     "run");
#endif
} /* test_injector_self_test */

/**
 * @brief The event trace must not drop entries during a normal transfer.
 *
 * @details
 * Stimulus:
 * - Reset the trace, then write one byte more than the payload length to the
 *   fault target, and read the event count back.
 *
 * On the bus:
 * - One ordinary addressed write, acknowledged throughout.
 *
 * Expected:
 * - The target recorded at least as many callbacks as bytes sent.
 * - The trace did not overflow.
 *
 * Several conditions decide what to assert by reading the trace back, B5 most
 * directly, and a trace that silently truncated would make those decisions on
 * partial information. Checking the depth against the largest transfer the
 * suite performs turns a subtle wrong answer into an explicit failure naming
 * CONFIG_I2C_TARGET_FAULT_TRACE_DEPTH.
 *
 * Requires: a registered fault target. Fixture: i2c_bus_short.
 */
ZTEST(i2c_fault_harness, test_trace_capacity)
{
	i2c_fault_require_target();

#if I2C_FAULT_HAS_TARGET
	uint8_t payload[I2C_FAULT_PAYLOAD_LEN + 1U];
	size_t events;
	int ret;

	i2c_target_fault_reset(ctx.target);
	i2c_fault_fill(payload, sizeof(payload), 0x10U);

	ret = i2c_write(ctx.dut, payload, sizeof(payload), ctx.target_addr);
	zassert_ok(ret, "known-good write to the fault target failed (%d)",
		   ret);

	events = i2c_target_fault_event_count(ctx.target);
	zassert_true(events >= sizeof(payload),
		     "the target recorded %zu callbacks for a %zu byte write",
		     events, sizeof(payload));
	zassert_false(i2c_target_fault_overflowed(ctx.target),
		      "the trace overflowed on a %zu byte write; raise "
		      "CONFIG_I2C_TARGET_FAULT_TRACE_DEPTH",
		      sizeof(payload));
#endif
} /* test_trace_capacity */
