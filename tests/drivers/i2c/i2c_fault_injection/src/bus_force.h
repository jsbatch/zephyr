/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Holding SDA or SCL low through spare GPIO pins jumpered to the bus.
 *
 * This is a millisecond-scale operation and demands no timing precision.
 * Because the bus is open drain the low always wins, so a pin pulled low
 * anywhere on the net stalls the controller under test no matter what it is
 * driving. Nothing here counts clock edges or meets a bit deadline.
 *
 * The pins named by the zephyr,user node must be spare GPIOs wired to the
 * controller's own SDA and SCL nets, and must be declared GPIO_ACTIVE_HIGH so
 * that a logical zero is a physical low.
 */

#ifndef BUS_FORCE_H_
#define BUS_FORCE_H_

#include <stdbool.h>

/** Lines the helper can hold low. */
enum bus_force_line {
	BUS_FORCE_SDA,
	BUS_FORCE_SCL,
	BUS_FORCE_BOTH,
};

/** True when the board wired both forcing GPIOs. */
bool bus_force_available(void);

/**
 * Configure a line as a released output so a later assertion is ISR-safe.
 *
 * Once armed, bus_force_assert_low() does nothing but write the pin, which
 * makes it usable from a target callback. Reserved for the conditions that
 * must pull a line down from inside a live transaction.
 *
 * @param line line to arm, BUS_FORCE_SDA or BUS_FORCE_SCL.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the board provides no forcing GPIOs.
 * @retval negative errno from the GPIO driver otherwise.
 */
int bus_force_arm(enum bus_force_line line);

/**
 * Pull an armed line low with a single pin write.
 *
 * Safe to call from interrupt context, unlike bus_force_low(), which
 * reconfigures the pin. The line must have been armed by bus_force_arm().
 *
 * @param line line to pull down, BUS_FORCE_SDA or BUS_FORCE_SCL.
 */
void bus_force_assert_low(enum bus_force_line line);

/**
 * Pull an armed line low now and release it after @p span_us.
 *
 * Safe to call from interrupt context: it writes the pin and starts a timer,
 * and does not wait. This is how the suite produces a clock stretch at a
 * chosen byte. Holding SCL by declining to return from a target callback
 * would do the same thing electrically, but those callbacks run in interrupt
 * context on most controllers, where waiting is not permissible.
 *
 * The line must have been armed by bus_force_arm().
 *
 * @param line    line to pull down, BUS_FORCE_SDA or BUS_FORCE_SCL.
 * @param span_us how long to hold it, in microseconds.
 */
void bus_force_pulse_low(enum bus_force_line line, uint32_t span_us);

/**
 * Wait for a pulse started by bus_force_pulse_low() to finish.
 *
 * @param timeout_ms longest time to wait.
 *
 * @retval 0 if the line was released.
 * @retval -ETIMEDOUT if it was not, which leaves the bus held.
 */
int bus_force_pulse_wait(uint32_t timeout_ms);

/**
 * Configure the forcing GPIOs and leave both lines released.
 *
 * @retval 0 on success, or when the board provides no forcing GPIOs.
 * @retval negative errno from the GPIO driver otherwise.
 */
int bus_force_init(void);

/**
 * Hold @p line low until bus_force_release() is called.
 *
 * @param line line or lines to pull down.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the board provides no forcing GPIOs.
 * @retval negative errno from the GPIO driver otherwise.
 */
int bus_force_low(enum bus_force_line line);

/**
 * Release both lines back to high impedance.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the board provides no forcing GPIOs.
 * @retval negative errno from the GPIO driver otherwise.
 */
int bus_force_release(void);

/**
 * Read the level of @p line without disturbing it.
 *
 * @param line line to sample, BUS_FORCE_SDA or BUS_FORCE_SCL.
 *
 * @retval 0 or 1 for the level read.
 * @retval -ENODEV if the board provides no forcing GPIOs.
 * @retval negative errno from the GPIO driver otherwise.
 */
int bus_force_level(enum bus_force_line line);

#endif /* BUS_FORCE_H_ */
