/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "bus_force.h"

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec sda =
	GPIO_DT_SPEC_GET_OR(ZEPHYR_USER_NODE, sda_gpios, {0});
static const struct gpio_dt_spec scl =
	GPIO_DT_SPEC_GET_OR(ZEPHYR_USER_NODE, scl_gpios, {0});

static void pulse_expiry(struct k_timer *timer);

static K_TIMER_DEFINE(pulse_timer, pulse_expiry, NULL);
static K_SEM_DEFINE(pulse_done, 0, 1);

bool bus_force_available(void)
{
	/*
	 * A devicetree property says which pins would be used, not that a
	 * jumper is fitted. Running the hang and recovery conditions against
	 * disconnected pins produces failures that look like driver defects,
	 * so the wiring must be declared explicitly by the twister scenario
	 * that carries the i2c_bus_fault fixture.
	 */
	if (!IS_ENABLED(CONFIG_I2C_FAULT_TEST_BUS_FAULT_WIRED)) {
		return false;
	}

	return (sda.port != NULL) && (scl.port != NULL) &&
	       gpio_is_ready_dt(&sda) && gpio_is_ready_dt(&scl);
} /* bus_force_available() */

/* Select the pin backing a line, or NULL when the line names both. */
static const struct gpio_dt_spec *line_pin(enum bus_force_line line)
{
	if (line == BUS_FORCE_SDA) {
		return &sda;
	}

	if (line == BUS_FORCE_SCL) {
		return &scl;
	}

	return NULL;
} /* line_pin() */

int bus_force_arm(enum bus_force_line line)
{
	const struct gpio_dt_spec *pin = line_pin(line);

	if (!bus_force_available()) {
		return -ENODEV;
	}

	if (pin == NULL) {
		return -EINVAL;
	}

	return gpio_pin_configure_dt(pin, GPIO_OUTPUT_ACTIVE);
} /* bus_force_arm() */

void bus_force_assert_low(enum bus_force_line line)
{
	const struct gpio_dt_spec *pin = line_pin(line);

	if (pin != NULL) {
		(void)gpio_pin_set_dt(pin, 0);
	}
} /* bus_force_assert_low() */

/*
 * Release the pulsed line. Runs in the system timer's context, so it does
 * nothing but write the pin and signal the waiter.
 */
static void pulse_expiry(struct k_timer *timer)
{
	enum bus_force_line line =
		(enum bus_force_line)POINTER_TO_UINT(k_timer_user_data_get(timer));
	const struct gpio_dt_spec *pin = line_pin(line);

	if (pin != NULL) {
		(void)gpio_pin_set_dt(pin, 1);
	}

	k_sem_give(&pulse_done);
} /* pulse_expiry() */

void bus_force_pulse_low(enum bus_force_line line, uint32_t span_us)
{
	const struct gpio_dt_spec *pin = line_pin(line);

	if (pin == NULL) {
		return;
	}

	k_sem_reset(&pulse_done);
	k_timer_user_data_set(&pulse_timer, UINT_TO_POINTER((uint32_t)line));
	(void)gpio_pin_set_dt(pin, 0);
	k_timer_start(&pulse_timer, K_USEC(span_us), K_NO_WAIT);
} /* bus_force_pulse_low() */

int bus_force_pulse_wait(uint32_t timeout_ms)
{
	return k_sem_take(&pulse_done, K_MSEC(timeout_ms));
} /* bus_force_pulse_wait() */

/*
 * Both pins start as inputs. A pin only becomes an output for as long as a
 * line is held low, so the helper contributes nothing to the bus at rest and
 * an aborted test cannot leave the bus stuck.
 */
int bus_force_init(void)
{
	int ret;

	if (!bus_force_available()) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&sda, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_configure_dt(&scl, GPIO_INPUT);
} /* bus_force_init() */

/* Drive one pin to a hard low. The open-drain bus turns that into a hold. */
static int hold_low(const struct gpio_dt_spec *pin)
{
	int ret;

	ret = gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_set_dt(pin, 0);
} /* hold_low() */

int bus_force_low(enum bus_force_line line)
{
	int ret;

	if (!bus_force_available()) {
		return -ENODEV;
	}

	if ((line == BUS_FORCE_SDA) || (line == BUS_FORCE_BOTH)) {
		ret = hold_low(&sda);
		if (ret < 0) {
			return ret;
		}
	}

	if ((line == BUS_FORCE_SCL) || (line == BUS_FORCE_BOTH)) {
		ret = hold_low(&scl);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
} /* bus_force_low() */

int bus_force_release(void)
{
	int ret;

	if (!bus_force_available()) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&sda, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_configure_dt(&scl, GPIO_INPUT);
} /* bus_force_release() */

int bus_force_level(enum bus_force_line line)
{
	const struct gpio_dt_spec *pin = line_pin(line);
	int ret;

	if (!bus_force_available()) {
		return -ENODEV;
	}

	if (pin == NULL) {
		return -EINVAL;
	}

	/*
	 * Sampling a pin that is currently driving the line low would read
	 * back the drive rather than the bus, so the pin is put back to an
	 * input first. The caller must not be holding the line it samples.
	 */
	ret = gpio_pin_configure_dt(pin, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_get_dt(pin);
} /* bus_force_level() */
