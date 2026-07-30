/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_MANAGEMENT_H_
#define ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_MANAGEMENT_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @cond INTERNAL_HIDDEN */

/*
 * Per-compatible specifier-data hooks. The clock-management framework routes
 * every "clocks = <&node cell>" entry to Z_CLOCK_MANAGEMENT_DATA_GET_<compat>,
 * where <compat> is the tokenized first compatible of the referenced node. The
 * returned scalar is handed back to the producer's configure() callback as its
 * "void *data" argument. None of these producers need a per-entry data
 * structure, so the DEFINE hooks are empty.
 */

/* Path mux: specifier cell is the zero-based input-sources index. */
#define Z_CLOCK_MANAGEMENT_DATA_DEFINE_infineon_cat1_path_mux(node_id, prop, idx)
#define Z_CLOCK_MANAGEMENT_DATA_GET_infineon_cat1_path_mux(node_id, prop, idx)      \
	DT_PHA_BY_IDX(node_id, prop, idx, mux)

/* clk_hf: specifier cell is the divider selector (divide factor = value + 1). */
#define Z_CLOCK_MANAGEMENT_DATA_DEFINE_infineon_cat1_clk_hf(node_id, prop, idx)
#define Z_CLOCK_MANAGEMENT_DATA_GET_infineon_cat1_clk_hf(node_id, prop, idx)        \
	DT_PHA_BY_IDX(node_id, prop, idx, divider)

/* peri-div: specifier cell is the 1-based integer divide value. */
#define Z_CLOCK_MANAGEMENT_DATA_DEFINE_infineon_cat1_peri_div(node_id, prop, idx)
#define Z_CLOCK_MANAGEMENT_DATA_GET_infineon_cat1_peri_div(node_id, prop, idx)      \
	DT_PHA_BY_IDX(node_id, prop, idx, divider)

/* DPLL: configured from its own devicetree divider fields; specifier unused. */
#define Z_CLOCK_MANAGEMENT_DATA_DEFINE_infineon_cat1_dpll(node_id, prop, idx)
#define Z_CLOCK_MANAGEMENT_DATA_GET_infineon_cat1_dpll(node_id, prop, idx)          \
	DT_PHA_BY_IDX(node_id, prop, idx, dummy)

/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_MANAGEMENT_H_ */
