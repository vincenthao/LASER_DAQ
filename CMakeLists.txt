# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(LASER_DAQ)

target_sources(app PRIVATE src/main.c src/Proto/can_proto.c src/Proto/can_sniffer.c src/Proto/can_collect.c)
