cmake_minimum_required(VERSION 3.24)
project(vulkan_compute_buffer LANGUAGES CXX)

# 独立项目：只查询另一台电脑上已有的 SDK，不下载任何依赖。
find_package(Vulkan REQUIRED)
if(Vulkan_GLSLC_EXECUTABLE)
    set(COMPUTE_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(COMPUTE_GLSLC NAMES glslc glslc.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
endif()

set(COMPUTE_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders/transform.comp.spv")
add_custom_command(OUTPUT "${COMPUTE_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${COMPUTE_GLSLC}" --target-env=vulkan1.1
        "${CMAKE_CURRENT_SOURCE_DIR}/transform.comp" -o "${COMPUTE_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/transform.comp"
    COMMENT "Compiling transform.comp for Vulkan 1.1" VERBATIM)
add_custom_target(compute_buffer_shader DEPENDS "${COMPUTE_SPV}")

add_executable(compute_buffer main.cpp)
add_dependencies(compute_buffer compute_buffer_shader)
target_compile_features(compute_buffer PRIVATE cxx_std_20)
set_target_properties(compute_buffer PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(compute_buffer PRIVATE COMPUTE_SHADER_PATH="${COMPUTE_SPV}")
target_link_libraries(compute_buffer PRIVATE Vulkan::Vulkan)
if(MSVC)
    target_compile_options(compute_buffer PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(compute_buffer PRIVATE -Wall -Wextra -Wpedantic)
endif()
