/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Root clock-source producer for the Infineon CAT1 clock-management
 *        prototype.
 *
 * Reports a fixed on-chip oscillator rate to the clock-management framework and
 * enables the oscillator when the framework brings the clock on. Rate math is
 * pure devicetree data; the oscillator enable reuses the same PDL primitive as
 * the existing clock-control driver.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>

#include <infineon_kconfig.h>
#include <zephyr/dt-bindings/clock/ifx_clock_source_common.h>

#include <cy_sysclk.h>

#define DT_DRV_COMPAT infineon_cat1_clock_source

struct ifx_cat1_clock_source_config {
	clock_freq_t rate;
	uint32_t system_clock;
};

static clock_freq_t ifx_cat1_clock_source_get_rate(const struct clk *clk_hw)
{
	const struct ifx_cat1_clock_source_config *config = clk_hw->hw_data;

	return config->rate;
}

static int ifx_cat1_clock_source_on_off(const struct clk *clk_hw, bool on)
{
	const struct ifx_cat1_clock_source_config *config = clk_hw->hw_data;

	if (!on) {
		/*
		 * On-chip root oscillators are shared by the whole SoC. Leave
		 * them running when the framework gates this output so the
		 * prototype cannot stall an oscillator another consumer needs.
		 */
		return 0;
	}

	switch (config->system_clock) {
	case IFX_IHO:
		Cy_SysClk_IhoEnable();
		break;
	default:
		/* Oscillator already brought up by SoC/board init. */
		break;
	}

	return 0;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
static clock_freq_t ifx_cat1_clock_source_configure_recalc(const struct clk *clk_hw,
							   const void *data)
{
	ARG_UNUSED(data);

	return ifx_cat1_clock_source_get_rate(clk_hw);
}
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
static clock_freq_t ifx_cat1_clock_source_best_rate(const struct clk *clk_hw,
						    clock_freq_t rate_req, bool commit)
{
	ARG_UNUSED(rate_req);
	ARG_UNUSED(commit);

	return ifx_cat1_clock_source_get_rate(clk_hw);
}
#endif

const struct clock_management_root_api ifx_cat1_clock_source_api = {
	.get_rate = ifx_cat1_clock_source_get_rate,
	.shared.on_off = ifx_cat1_clock_source_on_off,
#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	.root_configure_recalc = ifx_cat1_clock_source_configure_recalc,
#endif
#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
	.root_best_rate = ifx_cat1_clock_source_best_rate,
#endif
};

#define IFX_CAT1_CLOCK_SOURCE_DEFINE(inst)                                     \
	const struct ifx_cat1_clock_source_config ifx_cat1_clock_source_##inst = { \
		.rate = DT_INST_PROP(inst, clock_frequency),                   \
		.system_clock = DT_INST_PROP(inst, system_clock),              \
	};                                                                     \
	ROOT_CLOCK_DT_INST_DEFINE(inst,                                        \
				  &ifx_cat1_clock_source_##inst,               \
				  &ifx_cat1_clock_source_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_CLOCK_SOURCE_DEFINE)
