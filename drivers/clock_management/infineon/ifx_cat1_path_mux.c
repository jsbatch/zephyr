/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Clock-path multiplexer producer for the Infineon CAT1
 *        clock-management prototype.
 *
 * The framework selects a parent by zero-based index. PSC3 selects a path
 * source by a hardware source enum through Cy_SysClk_ClkPathSetSource(), so the
 * source-selects table maps each devicetree input index to its source enum.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>
#include <zephyr/drivers/clock_management/clock_helpers.h>

#include <infineon_kconfig.h>

#include <cy_sysclk.h>

#define DT_DRV_COMPAT infineon_cat1_path_mux

struct ifx_cat1_path_mux_config {
	MUX_CLK_SUBSYS_DATA_DEFINE
	const uint32_t *source_selects;
	uint8_t instance;
};

static int ifx_cat1_path_mux_configure(const struct clk *clk_hw, const void *mux)
{
	const struct ifx_cat1_path_mux_config *config = clk_hw->hw_data;
	uint32_t idx = (uint32_t)(uintptr_t)mux;

	if (idx >= config->parent_cnt) {
		return -EINVAL;
	}

	/*
	 * A null parent slot is an input the board does not populate, so it is
	 * listed for index stability but cannot be selected.
	 */
	if (config->parents[idx] == NULL) {
		return -ENOTCONN;
	}

	Cy_SysClk_ClkPathSetSource(config->instance, config->source_selects[idx]);

	return 0;
}

static int ifx_cat1_path_mux_get_parent(const struct clk *clk_hw)
{
	const struct ifx_cat1_path_mux_config *config = clk_hw->hw_data;
	uint32_t sel = (uint32_t)Cy_SysClk_ClkPathGetSource(config->instance);

	/* Reverse-map the hardware source value to its input-sources index. */
	for (uint8_t i = 0; i < config->parent_cnt; i++) {
		if (config->source_selects[i] == sel) {
			/* Selecting an unpopulated input yields no rate. */
			if (config->parents[i] == NULL) {
				return -ENOTCONN;
			}
			return i;
		}
	}

	return -ENOTCONN;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
static int ifx_cat1_path_mux_configure_recalc(const struct clk *clk_hw, const void *mux)
{
	const struct ifx_cat1_path_mux_config *config = clk_hw->hw_data;

	return clock_management_mux_validate_parent(config->parent_cnt,
						    (uint32_t)(uintptr_t)mux);
}

static int ifx_cat1_path_mux_validate_parent(const struct clk *clk_hw,
					     clock_freq_t parent_freq, uint8_t new_idx)
{
	ARG_UNUSED(parent_freq);
	const struct ifx_cat1_path_mux_config *config = clk_hw->hw_data;

	if ((new_idx < config->parent_cnt) && (config->parents[new_idx] == NULL)) {
		return -ENOTCONN;
	}

	return clock_management_mux_validate_parent(config->parent_cnt, (uint32_t)new_idx);
}
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
static int ifx_cat1_path_mux_set_parent(const struct clk *clk_hw, uint8_t new_idx)
{
	return ifx_cat1_path_mux_configure(clk_hw, (const void *)(uintptr_t)new_idx);
}
#endif

const struct clock_management_mux_api ifx_cat1_path_mux_api = {
	.shared.configure = ifx_cat1_path_mux_configure,
	.get_parent = ifx_cat1_path_mux_get_parent,
#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	.mux_configure_recalc = ifx_cat1_path_mux_configure_recalc,
	.mux_validate_parent = ifx_cat1_path_mux_validate_parent,
#endif
#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
	.set_parent = ifx_cat1_path_mux_set_parent,
#endif
};

/*
 * An input whose devicetree node is disabled describes hardware the board does
 * not populate, and no clock object exists for it. Emit a null slot rather than
 * a reference, so the entry keeps its position: the specifier cell in every
 * clock-state is an index into this array, and dropping absent inputs would
 * silently renumber the ones that remain. Selecting a null slot reports
 * -ENOTCONN, which the framework already treats as "no rate".
 */
#define IFX_MUX_INPUT(node_id, prop, idx)                                      \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_PHANDLE_BY_IDX(node_id, prop, idx),  \
				       okay),                                  \
		    (CLOCK_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))),     \
		    (NULL)),

#define IFX_MUX_SELECT(node_id, prop, idx)                                     \
	DT_PROP_BY_IDX(node_id, prop, idx),

#define IFX_CAT1_PATH_MUX_DEFINE(inst)                                         \
	static const struct clk *const ifx_mux_##inst##_parents[] = {          \
		DT_INST_FOREACH_PROP_ELEM(inst, input_sources, IFX_MUX_INPUT)  \
	};                                                                     \
	static const uint32_t ifx_mux_##inst##_selects[] = {                   \
		DT_INST_FOREACH_PROP_ELEM(inst, source_selects, IFX_MUX_SELECT)\
	};                                                                     \
	static const struct ifx_cat1_path_mux_config ifx_mux_##inst = {        \
		MUX_CLK_SUBSYS_DATA_INIT(ifx_mux_##inst##_parents,             \
					 DT_INST_PROP_LEN(inst, input_sources))\
			.source_selects = ifx_mux_##inst##_selects,            \
		.instance = (uint8_t)DT_INST_PROP(inst, instance),             \
	};                                                                     \
	MUX_CLOCK_DT_INST_DEFINE(inst, &ifx_mux_##inst, &ifx_cat1_path_mux_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_PATH_MUX_DEFINE)
