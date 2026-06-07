cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zenoh_pubsub)
