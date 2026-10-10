# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nu40_ble_display)
target_include_directories(app PRIVATE ../shared)
target_sources(app PRIVATE src/main.c src/font_0608.c src/kit_io.c src/kit_mgmt.c)
