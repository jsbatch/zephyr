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
	/* Parent handle from the "input" phandle; must stay the first member. */
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	/*
	 * Legal maximum output in Hz; 0 disables the check. Enforced only in
	 * runtime mode: with no vendor frequency oracle the output rate is known
	 * only where the framework supplies the parent rate.
	 */
	uint32_t max_freq;
	mem_addr_t reg;      /* CLK_ROOT_SELECT register for this root. */
	uint8_t mux_lsb;     /* Source-select (ROOT_MUX) field position. */
	uint8_t mux_width;   /* Source-select field width. */
	/*
	 * Integer-divider field position: ROOT_DIV_INT on SRSS layouts that
	 * provide it, ROOT_DIV on those that do not, so it comes from devicetree
	 * rather than being fixed here.
	 */
	uint8_t div_lsb;
	uint8_t div_width;   /* Integer-divider field width. */
	uint8_t enable_bit;  /* Clock-root enable bit position. */
	uint8_t source_path; /* CLK_PATH index driven onto the source mux. */
	uint8_t instance;    /* clk_hf root index; used only to guard the CPU root. */
	/*
	 * Encoded divider: the divide factor is divider + 1. Shadows the hardware
	 * field so recalc_rate() needs no register read; configure() refreshes it
	 * after a successful write.
	 */
	uint8_t divider;
};

/**
 * @brief Compute this root's output from its parent rate.
 *
 * @param clk_hw      Clock object for this clk_hf root.
 * @param parent_rate Rate arriving from the selected clock path.
 *
 * @return Output rate in Hz.
 */
static clock_freq_t ifx_cat1_clk_hf_recalc_rate(const struct clk *clk_hw, clock_freq_t parent_rate)
{
	const struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;

	return parent_rate / (data->divider + 1U);
} /* ifx_cat1_clk_hf_recalc_rate() */

/**
 * @brief Point this root at its source path, apply a divider, and enable it.
 *
 * The legal-maximum check cannot run here because a register-direct producer
 * has no way to read back the resulting frequency. It happens in
 * configure_recalc(), which only exists in runtime mode.
 *
 * @param clk_hw Clock object for this clk_hf root.
 * @param cfg    Divider selector, passed as an integer-valued pointer.
 *
 * @return 0; the register writes cannot fail.
 */
static int ifx_cat1_clk_hf_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	ifx_cat1_reg_field_set(data->reg, data->mux_lsb, data->mux_width, data->source_path);
	ifx_cat1_reg_field_set(data->reg, data->div_lsb, data->div_width, divider);
	ifx_cat1_reg_field_set(data->reg, data->enable_bit, 1U, 1U);

	data->divider = (uint8_t)divider;

	return 0;
} /* ifx_cat1_clk_hf_configure() */

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
/**
 * @brief Predict the output a pending divider would produce, and enforce the
 *        legal maximum.
 *
 * @param clk_hw      Clock object for this clk_hf root.
 * @param cfg         Divider selector, passed as an integer-valued pointer.
 * @param parent_rate Rate arriving from the selected clock path.
 *
 * @retval -EINVAL if the resulting rate would exceed the legal maximum.
 * @return Rate the root would output otherwise.
 */
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
} /* ifx_cat1_clk_hf_configure_recalc() */
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
/**
 * @brief Find the closest achievable rate, optionally applying it.
 *
 * The divider is an integer 1..16, so the result is the requested rate rounded
 * down to the nearest achievable step.
 *
 * @param clk_hw      Clock object for this clk_hf root.
 * @param rate_req    Requested output rate in Hz.
 * @param parent_rate Rate arriving from the selected clock path.
 * @param commit      Program the divider when true; only evaluate when false.
 *
 * @retval -EINVAL if the requested rate is not positive.
 * @return Achievable rate in Hz otherwise.
 */
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
} /* ifx_cat1_clk_hf_best_rate() */
#endif

/**
 * @brief Gate or ungate this root.
 *
 * @param clk_hw Clock object for this clk_hf root.
 * @param on     True to enable, false to gate.
 *
 * @retval -ENOSYS if this root feeds the CPU and so cannot be gated.
 * @retval 0 on success.
 */
static int ifx_cat1_clk_hf_on_off(const struct clk *clk_hw, bool on)
{
	const struct ifx_cat1_clk_hf_reg_data *data = clk_hw->hw_data;

	/*
	 * clk_hf0 feeds the CPU, so the hardware root must never be gated.
	 * Nothing below this check would stop the write, so it is the only
	 * guard. Reporting the root as not gateable lets the framework carry on
	 * walking the parent chain.
	 */
	if ((!on) && (data->instance == 0U)) {
		return -ENOSYS;
	}

	ifx_cat1_reg_field_set(data->reg, data->enable_bit, 1U, on ? 1U : 0U);

	return 0;
} /* ifx_cat1_clk_hf_on_off() */

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
