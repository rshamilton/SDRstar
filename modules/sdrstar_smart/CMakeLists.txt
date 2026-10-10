cmake_minimum_required(VERSION 3.13)
project(sdrstar_smart)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

# The pure-C++ SDRstar core (analysis, planner, decoder supervision, trunk follower) is built from the SDRstar repo.
if (NOT SDRSTAR_ROOT)
    message(FATAL_ERROR "SDRSTAR_ROOT is not set (scripts/prepare_sdrpp.sh writes sdrstar_paths.cmake)")
endif ()
set(SDRSTAR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDRSTAR_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/sdrstar_core)

target_include_directories(sdrstar_smart PRIVATE "src/" "../../decoder_modules/radio/src")
target_link_libraries(sdrstar_smart PRIVATE sdrstar_core)
