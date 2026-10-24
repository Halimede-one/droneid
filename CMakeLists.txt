cmake_minimum_required(VERSION 3.22)

set(SUPPORTED_TARGETS esp32c5)
set(PROJECT_VER "0.5.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(droneid)
