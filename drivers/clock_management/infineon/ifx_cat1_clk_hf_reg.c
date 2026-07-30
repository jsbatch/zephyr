/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Register-direct high-frequency root clock (clk_hf) producer
 *        (Infineon CAT1).
 *
 * Family-agnostic: the CLK_ROOT_SELECT register address and the source-mux,
 * integer-divider, and enable bitfield positions all come from devicetree.
 * Because the divider field position is a devicetree property, the same driver
 * serves both the 2-bit ROOT_DIV and the 4-bit ROOT_DIV_INT SRSS layouts.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>
#include <zephyr/logging/log.h>

#include "ifx_cat1_clock_reg.h"

#define DT_DRV_COMPAT infineon_cat1_clk_hf

LOG_MODULE_REGISTER(ifx_cat1_clk_hf_reg, CONFIG_CLOCK_MANAGEMENT_LOG_LEVEL);

struct ifx_cat1_clk_hf_reg_data {
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	uint32_t max_freq;
	mem_addr_t reg;
	uint8_t mux_lsb;
	uint8_t mux_width;
	uint8_t div_lsb;
	uint8_t div_width;
	uint8_t enable_bit;
	uint8_t source_path;
	uint8_t instance;
	uint8_t divider;
};

static clock_freq_t ifx_cat1_clk_hf_recalc_rate(const struct clk *clk_hw, clock_freq_t parent_rate)
{
	const struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;

	return parent_rate / (data->divider + 1U);
}

static int ifx_cat1_clk_hf_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	ifx_cat1_reg_field_set(data->reg, data->mux_lsb, data->mux_width, data->source_path);
	ifx_cat1_reg_field_set(data->reg, data->div_lsb, data->div_width, divider);
	ifx_cat1_reg_field_set(data->reg, data->enable_bit, 1U, 1U);

	data->divider = (uint8_t)divider;

	return 0;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
static clock_freq_t ifx_cat1_clk_hf_configure_recalc(const struct clk *clk_hw, const void *cfg,
						     clock_freq_t parent_rate)
{
	const struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;
	clock_freq_t out = parent_rate / (divider + 1U);

	/*
	 * A register-direct producer has no clock-frequency oracle, so the
	 * legal-max check is only possible here, where the framework supplies
	 * the parent rate. In static mode this check cannot run.
	 */
	if ((data->max_freq != 0U) && (out > (clock_freq_t)data->max_freq)) {
		LOG_ERR("clk_hf exceeds legal max %u Hz", data->max_freq);
		return -EINVAL;
	}

	return out;
}
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
static clock_freq_t ifx_cat1_clk_hf_best_rate(const struct clk *clk_hw, clock_freq_t rate_req,
					      clock_freq_t parent_rate, bool commit)
{
	uint32_t factor;

	if (rate_req <= 0) {
		return -EINVAL;
	}

	factor = CLAMP((uint32_t)(parent_rate / rate_req), 1U, 16U);

	if (commit) {
		struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;

		ifx_cat1_reg_field_set(data->reg, data->div_lsb, data->div_width, factor - 1U);
		data->divider = (uint8_t)(factor - 1U);
	}

	return parent_rate / factor;
}
#endif

static int ifx_cat1_clk_hf_on_off(const struct clk *clk_hw, bool on)
{
	const struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;

	/*
	 * clk_hf0 feeds the CPU, so the hardware root must never be gated. The
	 * vendor library refuses the request as well. Reporting the root as not
	 * gateable lets the framework carry on walking the parent chain.
	 */
	if ((!on) && (data->instance == 0U)) {
		return -ENOSYS;
	}

	ifx_cat1_reg_field_set(data->reg, data->enable_bit, 1U, on ? 1U : 0U);

	return 0;
}

const struct clock_management_standard_api ifx_cat1_clk_hf_api = {
	.recalc_rate = ifx_cat1_clk_hf_recalc_rate,
	.shared.configure = ifx_cat1_clk_hf_configure,
	.shared.on_off = ifx_cat1_clk_hf_on_off,
#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	.configure_recalc = ifx_cat1_clk_hf_configure_recalc,
#endif
#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
	.best_rate = ifx_cat1_clk_hf_best_rate,
#endif
};

#define IFX_CAT1_CLK_HF_DEFINE(inst)                                           \
	static struct ifx_cat1_clk_hf_reg_data ifx_cat1_clk_hf_data_##inst = {  \
		STANDARD_CLK_SUBSYS_DATA_INIT(                                 \
			CLOCK_DT_GET(DT_INST_PHANDLE(inst, input)))            \
			.max_freq = DT_INST_PROP_OR(inst, max_frequency, 0),   \
		.reg = (mem_addr_t)DT_INST_PROP(inst, reg_addr),               \
		.mux_lsb = (uint8_t)DT_INST_PROP_OR(inst, mux_lsb, 0),         \
		.mux_width = (uint8_t)DT_INST_PROP_OR(inst, mux_width, 4),     \
		.div_lsb = (uint8_t)DT_INST_PROP_OR(inst, div_lsb, 8),         \
		.div_width = (uint8_t)DT_INST_PROP_OR(inst, div_width, 4),     \
		.enable_bit = (uint8_t)DT_INST_PROP_OR(inst, enable_bit, 31),  \
		.source_path = (uint8_t)DT_INST_PROP(inst, source_path),       \
		.instance = (uint8_t)DT_INST_PROP(inst, instance),             \
		.divider = (uint8_t)DT_INST_PROP_OR(inst, clock_div, 0),       \
	};                                                                     \
	CLOCK_DT_INST_DEFINE(inst, &ifx_cat1_clk_hf_data_##inst, &ifx_cat1_clk_hf_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_CLK_HF_DEFINE)
