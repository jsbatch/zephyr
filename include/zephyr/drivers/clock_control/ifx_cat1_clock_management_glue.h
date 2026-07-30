/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_IFX_CAT1_CLOCK_MANAGEMENT_GLUE_H_
#define ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_IFX_CAT1_CLOCK_MANAGEMENT_GLUE_H_

/**
 * @file
 * @brief Source-clock rate accessor bridging clock management and the PDL.
 *
 * An Infineon peripheral owns its leaf (baud/bit-time) divider but needs the
 * rate of the shared source clock (clk_hf) that feeds it. This helper is the
 * single place that decides where that rate comes from: the clock-management
 * framework for a peripheral that has been converted to it, or the PDL for one
 * that has not. Keeping the selection here lets a driver move to clock
 * management on its own, one at a time, without each rate read carrying the
 * conditional.
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/drivers/clock_control/clock_control_ifx_cat1.h>

#if defined(CONFIG_CLOCK_MANAGEMENT)
#include <zephyr/drivers/clock_management.h>
#else
struct clock_output;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve the source-clock rate feeding a peripheral's leaf divider.
 *
 * @param clk        Clock-management output for the peripheral's source clock,
 *                   or NULL when the caller has not been converted to clock
 *                   management (the rate is then read through the PDL).
 * @param peri_group Peripheral divider group, used only on the PDL path to
 *                   locate the source clk_hf; ignored when @p clk is non-NULL.
 * @return Source-clock rate in Hz, or a negative errno on failure.
 */
static inline int ifx_cat1_periph_source_rate(const struct clock_output *clk, uint8_t peri_group)
{
#if defined(CONFIG_CLOCK_MANAGEMENT)
	if (clk != NULL) {
		return clock_management_get_rate(clk);
	}
#else
	ARG_UNUSED(clk);
#endif

#if defined(COMPONENT_CAT1A)
	ARG_UNUSED(peri_group);
	return (int)Cy_SysClk_ClkPeriGetFrequency();
#elif defined(COMPONENT_CAT1B) || defined(COMPONENT_CAT1C) ||                                       \
	defined(CONFIG_SOC_FAMILY_INFINEON_EDGE)
	return (int)Cy_SysClk_ClkHfGetFrequency(ifx_cat1_utils_peri_pclk_get_hfclk(peri_group));
#else
	ARG_UNUSED(peri_group);
	return (int)Cy_SysClk_ClkHfGetFrequency();
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_IFX_CAT1_CLOCK_MANAGEMENT_GLUE_H_ */
