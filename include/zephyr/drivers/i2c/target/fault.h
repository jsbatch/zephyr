/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the programmable I2C fault-injection target.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_I2C_TARGET_FAULT_H_
#define ZEPHYR_INCLUDE_DRIVERS_I2C_TARGET_FAULT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Programmable I2C fault-injection target
 * @defgroup i2c_target_fault I2C fault-injection target
 * @ingroup i2c_interface
 * @{
 *
 * The target answers a 7-bit address on one I2C controller while a second
 * controller, wired to the same SDA and SCL, acts as the device under test.
 * Faults are produced from inside the target callbacks, so what reaches the
 * controller under test is a bus event rather than a simulation of one.
 *
 * Two properties of the I2C target API make this possible. A callback that
 * returns a negative value makes the target peripheral NACK the byte, and a
 * callback that blocks holds SCL low for its duration because the peripheral
 * cannot release the clock until the callback returns.
 *
 * The target is intended for driver test suites and is not a model of any
 * real device.
 */

/** Apply the fault to every eligible byte rather than to one index. */
#define I2C_TARGET_FAULT_TRIGGER_EVERY 0xFFFFU

/** Apply the fault to every transaction until the target is disarmed. */
#define I2C_TARGET_FAULT_REPEAT_FOREVER 0U

/**
 * Fault the target applies to the transactions that follow.
 *
 * The address-phase kinds refuse the transaction from the callback the
 * controller runs on address match. Note that the target peripheral has
 * normally already acknowledged the address in hardware by the time that
 * callback runs, so what appears on the wire is a NACK of the first data
 * byte rather than of the address itself. The I2C API states the contract
 * as "an error return shall cause the controller to NACK the next byte
 * received" (include/zephyr/drivers/i2c.h). Use I2C_TARGET_FAULT_NONE and an
 * address nothing answers to fault the address phase itself.
 *
 * There is deliberately no kind that stretches the clock. A stretch is
 * produced by a callback declining to return, and these callbacks run in
 * interrupt context on most controllers, where waiting is not permissible.
 * A test that wants a stretch positioned at a chosen byte installs a hook
 * with i2c_target_fault_set_hook() and pulls SCL low from it, which is a
 * single pin write, and releases the line from a timer.
 */
enum i2c_target_fault_kind {
	/** Answer normally. */
	I2C_TARGET_FAULT_NONE = 0,
	/** Refuse a write from the callback run on address match. */
	I2C_TARGET_FAULT_NACK_WRITE_ADDR,
	/** Refuse a read from the callback run on address match. */
	I2C_TARGET_FAULT_NACK_READ_ADDR,
	/** Refuse the written data byte selected by @ref trigger_index. */
	I2C_TARGET_FAULT_NACK_WRITE_DATA,
	/** Refuse to supply the read byte selected by @ref trigger_index. */
	I2C_TARGET_FAULT_FAIL_READ_DATA,
};

/** Target callback that produced a trace entry. */
enum i2c_target_fault_event_type {
	/** write_requested(), a START addressed the target for a write. */
	I2C_TARGET_FAULT_EV_WRITE_REQUESTED = 0,
	/** write_received(), the controller delivered a byte. */
	I2C_TARGET_FAULT_EV_WRITE_RECEIVED,
	/** read_requested(), a START addressed the target for a read. */
	I2C_TARGET_FAULT_EV_READ_REQUESTED,
	/** read_processed(), the controller took a byte and wants another. */
	I2C_TARGET_FAULT_EV_READ_PROCESSED,
	/** stop(), the transaction ended. */
	I2C_TARGET_FAULT_EV_STOP,
	/** error(), the controller reported a bus error to the target. */
	I2C_TARGET_FAULT_EV_ERROR,
};

/** One recorded target callback. */
struct i2c_target_fault_event {
	/** Callback that ran, from @ref i2c_target_fault_event_type. */
	uint8_t type;
	/** Byte the callback carried, or the error reason for an error. */
	uint8_t value;
	/** Byte index within the current transaction phase. */
	uint16_t index;
	/** Set when this callback applied the armed fault. */
	bool injected;
	/** k_cycle_get_32() sampled on entry to the callback. */
	uint32_t cycles;
};

/** Fault to apply to the transactions that follow. */
struct i2c_target_fault_config {
	/** Behaviour to apply. */
	enum i2c_target_fault_kind kind;
	/**
	 * Byte index the fault acts on, counted from zero within the data
	 * phase of the current transaction. Address faults ignore it.
	 * @ref I2C_TARGET_FAULT_TRIGGER_EVERY acts on every byte.
	 */
	uint16_t trigger_index;
	/**
	 * Number of transactions to inject into, counted from the arm call.
	 * @ref I2C_TARGET_FAULT_REPEAT_FOREVER injects into every transaction
	 * until the target is disarmed.
	 */
	uint16_t repeat_count;
	/**
	 * Value the callback returns to refuse a byte. Must be negative.
	 * Zero selects -EIO.
	 */
	int refuse_errno;
};

/**
 * @brief Callback invoked from inside a target callback.
 *
 * Runs in whatever context the controller driver services target callbacks
 * in, which is interrupt context on most drivers. It must therefore do only
 * bounded, non-blocking work: setting a pin, giving a semaphore, starting a
 * timer. It must not wait.
 *
 * It exists so that a test can act at a chosen point inside a live
 * transaction, for instance to pull a bus line low while the controller under
 * test is mid-transfer, or to begin a clock stretch that a timer will end.
 *
 * @param dev       fault target the callback belongs to.
 * @param type      callback that is running.
 * @param index     byte index within the current transaction phase.
 * @param user_data value passed to i2c_target_fault_set_hook().
 */
typedef void (*i2c_target_fault_hook_t)(const struct device *dev,
					enum i2c_target_fault_event_type type,
					uint16_t index, void *user_data);

/**
 * @brief Arm a fault.
 *
 * Replaces any fault already armed and rewinds the window that
 * @ref i2c_target_fault_config.repeat_count is measured against. The trace
 * and the injection and transaction counters are left alone, so a test may
 * arm mid-run without losing history; use i2c_target_fault_reset() to clear
 * them.
 *
 * @param dev fault target.
 * @param cfg fault to apply, or NULL to disarm.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p cfg names a fault the target cannot produce.
 */
int i2c_target_fault_arm(const struct device *dev,
			 const struct i2c_target_fault_config *cfg);

/**
 * @brief Revert to answering every transaction normally.
 *
 * @param dev fault target.
 */
void i2c_target_fault_disarm(const struct device *dev);

/**
 * @brief Discard the trace and zero the injection and transaction counters.
 *
 * The armed fault is left in place.
 *
 * @param dev fault target.
 */
void i2c_target_fault_reset(const struct device *dev);

/**
 * @brief Number of callbacks held in the trace.
 *
 * Saturates at CONFIG_I2C_TARGET_FAULT_TRACE_DEPTH; use
 * i2c_target_fault_overflowed() to tell a full trace from a truncated one.
 *
 * @param dev fault target.
 *
 * @return number of entries i2c_target_fault_event() will return.
 */
size_t i2c_target_fault_event_count(const struct device *dev);

/**
 * @brief Fetch a recorded callback.
 *
 * @param[in]  dev fault target.
 * @param[in]  idx entry to fetch, counted from the oldest.
 * @param[out] out storage for the entry.
 *
 * @retval 0 on success.
 * @retval -ENOENT if @p idx is past the end of the trace.
 */
int i2c_target_fault_event(const struct device *dev, size_t idx,
			   struct i2c_target_fault_event *out);

/**
 * @brief Report whether the trace dropped entries.
 *
 * @param dev fault target.
 *
 * @return true if at least one callback was not recorded.
 */
bool i2c_target_fault_overflowed(const struct device *dev);

/**
 * @brief Number of callbacks that applied the armed fault.
 *
 * A test arms a fault, performs a transfer, and checks this is non-zero
 * before drawing any conclusion from the value the transfer returned. A
 * fault that was never injected makes the transfer result meaningless.
 *
 * @param dev fault target.
 *
 * @return injection count since the last i2c_target_fault_reset().
 */
size_t i2c_target_fault_injections(const struct device *dev);

/**
 * @brief Number of transactions the target has been addressed in.
 *
 * @param dev fault target.
 *
 * @return transaction count since the last i2c_target_fault_reset().
 */
size_t i2c_target_fault_transactions(const struct device *dev);

/**
 * @brief Overwrite the backing buffer without using the bus.
 *
 * @param dev  fault target.
 * @param data bytes to store from offset zero.
 * @param len  number of bytes.
 *
 * @retval 0 on success.
 * @retval -ENOSPC if @p len exceeds i2c_target_fault_size().
 */
int i2c_target_fault_set_data(const struct device *dev, const uint8_t *data,
			      size_t len);

/**
 * @brief Read the backing buffer back without using the bus.
 *
 * @param[in]  dev fault target.
 * @param[out] buf storage for the bytes.
 * @param[in]  len number of bytes to copy.
 *
 * @return number of bytes copied, which is the smaller of @p len and
 *         i2c_target_fault_size().
 */
size_t i2c_target_fault_get_data(const struct device *dev, uint8_t *buf,
				 size_t len);

/**
 * @brief Size of the backing buffer in bytes.
 *
 * @param dev fault target.
 *
 * @return size taken from the devicetree @c size property.
 */
size_t i2c_target_fault_size(const struct device *dev);

/**
 * @brief Install a callback that runs inside the target callbacks.
 *
 * @param dev       fault target.
 * @param hook      function to call, or NULL to remove the current one.
 * @param user_data opaque value handed back to @p hook.
 */
void i2c_target_fault_set_hook(const struct device *dev,
			       i2c_target_fault_hook_t hook, void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_I2C_TARGET_FAULT_H_ */
