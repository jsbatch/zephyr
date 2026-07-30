/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief High-frequency root clock (clk_hf) producer for the Infineon CAT1
 *        clock-management prototype.
 *
 * Selects a clock path as the source and applies an integer divider. The
 * divide factor is the selector value plus one (IFX_CLK_HF_NO_DIVIDE selects
 * a divide-by-one). An optional max-frequency guards the PSC3 legal maximum.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>
#include <zephyr/logging/log.h>

#include <infineon_kconfig.h>
#include <zephyr/dt-bindings/clock/ifx_clock_source_common.h>

#include <cy_sysclk.h>

#define DT_DRV_COMPAT infineon_cat1_clk_hf

LOG_MODULE_REGISTER(ifx_cat1_clk_hf, CONFIG_CLOCK_MANAGEMENT_LOG_LEVEL);

struct ifx_cat1_clk_hf_data {
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	uint32_t max_freq;
	uint8_t source_path;
	uint8_t instance;
	uint8_t divider;
};

static clock_freq_t ifx_cat1_clk_hf_recalc_rate(const struct clk *clk_hw, clock_freq_t parent_rate)
{
	const struct ifx_cat1_clk_hf_data *data = clk_hw->hw_data;

	return parent_rate / (data->divider + 1U);
}

static int ifx_cat1_clk_hf_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_clk_hf_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	if (Cy_SysClk_ClkHfSetSource(data->instance, data->source_path) != CY_SYSCLK_SUCCESS) {
		return -EIO;
	}
	if (Cy_SysClk_ClkHfSetDivider(data->instance, divider) != CY_SYSCLK_SUCCESS) {
		return -EIO;
	}
	if (Cy_SysClk_ClkHfEnable(data->instance) != CY_SYSCLK_SUCCESS) {
		return -EIO;
	}

	data->divider = (uint8_t)divider;

	/* Legal-max enforcement lives in code because no binding expresses it. */
	if ((data->max_freq != 0U) &&
	    (Cy_SysClk_ClkHfGetFrequency(data->instance) > data->max_freq)) {
		LOG_ERR("clk_hf%u exceeds legal max %u Hz", data->instance, data->max_freq);
		return -EINVAL;
	}

	return 0;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
static clock_freq_t ifx_cat1_clk_hf_configure_recalc(const struct clk *clk_hw, const void *cfg,
						     clock_freq_t parent_rate)
{
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	return parent_rate / (divider + 1U);
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
		struct ifx_cat1_clk_hf_data *data = clk_hw->hw_data;

		if (Cy_SysClk_ClkHfSetDivider(data->instance, factor - 1U) != CY_SYSCLK_SUCCESS) {
			return -EIO;
		}
		data->divider = (uint8_t)(factor - 1U);
	}

	return parent_rate / factor;
}
#endif

static int ifx_cat1_clk_hf_on_off(const struct clk *clk_hw, bool on)
{
	const struct ifx_cat1_clk_hf_data *data = clk_hw->hw_data;
	cy_en_sysclk_status_t status;

	/*
	 * clk_hf0 feeds the CPU, so the hardware root must never be gated. The
	 * vendor library refuses the request as well. Reporting the root as not
	 * gateable lets the framework carry on walking the parent chain.
	 */
	if ((!on) && (data->instance == 0U)) {
		return -ENOSYS;
	}

	if (on) {
		status = Cy_SysClk_ClkHfEnable(data->instance);
	} else {
		status = Cy_SysClk_ClkHfDisable(data->instance);
	}

	if (status != CY_SYSCLK_SUCCESS) {
		LOG_ERR("clk_hf%u %s failed", data->instance, on ? "enable" : "disable");
		return -EIO;
	}

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
	static struct ifx_cat1_clk_hf_data ifx_cat1_clk_hf_data_##inst = {      \
		STANDARD_CLK_SUBSYS_DATA_INIT(                                 \
			CLOCK_DT_GET(DT_INST_PHANDLE(inst, input)))            \
			.max_freq = DT_INST_PROP_OR(inst, max_frequency, 0),   \
		.source_path = (uint8_t)DT_INST_PROP(inst, source_path),       \
		.instance = (uint8_t)DT_INST_PROP(inst, instance),             \
		.divider = (uint8_t)DT_INST_PROP_OR(inst, clock_div, 0),       \
	};                                                                     \
	CLOCK_DT_INST_DEFINE(inst, &ifx_cat1_clk_hf_data_##inst, &ifx_cat1_clk_hf_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_CLK_HF_DEFINE)
