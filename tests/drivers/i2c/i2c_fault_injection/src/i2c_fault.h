/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared state and helpers for the I2C fault injection suite.
 *
 * The suite is split across one file per fault class, and every one of them
 * needs the same three things: the controller under test, an address that
 * nothing on the bus answers, and a way to assert that a faulted transfer
 * failed rather than hung. They are established once in main.c and reached
 * from here.
 */

#ifndef I2C_FAULT_H_
#define I2C_FAULT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>

/* Defines struct i2c_fault_outcome, which the bounded helpers below return. */
#include "i2c_timing.h"

/* Controller under test. Every board supporting the suite provides this. */
#define I2C_FAULT_DUT_NODE DT_ALIAS(i2c_dut)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(I2C_FAULT_DUT_NODE),
	     "the i2c-dut alias must name an enabled I2C controller");

/* Fault target, present only on boards wired for the i2c_bus_short fixture. */
#define I2C_FAULT_HAS_TARGET DT_HAS_COMPAT_STATUS_OKAY(zephyr_i2c_target_fault)

#if I2C_FAULT_HAS_TARGET
#define I2C_FAULT_TARGET_NODE                                                  \
	DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_i2c_target_fault)
#endif

/*
 * Second fault target, used only by the condition that puts two targets on
 * one address. Boards providing it declare the "i2c-fault-target-2" alias.
 */
#define I2C_FAULT_HAS_TARGET2                                                  \
	DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_fault_target_2))

/*
 * Device the second controller addresses in the contention condition, named
 * by the "i2c-fault-contender" alias. It must sit on the bus under test and
 * answer its address. Arbitration is decided in the address phase and the
 * lower address wins, so its address must also be below the fault target's
 * for the controller under test to be the one that loses.
 */
#define I2C_FAULT_HAS_CONTENDER                                                \
	DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_fault_contender))

#if I2C_FAULT_HAS_CONTENDER
#define I2C_FAULT_CONTENDER_NODE DT_ALIAS(i2c_fault_contender)
#define I2C_FAULT_CONTENDER_ADDR                                               \
	((uint16_t)DT_REG_ADDR(I2C_FAULT_CONTENDER_NODE))

BUILD_ASSERT(DT_SAME_NODE(DT_BUS(I2C_FAULT_CONTENDER_NODE),
			  I2C_FAULT_DUT_NODE),
	     "the i2c-fault-contender alias must name a device on the "
	     "i2c-dut bus");
#endif

/* Payload length the data-phase conditions transfer. */
#define I2C_FAULT_PAYLOAD_LEN CONFIG_I2C_FAULT_TEST_PAYLOAD_LEN

/** Capabilities discovered at setup, and the addresses the suite settled on. */
struct i2c_fault_ctx {
	/** Controller under test. */
	const struct device *dut;
	/** Address on the device under test bus that nothing answers. */
	uint16_t absent_addr;
	/** Address that answers, used for the known-good transfers. */
	uint16_t present_addr;
	/** Set when a present address was found or configured. */
	bool have_present;
	/** Fault target device, or NULL when the board has none. */
	const struct device *target;
	/** Controller the fault target is registered on. */
	const struct device *target_bus;
	/** Address the fault target answers. */
	uint16_t target_addr;
	/**
	 * Set once the target is registered on its controller. This does not
	 * imply the target can refuse anything; see target_ready. Nor does it
	 * imply the controller under test can reach it; see target_reachable.
	 */
	bool target_registered;
	/**
	 * Set once the target has answered the controller under test.
	 *
	 * Registration is a fact about software: some controller accepted a
	 * target configuration. This is the fact about the bus, and it is the
	 * one every condition that addresses the target depends on. On a rig
	 * where the two controllers are not wired together, a target can be
	 * registered and answer nothing.
	 */
	bool target_reachable;
	/**
	 * Set once the target is registered and the injector self-test has
	 * shown that arming a fault changes what the controller sees.
	 */
	bool target_ready;
	/** Set when the target sits on a controller other than the DUT. */
	bool target_is_second_controller;
	/** Set when zephyr,user supplies the SDA and SCL forcing GPIOs. */
	bool bus_force_ready;
	/**
	 * Set once the bus has failed to complete enough consecutive
	 * transfers that it is judged broken rather than merely faulted.
	 * A NACK is an answer; a timeout means nothing is completing, which
	 * is a wiring or bus fault and makes every later result meaningless.
	 */
	bool bus_unusable;
	/** Set once a transfer has failed to return within the time bound. */
	bool hang_detected;
};

const struct i2c_fault_ctx *i2c_fault_ctx(void);

void *i2c_fault_setup(void);
void i2c_fault_before(void *fixture);
void i2c_fault_after(void *fixture);

/** Probe an address with the canonical zero-length write. */
int i2c_fault_probe(const struct device *bus, uint16_t addr);

/** Skip the running test unless the fault target is usable. */
void i2c_fault_require_target(void);

/**
 * Skip the running test unless the fault target answers the controller under
 * test.
 *
 * Weaker than i2c_fault_require_target(), which additionally requires that
 * the target can refuse a byte. The M7 and contention conditions do not need
 * a refusal, but they do address the target on the bus, so registration on
 * its own is not enough: on a rig where the two controllers are not wired
 * together the target is registered and answers nothing.
 */
void i2c_fault_require_target_reachable(void);

/** Skip the running test unless the line-forcing GPIOs are wired. */
void i2c_fault_require_bus_force(void);

/** Skip the running test unless a known-good address is available. */
void i2c_fault_require_present(void);

