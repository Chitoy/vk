cmake_minimum_required(VERSION 3.24)
project(vulkan_shader_starter VERSION 0.1.0 LANGUAGES CXX)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

if(TARGET glfw)
    set(GLFW_TARGET glfw)
elseif(TARGET glfw3::glfw)
    set(GLFW_TARGET glfw3::glfw)
else()
    message(FATAL_ERROR "A GLFW CMake target was not found")
endif()

if(Vulkan_GLSLC_EXECUTABLE)
    set(GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(
        GLSLC_EXECUTABLE
        NAMES glslc glslc.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        REQUIRED
    )
endif()

add_executable(vulkan_shader_starter src/main.cpp)
target_compile_features(vulkan_shader_starter PRIVATE cxx_std_20)
target_link_libraries(vulkan_shader_starter PRIVATE Vulkan::Vulkan ${GLFW_TARGET})
target_compile_definitions(
    vulkan_shader_starter
    PRIVATE VULKAN_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
)

if(MSVC)
    target_compile_options(vulkan_shader_starter PRIVATE /W4 /permissive-)
else()
    target_compile_options(vulkan_shader_starter PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag"
)
set(SPIRV_FILES)

foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
    set(SPIRV_FILE "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT "${SPIRV_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.1 "${SHADER_SOURCE}" -o "${SPIRV_FILE}"
        DEPENDS "${SHADER_SOURCE}"
        COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
        VERBATIM
    )
    list(APPEND SPIRV_FILES "${SPIRV_FILE}")
endforeach()

add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_FILES})
add_dependencies(vulkan_shader_starter compile_shaders)

