cmake_minimum_required(VERSION 3.22)
project(KobaltWindows LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Windows desktop build must be configured on Windows.")
endif()

set(BUILD_TESTING OFF CACHE BOOL "Disable test targets for the initial Windows bring-up" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../desktop" "${CMAKE_BINARY_DIR}/desktop")
