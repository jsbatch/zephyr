/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Low-power DPLL (DPLL_LP) producer for the Infineon CAT1
 *        clock-management prototype.
 *
 * The output rate is computed from the devicetree divider fields. The DPLL
 * programming sequence is a validated calibration/trim procedure and is
 * retained through the vendor library; both the PDL and register-direct
 * backends use this producer unchanged, because a register-direct
 * reimplementation of the DPLL trim table is out of scope and error-prone.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>
#include <zephyr/logging/log.h>

#include <infineon_kconfig.h>

#include <cy_sysclk.h>

#define DT_DRV_COMPAT infineon_cat1_dpll

LOG_MODULE_REGISTER(ifx_cat1_dpll, CONFIG_CLOCK_MANAGEMENT_LOG_LEVEL);

struct ifx_cat1_dpll_data {
	STANDARD_CLK_SUBSYS_DATA_DEFINE
	uint16_t feedback_div;
	uint8_t reference_div;
	uint8_t output_div;
	uint32_t fraction_div;
	uint8_t instance;
	bool dco_mode;
};

static clock_freq_t ifx_cat1_dpll_recalc_rate(const struct clk *clk_hw, clock_freq_t parent_rate)
{
	const struct ifx_cat1_dpll_data *data = clk_hw->hw_data;
	uint64_t num;
	uint32_t den;

	if ((data->reference_div == 0U) || (data->output_div == 0U)) {
		return -EINVAL;
	}

	/* Fout = Fref * (feedback + fraction/2^24) / (reference * output). */
	num = ((uint64_t)parent_rate * data->feedback_div) +
	      (((uint64_t)parent_rate * data->fraction_div) >> 24);
	den = (uint32_t)data->reference_div * data->output_div;

	return (clock_freq_t)(num / den);
}

#if defined(CONFIG_SOC_SERIES_PSC3)
static int ifx_cat1_dpll_program(const struct ifx_cat1_dpll_data *data)
{
	/*
	 * Validated DPLL_LP configuration. The divider fields come from
	 * devicetree; the remaining fields are the silicon-validated trim/loop
	 * constants and must not be re-derived.
	 */
	cy_stc_dpll_lp_config_t lp_config = {
		.feedbackDiv = data->feedback_div,
		.referenceDiv = data->reference_div,
		.outputDiv = data->output_div,
		.pllDcoMode = data->dco_mode,
		.outputMode = CY_SYSCLK_FLLPLL_OUTPUT_AUTO,
		.fracDiv = data->fraction_div,
		.fracDitherEn = false,
		.fracEn = true,
		.accMode = 0x1U,
		.tdcMode = 0x1U,
		.kiInt = 0x24U,
		.kpInt = 0x1CU,
		.kiAccInt = 0x23U,
		.kpAccInt = 0x1AU,
		.kiFrac = 0x24U,
		.kpFrac = 0x20U,
		.kiAccFrac = 0x23U,
		.kpAccFrac = 0x1AU,
		.kiSscg = 0x18U,
		.kpSscg = 0x18U,
		.kiAccSscg = 0x16U,
		.kpAccSscg = 0x14U,
	};
	cy_stc_pll_manual_config_t pll_config = {
		.lpPllCfg = &lp_config,
	};
	uint32_t path = (data->instance == 0U) ? SRSS_PLL_250M_0_PATH_NUM
					       : SRSS_PLL_250M_1_PATH_NUM;

	if (Cy_SysClk_PllIsEnabled(path)) {
		return 0;
	}

	Cy_SysClk_PllDisable(path);
	if (Cy_SysClk_PllManualConfigure(path, &pll_config) != CY_SYSCLK_SUCCESS) {
		return -EIO;
	}
	if (Cy_SysClk_PllEnable(path, 10000U) != CY_SYSCLK_SUCCESS) {
		return -EIO;
	}

	return 0;
}
#else
static int ifx_cat1_dpll_program(const struct ifx_cat1_dpll_data *data)
{
	ARG_UNUSED(data);

	return -ENOTSUP;
}
#endif

static int ifx_cat1_dpll_configure(const struct clk *clk_hw, const void *cfg)
{
	const struct ifx_cat1_dpll_data *data = clk_hw->hw_data;
	int ret;

	ARG_UNUSED(cfg);

	ret = ifx_cat1_dpll_program(data);
	if (ret < 0) {
		LOG_ERR("DPLL_LP%u programming failed (%d)", data->instance, ret);
	}

	return ret;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
static clock_freq_t ifx_cat1_dpll_configure_recalc(const struct clk *clk_hw, const void *cfg,
						   clock_freq_t parent_rate)
{
	ARG_UNUSED(cfg);

	return ifx_cat1_dpll_recalc_rate(clk_hw, parent_rate);
}
#endif

const struct clock_management_standard_api ifx_cat1_dpll_api = {
	.recalc_rate = ifx_cat1_dpll_recalc_rate,
	.shared.configure = ifx_cat1_dpll_configure,
#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	.configure_recalc = ifx_cat1_dpll_configure_recalc,
#endif
};

#define IFX_CAT1_DPLL_DEFINE(inst)                                             \
	static struct ifx_cat1_dpll_data ifx_cat1_dpll_data_##inst = {          \
		STANDARD_CLK_SUBSYS_DATA_INIT(                                 \
			CLOCK_DT_GET(DT_INST_PHANDLE(inst, input)))            \
			.feedback_div = DT_INST_PROP(inst, feedback_div),      \
		.reference_div = DT_INST_PROP(inst, reference_div),            \
		.output_div = DT_INST_PROP(inst, output_div),                  \
		.fraction_div = DT_INST_PROP_OR(inst, fraction_div, 0),        \
		.instance = (uint8_t)DT_INST_PROP(inst, instance),             \
		.dco_mode = DT_INST_PROP(inst, dco_mode_enable),               \
	};                                                                     \
	CLOCK_DT_INST_DEFINE(inst, &ifx_cat1_dpll_data_##inst, &ifx_cat1_dpll_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_DPLL_DEFINE)
