/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Register-direct clock-path multiplexer producer (Infineon CAT1).
 *
 * Family-agnostic: the path-select register address and the PATH_MUX bitfield
 * position come from devicetree. The framework selects a parent by index; the
 * source-selects table maps that index to the hardware source value written to
 * the bitfield.
 */

#include <zephyr/drivers/clock_management/clock_driver.h>
#include <zephyr/drivers/clock_management/clock_helpers.h>

#include "ifx_cat1_clock_reg.h"

#define DT_DRV_COMPAT infineon_cat1_path_mux

struct ifx_cat1_path_mux_reg_config {
	/* Parent array and count; must stay the first member. */
	MUX_CLK_SUBSYS_DATA_DEFINE
	/*
	 * Hardware source value per input, parallel to the parent array. The
	 * framework works in zero-based indices, the hardware does not, so this
	 * table is the translation between them.
	 */
	const uint32_t *source_selects;
	mem_addr_t reg;      /* Path-select register for this CLK_PATH. */
	uint8_t field_lsb;   /* PATH_MUX field position. */
	uint8_t field_width; /* PATH_MUX field width. */
};

/**
 * @brief Select a path-mux input by framework index.
 *
 * @param clk_hw Clock object for this path mux.
 * @param mux    Input-sources index, passed as an integer-valued pointer.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the index is out of range.
 * @retval -ENOTCONN if the selected input is not populated on this board.
 */
static int ifx_cat1_path_mux_configure(const struct clk *clk_hw, const void *mux)
{
	const struct ifx_cat1_path_mux_reg_config *config = clk_hw->hw_data;
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

	/*
	 * Switching away from the current source needs four cycles of it, and
	 * that source must not be gated until they have elapsed (Architecture
	 * TRM Table 110). Nothing here enforces the delay.
	 */
	ifx_cat1_reg_field_set(config->reg, config->field_lsb, config->field_width,
			       config->source_selects[idx]);

	return 0;
} /* ifx_cat1_path_mux_configure() */

/**
 * @brief Report which input the hardware currently selects.
 *
 * @param clk_hw Clock object for this path mux.
 *
 * @return Input-sources index, or -ENOTCONN if the hardware selects a value
 *         this node does not list or an input the board does not populate.
 */
static int ifx_cat1_path_mux_get_parent(const struct clk *clk_hw)
{
	const struct ifx_cat1_path_mux_reg_config *config = clk_hw->hw_data;
	uint32_t sel = ifx_cat1_reg_field_get(config->reg, config->field_lsb, config->field_width);

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
} /* ifx_cat1_path_mux_get_parent() */

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
/**
 * @brief Validate a pending selection without touching hardware.
 *
 * @param clk_hw Clock object for this path mux.
 * @param mux    Input-sources index, passed as an integer-valued pointer.
 *
 * @return 0 if the index is selectable, negative errno otherwise.
 */
static int ifx_cat1_path_mux_configure_recalc(const struct clk *clk_hw, const void *mux)
{
	const struct ifx_cat1_path_mux_reg_config *config = clk_hw->hw_data;

	return clock_management_mux_validate_parent(config->parent_cnt,
						    (uint32_t)(uintptr_t)mux);
} /* ifx_cat1_path_mux_configure_recalc() */

/**
 * @brief Reject a reparent onto an input the board does not populate.
 *
 * @param clk_hw      Clock object for this path mux.
 * @param parent_freq Candidate parent rate; not used, selection is index-based.
 * @param new_idx     Proposed input-sources index.
 *
 * @retval -ENOTCONN if the proposed input has no clock object.
 * @return Otherwise the framework's own range check.
 */
static int ifx_cat1_path_mux_validate_parent(const struct clk *clk_hw,
					     clock_freq_t parent_freq, uint8_t new_idx)
{
	ARG_UNUSED(parent_freq);
	const struct ifx_cat1_path_mux_reg_config *config = clk_hw->hw_data;

	if ((new_idx < config->parent_cnt) && (config->parents[new_idx] == NULL)) {
		return -ENOTCONN;
	}

	return clock_management_mux_validate_parent(config->parent_cnt, (uint32_t)new_idx);
} /* ifx_cat1_path_mux_validate_parent() */
#endif

#if defined(CONFIG_CLOCK_MANAGEMENT_SET_RATE)
/**
 * @brief Reparent the mux at runtime.
 *
 * @param clk_hw  Clock object for this path mux.
 * @param new_idx Input-sources index to select.
 *
 * @return 0 on success, negative errno otherwise.
 */
static int ifx_cat1_path_mux_set_parent(const struct clk *clk_hw, uint8_t new_idx)
{
	return ifx_cat1_path_mux_configure(clk_hw, (const void *)(uintptr_t)new_idx);
} /* ifx_cat1_path_mux_set_parent() */
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
	static const struct ifx_cat1_path_mux_reg_config ifx_mux_##inst = {    \
		MUX_CLK_SUBSYS_DATA_INIT(ifx_mux_##inst##_parents,             \
					 DT_INST_PROP_LEN(inst, input_sources))\
			.source_selects = ifx_mux_##inst##_selects,            \
		.reg = (mem_addr_t)DT_INST_PROP(inst, reg_addr),               \
		.field_lsb = (uint8_t)DT_INST_PROP_OR(inst, field_lsb, 0),     \
		.field_width = (uint8_t)DT_INST_PROP_OR(inst, field_width, 3), \
	};                                                                     \
	MUX_CLOCK_DT_INST_DEFINE(inst, &ifx_mux_##inst, &ifx_cat1_path_mux_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_CAT1_PATH_MUX_DEFINE)
