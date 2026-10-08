cmake_minimum_required(VERSION 3.16)
project(displaylink-turbo VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization and architecture flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -pthread)
endif()

# Find EVDI headers
find_path(EVDI_INCLUDE_DIR evdi_lib.h PATHS /usr/include /usr/local/include)
if(NOT EVDI_INCLUDE_DIR)
    message(FATAL_ERROR "evdi development headers (evdi_lib.h) not found!")
endif()

# Find Vulkan library
find_library(VULKAN_LIBRARY NAMES vulkan PATHS /usr/lib /usr/lib64 /usr/local/lib)
if(NOT VULKAN_LIBRARY)
    find_package(Vulkan REQUIRED)
    set(VULKAN_LIBRARY Vulkan::Vulkan)
endif()

# Shader Compilation (tile_differencing.comp)
find_program(GLSLC_COMPILER glslc)
if(GLSLC_COMPILER)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
        COMMAND ${GLSLC_COMPILER} -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing.comp -o ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing.comp
        COMMENT "Compiling Compute Shader tile_differencing.comp with glslc"
    )
    add_custom_target(compile_shaders ALL
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
    )
endif()

message(STATUS "Found evdi headers: ${EVDI_INCLUDE_DIR}")
message(STATUS "Found vulkan: ${VULKAN_LIBRARY}")

# Vulkan EVDI Turbo Shim Library
set(SHIM_SOURCES
    src/shim/evdi_shim.cpp
    src/vulkan_converter.cpp
    src/tile_engine.cpp
)

add_library(evdi_turbo SHARED ${SHIM_SOURCES})
if(GLSLC_COMPILER)
    add_dependencies(evdi_turbo compile_shaders)
endif()

target_include_directories(evdi_turbo PRIVATE
    src
    include
    ${EVDI_INCLUDE_DIR}
)

target_link_libraries(evdi_turbo PRIVATE
    ${VULKAN_LIBRARY}
    dl
    pthread
)

set_target_properties(evdi_turbo PROPERTIES
    OUTPUT_NAME "evdi_turbo"
    PREFIX "lib"
)

# Symlinks for convenience
add_custom_command(TARGET evdi_turbo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE_NAME:evdi_turbo>
    $<TARGET_FILE_DIR:evdi_turbo>/libevdi.so.1
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE_NAME:evdi_turbo>
    $<TARGET_FILE_DIR:evdi_turbo>/libevdi.so
)

# Installation target
install(TARGETS evdi_turbo LIBRARY DESTINATION /usr/local/lib)
