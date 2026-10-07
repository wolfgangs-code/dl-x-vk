cmake_minimum_required(VERSION 3.16)
project(dl-x-vk VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization and architecture flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -pthread)
endif()

# Find libusb-1.0
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# Find evdi
find_path(EVDI_INCLUDE_DIR evdi_lib.h PATHS /usr/include /usr/local/include)
find_library(EVDI_LIBRARY NAMES evdi PATHS /usr/lib /usr/lib64 /usr/local/lib)

if(NOT EVDI_INCLUDE_DIR OR NOT EVDI_LIBRARY)
    message(FATAL_ERROR "evdi development headers or library (libevdi.so) not found!")
endif()

# Find Vulkan library
find_library(VULKAN_LIBRARY NAMES vulkan PATHS /usr/lib /usr/lib64 /usr/local/lib)
if(NOT VULKAN_LIBRARY)
    find_package(Vulkan REQUIRED)
    set(VULKAN_LIBRARY Vulkan::Vulkan)
endif()

# Find glslc compiler for shader compilation
find_program(GLSLC_COMPILER glslc)
if(GLSLC_COMPILER)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rgb_to_yuv420_spv.inc
        COMMAND ${GLSLC_COMPILER} -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rgb_to_yuv420.comp -o ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rgb_to_yuv420_spv.inc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rgb_to_yuv420.comp
        COMMENT "Compiling Compute Shader rgb_to_yuv420.comp with glslc"
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
        COMMAND ${GLSLC_COMPILER} -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing.comp -o ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing.comp
        COMMENT "Compiling Compute Shader tile_differencing.comp with glslc"
    )
    add_custom_target(compile_shaders ALL
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rgb_to_yuv420_spv.inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/tile_differencing_spv.inc
    )
endif()

message(STATUS "Found libusb: ${LIBUSB_INCLUDE_DIRS} ${LIBUSB_LIBRARIES}")
message(STATUS "Found evdi: ${EVDI_INCLUDE_DIR} ${EVDI_LIBRARY}")
message(STATUS "Found vulkan: ${VULKAN_LIBRARY}")

# Sources
set(SOURCES
    src/main.cpp
    src/color_converter.cpp
    src/vulkan_converter.cpp
    src/tile_engine.cpp
    src/codec_dl3.cpp
    src/usb_transport.cpp
    src/evdi_device.cpp
    src/service.cpp
)

add_executable(dl-x-vk ${SOURCES})
if(GLSLC_COMPILER)
    add_dependencies(dl-x-vk compile_shaders)
endif()

target_include_directories(dl-x-vk PRIVATE
    src
    include
    ${LIBUSB_INCLUDE_DIRS}
    ${EVDI_INCLUDE_DIR}
)

target_link_libraries(dl-x-vk PRIVATE
    ${LIBUSB_LIBRARIES}
    ${EVDI_LIBRARY}
    ${VULKAN_LIBRARY}
    pthread
)

# Also create displaylink-turbo symlink/target alias
add_custom_command(TARGET dl-x-vk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE_NAME:dl-x-vk>
    $<TARGET_FILE_DIR:dl-x-vk>/displaylink-turbo
)

# Installation target
install(TARGETS dl-x-vk RUNTIME DESTINATION bin)