/**
 * Withdraw the fault target so its address stops answering.
 *
 * Mechanism M7. Unregistering makes a device that was present a moment ago
 * NACK its address, which is a fault against a present device and needs
 * nothing of the controller beyond the target registration API itself. It is
 * therefore available on controllers whose byte callbacks cannot refuse
 * anything.
 *
 * @retval 0 on success.
 * @retval -ENOSYS if the board has no fault target.
 * @retval negative errno from i2c_target_unregister() otherwise.
 */
int i2c_fault_target_withdraw(void);

/**
 * Restore a target withdrawn by i2c_fault_target_withdraw().
 *
 * @retval 0 on success.
 * @retval -ENOSYS if the board has no fault target.
 * @retval negative errno from i2c_target_register() otherwise.
 */
int i2c_fault_target_restore(void);

/**
 * Assert that @p ret reports a failed transfer.
 *
 * The I2C API documents -EIO for a failed transfer and the existing suites
 * assert that exact value, so it is what this requires by default.
 * CONFIG_I2C_FAULT_TEST_RELAXED_ERRNO weakens it to any negative value for
 * bringing up a driver that is not yet conformant. Either way the value is
 * recorded, because collecting what drivers actually return is one of the
 * outputs of this suite.
 */
void i2c_fault_assert_error(int ret, const char *condition);

/** Record the value a driver returned without asserting anything about it. */
void i2c_fault_record(const char *condition, int ret);

/** Assert that a known-good transfer still succeeds. */
void i2c_fault_assert_healthy(const char *condition);

/**
 * Report whether the bus is usable, without asserting anything.
 *
 * Same check as i2c_fault_assert_healthy() and through the same bounded
 * harness, but for a condition whose measurement is only meaningful on a
 * working bus and which should skip rather than fail when an earlier
 * condition has already left it unusable. Reporting one defect twice tells a
 * reader nothing and obscures which condition found it.
 *
 * @return true if a probe of the known-good address still answers, or, where
 *         the board has no known-good address, if the absent address still
 *         refuses.
 */
bool i2c_fault_bus_healthy(void);

/**
 * Wait for a console keypress so an instrument can be armed.
 *
 * Compiles to nothing unless CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE is set. While
 * the suite waits, no thread drives the bus, so the first edge after the
 * keypress is the event under investigation. That is what makes a plain
 * single-shot edge trigger usable: without the silence, the same trigger
 * fires on any of the hundreds of legitimate transfers performed first.
 *
 * Call it as the first statement of a condition, before any bus activity
 * that condition performs.
 *
 * No call sites are kept in the tree. A pause belongs to one investigation,
 * so it is added where it is wanted and removed once that question is
 * answered, rather than left behind for a later reader to puzzle over. See
 * README.rst.
 *
 * @param what identifier of the condition, named in the console prompt.
 */
void i2c_fault_scope_pause(const char *what);

/** Fill @p buf with an incrementing pattern. */
void i2c_fault_fill(uint8_t *buf, size_t len, uint8_t seed);

/**
 * Callback table for a target registered only to check that registration
 * works.
 *
 * Some controllers call into the table unconditionally when an event
 * arrives, so a table of NULL pointers would fault a conforming driver. These
 * accept everything and refuse nothing.
 */
extern const struct i2c_target_callbacks i2c_fault_noop_callbacks;

/** Largest message count the bounded harness can pin for a transfer. */
#define I2C_FAULT_MAX_MSGS 4

/** A transfer described well enough to be handed to the bounded harness. */
struct i2c_fault_xfer {
	const struct device *bus;
	uint16_t addr;
	struct i2c_msg *msgs;
	uint8_t num_msgs;
};

/**
 * Job entry point that performs an @ref i2c_fault_xfer.
 *
 * Faulted transfers run on the bounded harness worker rather than in the
 * test thread, so that a driver which never returns is reported as a hang
 * instead of stalling the whole run.
 */
int i2c_fault_xfer_job(void *arg);

/**
 * Run a transfer on the bounded harness and fail the test if it hangs.
 *
 * The message array is copied into storage the harness owns, because a job
 * that outruns the bound is left running and the controller may still be
 * walking the descriptors after the test thread has unwound its stack. The
 * data buffers the messages point at are not copied and must therefore
 * outlive the run; the suite uses file-scope buffers for that reason.
 *
 * @param[in]  xfer      transfer to perform, at most @ref I2C_FAULT_MAX_MSGS
 *                       messages.
 * @param[in]  condition condition identifier used in the failure message.
 * @param[out] out       result and elapsed time.
 */
void i2c_fault_transfer_bounded(const struct i2c_fault_xfer *xfer,
				const char *condition,
				struct i2c_fault_outcome *out);

/**
 * Hand a transfer to the bounded harness without waiting for it.
 *
 * Same storage rules as i2c_fault_transfer_bounded(). Must be paired with
 * i2c_timing_collect().
 *
 * @param xfer transfer to perform.
 *
 * @retval 0 on success.
 * @retval -ETIMEDOUT if an earlier job never returned.
 */
int i2c_fault_transfer_submit(const struct i2c_fault_xfer *xfer);

/**
 * Run a write-read on the bounded harness and fail the test if it hangs.
 *
 * @param[in]  addr      target address.
 * @param[in]  wr        bytes to write, must outlive the run.
 * @param[in]  wr_len    number of bytes to write.
 * @param[out] rd        storage for the bytes read, must outlive the run.
 * @param[in]  rd_len    number of bytes to read.
 * @param[in]  condition condition identifier used in the failure message.
 * @param[out] out       result and elapsed time.
 */
void i2c_fault_write_read_bounded(uint16_t addr, uint8_t *wr, size_t wr_len,
				  uint8_t *rd, size_t rd_len,
				  const char *condition,
				  struct i2c_fault_outcome *out);

#endif /* I2C_FAULT_H_ */
