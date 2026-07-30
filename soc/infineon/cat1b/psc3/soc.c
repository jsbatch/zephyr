/*
 * Copyright (c) 2025 Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <ifx_cycfg_init.h>

#if defined(CONFIG_CLOCK_MANAGEMENT) && DT_NODE_HAS_PROP(DT_NODELABEL(cpu0), clock_outputs)
#include <zephyr/init.h>
#include <zephyr/drivers/clock_management.h>
#include <cy_sysclk.h>
#include <errno.h>

#define CPU_CLOCK_NODE DT_NODELABEL(cpu0)

CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT(CPU_CLOCK_NODE);

static const struct clock_output *cpu_clock = CLOCK_MANAGEMENT_DT_GET_OUTPUT(CPU_CLOCK_NODE);

/*
 * Bring the CPU up to its full-speed system clock. An always-on system clock
 * has no peripheral driver to own it, so the SoC applies its default state at
 * boot and refreshes the CMSIS core-clock variable. This runs at PRE_KERNEL_1
 * (not the early hook) because clock_management_apply_state() takes a kernel
 * mutex, and at priority 0 so the core clock is up before peripheral drivers.
 */
static int ifx_cat1_core_clock_init(void)
{
	clock_management_state_t default_state =
		CLOCK_MANAGEMENT_DT_GET_STATE(CPU_CLOCK_NODE, default, default);
	int rate = clock_management_apply_state(cpu_clock, default_state);
	int ret;

	if (rate < 0) {
		return rate;
	}

	/*
	 * Refresh the CMSIS core-clock variable as soon as the new rate is in
	 * effect, so it cannot be left stale by a later failure.
	 */
	SystemCoreClockUpdate();

	/*
	 * Register the CPU as a user of its clock: applying a state does not
	 * mark a clock as in use, only clock_management_on() raises the usage
	 * count that clock_management_disable_unused() tests. The API documents
	 * -ENOSYS for a clock that implements no on_off, which is the case for
	 * the DPLL and path multiplexer in this chain, so that return is
	 * expected here rather than fatal.
	 */
	ret = clock_management_on(cpu_clock);
	if ((ret < 0) && (ret != -ENOSYS)) {
		return ret;
	}

#if !defined(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_LEGACY_BRINGUP)
	/*
	 * Gating unused clocks is only safe once every consumer is a
	 * clock-management consumer. A driver still on the legacy clock path
	 * raises no usage count, so a clock it depends on would look unused and
	 * be gated out from under it. The legacy bring-up being enabled
	 * is exactly the signal that such drivers still exist.
	 */
	clock_management_disable_unused();
#endif

	return 0;
}

SYS_INIT(ifx_cat1_core_clock_init, PRE_KERNEL_1, 0);
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_LEGACY_BRINGUP) && \
	DT_NODE_EXISTS(DT_NODELABEL(clk_legacy_bringup))
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_management.h>
#include <errno.h>

#define BRINGUP_NODE DT_NODELABEL(clk_legacy_bringup)

CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_IDX(BRINGUP_NODE, 0);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_IDX(BRINGUP_NODE, 1);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_IDX(BRINGUP_NODE, 2);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_IDX(BRINGUP_NODE, 3);

static const struct clock_output *const bringup_clocks[] = {
	CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_IDX(BRINGUP_NODE, 0),
	CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_IDX(BRINGUP_NODE, 1),
	CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_IDX(BRINGUP_NODE, 2),
	CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_IDX(BRINGUP_NODE, 3),
};

static const clock_management_state_t bringup_states[] = {
	CLOCK_MANAGEMENT_DT_GET_STATE(BRINGUP_NODE, hf1, default),
	CLOCK_MANAGEMENT_DT_GET_STATE(BRINGUP_NODE, hf2, default),
	CLOCK_MANAGEMENT_DT_GET_STATE(BRINGUP_NODE, hf3, default),
	CLOCK_MANAGEMENT_DT_GET_STATE(BRINGUP_NODE, hf4, default),
};

BUILD_ASSERT(ARRAY_SIZE(bringup_clocks) == ARRAY_SIZE(bringup_states),
	     "bring-up clock and state tables must stay in step");

/*
 * Bring up the peripheral-group clk_hf roots that have no clock-management
 * consumer yet, so a peripheral driver still on the legacy clock path finds its
 * source clock running. Runs at PRE_KERNEL_1 because apply_state() takes a
 * kernel mutex, after the core clock (priority 0) so the shared DPLLs are
 * already configured.
 */
static int ifx_cat1_legacy_clock_bringup(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(bringup_clocks); i++) {
		int rate = clock_management_apply_state(bringup_clocks[i], bringup_states[i]);
		int ret;

		if (rate < 0) {
			return rate;
		}

		/*
		 * The drivers on these roots are not clock-management consumers
		 * yet, so legacy bring-up holds the usage count on their behalf.
		 * -ENOSYS means a clock on the path implements no on_off and so
		 * cannot be gated, which is not a failure.
		 */
		ret = clock_management_on(bringup_clocks[i]);
		if ((ret < 0) && (ret != -ENOSYS)) {
			return ret;
		}
	}

	return 0;
}

SYS_INIT(ifx_cat1_legacy_clock_bringup, PRE_KERNEL_1, 1);
#endif

void soc_early_init_hook(void)
{
	ifx_cycfg_init();
}
