/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Register-direct root clock-source producer (Infineon CAT1).
 *
 * Family-agnostic: the oscillator-enable register address and bit come from
 * devicetree. Reports a fixed rate and sets the enable bit when the framework
 * brings the clock on.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>

#include "ifx_cat1_clock_reg.h"

#define DT_DRV_COMPAT infineon_cat1_clock_source

struct ifx_cat1_clock_source_reg_config {
	clock_freq_t rate;
	mem_addr_t enable_reg;
	uint8_t enable_bit;
};

static clock_freq_t ifx_cat1_clock_source_get_rate(const struct clk *clk_hw)
{
	return ((const struct ifx_cat1_clock_source_reg_config *)clk_hw->hw_data)->rate;
}

static int ifx_cat1_clock_source_on_off(const struct clk *clk_hw, bool on)
{
	const struct ifx_cat1_clock_source_reg_config *config = clk_hw->hw_data;

	if (!on) {
		/* Shared root oscillator: leave running when this output gates. */
		return 0;
	}

	if (config->enable_reg != 0U) {
		ifx_cat1_reg_field_set(config->enable_reg, config->enable_bit, 1U, 1U);
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
	const struct ifx_cat1_clock_source_reg_config ifx_cat1_clock_source_##inst = { \
		.rate = DT_INST_PROP(inst, clock_frequency),                   \
		.enable_reg = (mem_addr_t)DT_INST_PROP_OR(inst, reg_addr, 0),  \
		.enable_bit = (uint8_t)DT_INST_PROP_OR(inst, enable_bit, 0),   \
	};                                                                     \
	ROOT_CLOCK_DT_INST_DEFINE(inst,                                        \
				  &ifx_cat1_clock_source_##inst,               \
				  &ifx_cat1_clock_source_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_CLOCK_SOURCE_DEFINE)
