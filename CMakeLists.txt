# UCSynth — полифонический wavetable-синтезатор на ESP32-S3.
# Нативный ESP-IDF (не Arduino, не PlatformIO). Таргет фиксируется в sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ucsynth)
