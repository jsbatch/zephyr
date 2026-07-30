# SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
# or an affiliate of Infineon Technologies AG. All rights reserved.
#
# SPDX-License-Identifier: Apache-2.0

# Two interchangeable producer backends implement the same devicetree
# compatibles; exactly one is compiled so there is no duplicate clock object.
if(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_BACKEND_REG)
  # Register-direct backend (family-agnostic, addresses from devicetree)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_CLOCK_SOURCE
    ifx_cat1_clock_source_reg.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_PATH_MUX
    ifx_cat1_path_mux_reg.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_CLK_HF
    ifx_cat1_clk_hf_reg.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_PERI_DIV
    ifx_cat1_peri_div_reg.c)
else()
  # PDL runtime backend
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_CLOCK_SOURCE
    ifx_cat1_clock_source.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_PATH_MUX
    ifx_cat1_path_mux.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_CLK_HF
    ifx_cat1_clk_hf.c)
  zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_PERI_DIV
    ifx_cat1_peri_div.c)
endif()

# The DPLL programming sequence is a validated vendor procedure retained for
# both backends, so this producer is compiled regardless of backend selection.
zephyr_library_sources_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_DPLL
  ifx_cat1_dpll.c)

# Specifier-data hooks for the clock-management framework (shared by both backends)
add_clock_management_header_ifdef(CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1
  ifx_cat1_clock_management.h)
