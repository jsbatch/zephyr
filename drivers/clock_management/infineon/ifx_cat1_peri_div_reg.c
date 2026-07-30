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
 * Ref: PSOC Control C3 Registers Reference Manual, PERI_PCLK0_GR0_DIV_CMD.
 * Setting PA_TYPE_SEL/PA_DIV_SEL to all-ones selects clk_peri as the
 * phase-alignment reference, matching the vendor enable sequence.
 */
#define IFX_PERI_DIV_CMD_ENABLE      BIT(31)
#define IFX_PERI_DIV_CMD_PA_TYPE_SEL (0x3U << 24)
#define IFX_PERI_DIV_CMD_PA_DIV_SEL  (0xFFU << 16)
#define IFX_PERI_DIV_CMD_TYPE_SEL(t) (((uint32_t)(t) & 0x3U) << 8)
#define IFX_PERI_DIV_CMD_DIV_SEL(c)  (((uint32_t)(c) & 0xFFU) << 0)

struct ifx_cat1_peri_div_reg_data {
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	mem_addr_t ctl_reg;
	mem_addr_t cmd_reg;
	uint16_t divider;
	uint8_t frac_divider;
	uint8_t div_lsb;
	uint8_t div_width;
	uint8_t div_type;
	uint8_t channel;
};

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
}

static int ifx_cat1_peri_div_configure(const struct clk *clk_hw, const void *cfg)
{
	struct ifx_cat1_peri_div_reg_data *data = clk_hw->hw_data;
	uint32_t divider = (uint32_t)(uintptr_t)cfg;

	if (divider == 0U) {
		return -EINVAL;
	}

	/* Program the integer divide value (hardware stores value - 1). */
	ifx_cat1_reg_field_set(data->ctl_reg, data->div_lsb, data->div_width, divider - 1U);

	/* Enable with phase alignment to clk_peri. */
	sys_write32(IFX_PERI_DIV_CMD_ENABLE | IFX_PERI_DIV_CMD_PA_TYPE_SEL |
			    IFX_PERI_DIV_CMD_PA_DIV_SEL | IFX_PERI_DIV_CMD_TYPE_SEL(data->div_type) |
			    IFX_PERI_DIV_CMD_DIV_SEL(data->channel),
		    data->cmd_reg);

	/* Buffered-write flush read (Registers RM, PERI_PCLK0_GR0_DIV_CMD). */
	(void)sys_read32(data->cmd_reg);

	data->divider = (uint16_t)divider;

	return 0;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
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
}
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
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
}
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
