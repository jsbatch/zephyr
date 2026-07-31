/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Peripheral clock divider (peri-div) producer for the Infineon CAT1
 *        clock-management prototype.
 *
 * Applies an integer (or fractional) divide to its clk_hf source. The divide
 * value is 1-based, matching the existing infineon,peri-div clock-control
 * binding. Hardware programming reuses the shared PDL peripheral-divider
 * helpers so behaviour matches the clock-control driver.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>

#include <infineon_kconfig.h>
#include <zephyr/drivers/clock_control/clock_control_ifx_cat1.h>

#include <cy_sysclk.h>

#define DT_DRV_COMPAT infineon_cat1_peri_div

struct ifx_cat1_peri_div_data {
	/* Parent handle from the "input" phandle; must stay the first member. */
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	struct ifx_cat1_clock clock; /* Peripheral-group and channel identity. */
	uint16_t divider;      /* Encoded integer divider; divide factor is divider + 1. */
	uint8_t frac_divider;  /* Fractional remainder in 1/32 steps. */
	uint8_t div_type;      /* CY_SYSCLK_DIV_*; selects integer or fractional programming. */
};

/**
 * @brief Build the peripheral-clock destination selector for this channel.
 *
 * Only SoC families whose peripheral clock is addressed by group and instance
 * need a composed value; the rest address the divider directly.
 *
 * @param data Divider state carrying the group and instance identity.
 *
 * @return Destination selector for the peripheral-clock helpers.
 */
static en_clk_dst_t ifx_cat1_peri_div_dst(const struct ifx_cat1_peri_div_data *data)
{
	en_clk_dst_t clk_dst = 0;

#if defined(COMPONENT_CAT1B) || defined(COMPONENT_CAT1C) ||                     \
	defined(CONFIG_SOC_FAMILY_INFINEON_EDGE)
	clk_dst |= ((uint32_t)data->clock.group << PERI_PCLK_GR_NUM_Pos);
	clk_dst |= ((uint32_t)data->clock.instance << PERI_PCLK_INST_NUM_Pos);
#else
	ARG_UNUSED(data);
#endif
	return clk_dst;
} /* ifx_cat1_peri_div_dst() */

/**
 * @brief Compute this divider's output from its parent rate.
 *
 * @param clk_hw      Clock object for this peripheral divider.
 * @param parent_rate Rate arriving from the peripheral clock root.
 *
 * @retval -EINVAL if both divider parts are zero.
 * @return Output rate in Hz otherwise.
 */
static clock_freq_t ifx_cat1_peri_div_recalc_rate(const struct clk *clk_hw,
						  clock_freq_t parent_rate)
{
	const struct ifx_cat1_peri_div_data *data = clk_hw->hw_data;
	uint32_t scaled = ((uint32_t)data->divider << 5) + data->frac_divider;

	if (scaled == 0U) {
		return -EINVAL;
	}

	/* Fractional dividers carry the fraction in 1/32 steps. */
	return (clock_freq_t)(((uint64_t)parent_rate << 5) / scaled);
} /* ifx_cat1_peri_div_recalc_rate() */

/**
 * @brief Program and enable this peripheral divider.
 *
 * @param clk_hw Clock object for this peripheral divider.
 * @param cfg    One-based divide value, passed as an integer-valued pointer.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the requested divide value is zero.
 * @retval -EIO if a peripheral-clock helper rejects the request.
 */
