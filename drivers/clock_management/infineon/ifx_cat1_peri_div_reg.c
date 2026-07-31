/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Register-direct peripheral clock divider (peri-div) producer
 *        (Infineon CAT1).
 *
 * Family-agnostic: the divider control register, the divider command register,
 * and the integer-divider bitfield position all come from devicetree. The
 * DIV_CMD enable protocol (phase-align to clk_peri, then a buffered-write flush
 * read) is common to the CAT1 PERI IP and is encoded here with register-manual
 * citations.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>

#include "ifx_cat1_clock_reg.h"

#define DT_DRV_COMPAT infineon_cat1_peri_div

/*
 * PERI_PCLK_GRx_DIV_CMD field layout, common to the CAT1 PERI IP.
 * Ref: PSOC Control C3 Registers Reference Manual, PERI_PCLK0_GR0_DIV_CMD
 * (section 11.1.1).
 *
 * PA_DIV_SEL = 255 with PA_TYPE_SEL = 3 selects clk_pclk_root[i] as the
 * phase-alignment reference, which is the all-ones encoding used here.
 */
#define IFX_PERI_DIV_CMD_ENABLE      BIT(31)
#define IFX_PERI_DIV_CMD_PA_TYPE_SEL (0x3U << 24)
#define IFX_PERI_DIV_CMD_PA_DIV_SEL  (0xFFU << 16)
#define IFX_PERI_DIV_CMD_TYPE_SEL(t) (((uint32_t)(t) & 0x3U) << 8)
#define IFX_PERI_DIV_CMD_DIV_SEL(c)  (((uint32_t)(c) & 0xFFU) << 0)

struct ifx_cat1_peri_div_reg_data {
	/* Parent handle from the "input" phandle; must stay the first member. */
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	mem_addr_t ctl_reg;   /* Divider control register holding the divide value. */
	mem_addr_t cmd_reg;   /* Divider command register that latches and enables. */
	uint16_t divider;     /* Encoded integer divider; divide factor is divider + 1. */
	uint8_t frac_divider; /* Fractional remainder in 1/32 steps. */
	uint8_t div_lsb;      /* Divide-value field position within ctl_reg. */
	uint8_t div_width;    /* Divide-value field width. */
	uint8_t div_type;     /* Divider type written to the command register. */
	uint8_t channel;      /* Divider channel selected by the command register. */
};

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
	const struct ifx_cat1_peri_div_reg_data *data = clk_hw->hw_data;
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
 */
static int ifx_cat1_peri_div_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_peri_div_reg_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	if (divider == 0U) {
		return -EINVAL;
	}

	/* Program the integer divide value (hardware stores value - 1). */
	ifx_cat1_reg_field_set(data->ctl_reg, data->div_lsb, data->div_width, divider - 1U);

	/* Enable with phase alignment to clk_pclk_root[i]. */
	sys_write32(IFX_PERI_DIV_CMD_ENABLE | IFX_PERI_DIV_CMD_PA_TYPE_SEL |
			    IFX_PERI_DIV_CMD_PA_DIV_SEL | IFX_PERI_DIV_CMD_TYPE_SEL(data->div_type) |
			    IFX_PERI_DIV_CMD_DIV_SEL(data->channel),
		    data->cmd_reg);

	/* Read back so the enable command has landed before the caller proceeds. */
	(void)sys_read32(data->cmd_reg);

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
	const struct ifx_cat1_peri_div_reg_data *data = clk_hw->hw_data;
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
	static struct ifx_cat1_peri_div_reg_data ifx_cat1_peri_div_data_##inst = { \
		STANDARD_CLK_SUBSYS_DATA_INIT(                                 \
			CLOCK_DT_GET(DT_INST_PHANDLE(inst, input)))            \
			.ctl_reg = (mem_addr_t)DT_INST_PROP(inst, reg_addr),   \
		.cmd_reg = (mem_addr_t)DT_INST_PROP(inst, cmd_reg_addr),        \
		.divider = DT_INST_PROP_OR(inst, clock_div, 1),                \
		.frac_divider = DT_INST_PROP_OR(inst, div_frac_value, 0),      \
		.div_lsb = (uint8_t)DT_INST_PROP_OR(inst, div_lsb, 8),         \
		.div_width = (uint8_t)DT_INST_PROP_OR(inst, div_width, 8),     \
		.div_type = (uint8_t)DT_INST_PROP(inst, div_type),             \
		.channel = (uint8_t)DT_INST_PROP(inst, channel),               \
	};                                                                     \
	CLOCK_DT_INST_DEFINE(inst, &ifx_cat1_peri_div_data_##inst, &ifx_cat1_peri_div_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_PERI_DIV_DEFINE)
