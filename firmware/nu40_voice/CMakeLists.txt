# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(military_wallaby_nu40)
target_include_directories(app PRIVATE ../../shared ../voice_common)
target_sources(app PRIVATE src/main.c src/font_0608.c src/voice.c src/ota.c src/alarm.c)