static int ifx_cat1_peri_div_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_peri_div_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;
	en_clk_dst_t clk_dst = ifx_cat1_peri_div_dst(data);

	if (divider == 0U) {
		return -EINVAL;
	}

	if ((data->div_type == CY_SYSCLK_DIV_8_BIT) ||
	    (data->div_type == CY_SYSCLK_DIV_16_BIT)) {
		if (ifx_cat1_utils_peri_pclk_set_divider(clk_dst, &data->clock,
							 divider - 1U) != CY_RSLT_SUCCESS) {
			return -EIO;
		}
	} else {
		if (ifx_cat1_utils_peri_pclk_set_frac_divider(clk_dst, &data->clock, divider - 1U,
							      data->frac_divider) !=
		    CY_RSLT_SUCCESS) {
			return -EIO;
		}
	}

	if (ifx_cat1_utils_peri_pclk_enable_divider(clk_dst, &data->clock) != CY_RSLT_SUCCESS) {
		return -EIO;
	}

	data->divider = (uint16_t)divider;

	return 0;
} /* ifx_cat1_peri_div_configure() */

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
/**
 * @brief Predict the output a pending divide value would produce.
 *
 * @param clk_hw      Clock object for this peripheral divider.
 * @param cfg         One-based divide value, passed as an integer-valued pointer.
 * @param parent_rate Rate arriving from the peripheral clock root.
 *
 * @retval -EINVAL if both divider parts would be zero.
 * @return Rate the divider would output otherwise.
 */
static clock_freq_t ifx_cat1_peri_div_configure_recalc(const struct clk *clk_hw, const void *cfg,
						       clock_freq_t parent_rate)
{
	const struct ifx_cat1_peri_div_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;
	uint32_t scaled = (divider << 5) + data->frac_divider;

	if (scaled == 0U) {
		return -EINVAL;
	}

	return (clock_freq_t)(((uint64_t)parent_rate << 5) / scaled);
} /* ifx_cat1_peri_div_configure_recalc() */
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
/**
 * @brief Find the closest achievable rate, optionally applying it.
 *
 * Integer division only; the fractional part is left as configured.
 *
 * @param clk_hw      Clock object for this peripheral divider.
 * @param rate_req    Requested output rate in Hz.
 * @param parent_rate Rate arriving from the peripheral clock root.
 * @param commit      Program the divider when true; only evaluate when false.
 *
 * @retval -EINVAL if the requested rate is not positive.
 * @return Achievable rate in Hz, or an error from the programming step.
 */
static clock_freq_t ifx_cat1_peri_div_best_rate(const struct clk *clk_hw, clock_freq_t rate_req,
						clock_freq_t parent_rate, bool commit)
{
	uint32_t divider;

	if (rate_req <= 0) {
		return -EINVAL;
	}

	divider = MAX((uint32_t)(parent_rate / rate_req), 1U);

	if (commit) {
		int ret = ifx_cat1_peri_div_configure(clk_hw, (const void *)(uintptr_t)divider);

		if (ret < 0) {
			return ret;
		}
	}

	return parent_rate / divider;
} /* ifx_cat1_peri_div_best_rate() */
#endif

const struct clock_management_standard_api ifx_cat1_peri_div_api = {
	.recalc_rate = ifx_cat1_peri_div_recalc_rate,
	.shared.configure = ifx_cat1_peri_div_configure,
#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	.configure_recalc = ifx_cat1_peri_div_configure_recalc,
#endif
#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
	.best_rate = ifx_cat1_peri_div_best_rate,
#endif
};

#define IFX_CAT1_PERI_DIV_DEFINE(inst)                                         \
	static struct ifx_cat1_peri_div_data ifx_cat1_peri_div_data_##inst = {  \
		STANDARD_CLK_SUBSYS_DATA_INIT(                                 \
			CLOCK_DT_GET(DT_INST_PHANDLE(inst, input)))            \
			.clock = {                                             \
			.block = IFX_CAT1_PERIPHERAL_GROUP_ADJUST(             \
				DT_INST_PROP_BY_IDX(inst, peri_group, 1),      \
				DT_INST_PROP(inst, div_type)),                 \
			.channel = DT_INST_PROP(inst, channel),                \
			.instance = DT_INST_PROP_BY_IDX(inst, peri_group, 0),  \
			.group = DT_INST_PROP_BY_IDX(inst, peri_group, 1),     \
		},                                                             \
		.divider = DT_INST_PROP_OR(inst, clock_div, 1),                \
		.frac_divider = DT_INST_PROP_OR(inst, div_frac_value, 0),      \
		.div_type = DT_INST_PROP(inst, div_type),                      \
	};                                                                     \
	CLOCK_DT_INST_DEFINE(inst, &ifx_cat1_peri_div_data_##inst, &ifx_cat1_peri_div_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_PERI_DIV_DEFINE)
