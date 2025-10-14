cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pdm_capture_min)
target_sources(app PRIVATE src/main.c)
