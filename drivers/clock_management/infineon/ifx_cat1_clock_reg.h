/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Shared helpers for the register-direct Infineon CAT1 clock-management
 *        producers.
 *
 * These producers are family-agnostic: the register address, bitfield offset,
 * and bitfield width all come from devicetree, so the same driver code works
 * across CAT1 SoC families (the per-SoC devicetree, ideally generated, carries
 * the register map). No PDL runtime functions and no vendor register-definition
 * headers are used here.
 */

#ifndef ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_REG_H_
#define ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_REG_H_

#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/**
 * @brief Read-modify-write a bitfield within a 32-bit register.
 *
 * @param addr  Absolute register address.
 * @param lsb   Least-significant bit of the field.
 * @param width Field width in bits (1..32).
 * @param val   Value to place in the field.
 */
static inline void ifx_cat1_reg_field_set(mem_addr_t addr, uint8_t lsb, uint8_t width,
					  uint32_t val)
{
	uint32_t mask = ((width >= 32U) ? 0xFFFFFFFFU : (BIT(width) - 1U)) << lsb;

	sys_write32((sys_read32(addr) & ~mask) | ((val << lsb) & mask), addr);
} /* ifx_cat1_reg_field_set() */

/**
 * @brief Read a bitfield within a 32-bit register.
 *
 * @param addr  Absolute register address.
 * @param lsb   Least-significant bit of the field.
 * @param width Field width in bits (1..32).
 *
 * @return Field value, right-aligned.
 */
static inline uint32_t ifx_cat1_reg_field_get(mem_addr_t addr, uint8_t lsb, uint8_t width)
{
	uint32_t mask = (width >= 32U) ? 0xFFFFFFFFU : (BIT(width) - 1U);

	return (sys_read32(addr) >> lsb) & mask;
} /* ifx_cat1_reg_field_get() */

#endif /* ZEPHYR_DRIVERS_CLOCK_MANAGEMENT_INFINEON_IFX_CAT1_CLOCK_REG_H_ */
