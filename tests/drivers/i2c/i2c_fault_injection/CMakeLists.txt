# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(i2c_fault_injection)

target_sources(app PRIVATE
  src/bus_force.c
  src/i2c_timing.c
  src/main.c
  src/test_addr.c
  src/test_api_misuse.c
  src/test_arbitration.c
  src/test_data.c
  src/test_integrity.c
  src/test_liveness.c
  src/test_recovery.c
  src/test_stretch.c
  src/test_timing.c
)
