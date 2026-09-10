cmake_minimum_required(VERSION 3.16)
project(glss VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# Vulkan is loaded at runtime (dlopen / LoadLibrary), never linked.
# Headers are vendored under third_party/vulkan (Khronos Vulkan-Headers).
set(GLSS_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan
)

if(MSVC)
    set(GLSS_WARNINGS /W4)
else()
    set(GLSS_WARNINGS -Wall -Wextra)
endif()

add_library(glss_core STATIC
    src/vulkan_context.cpp
    src/vulkan_compute.cpp
    src/frame_interpolator.cpp
    src/super_resolution.cpp
    src/window_capture.cpp
    src/glss_pipeline.cpp
)
target_include_directories(glss_core PUBLIC ${GLSS_INCLUDE_DIRS})
target_compile_options(glss_core PRIVATE ${GLSS_WARNINGS})
target_link_libraries(glss_core PUBLIC Threads::Threads)
if(UNIX)
    target_link_libraries(glss_core PUBLIC ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    # DXGI Desktop Duplication / D3D11 are system libraries on Windows.
    target_link_libraries(glss_core PUBLIC d3d11 dxgi)
endif()

add_executable(glss src/main.cpp)
target_link_libraries(glss PRIVATE glss_core)
target_compile_options(glss PRIVATE ${GLSS_WARNINGS})

option(GLSS_BUILD_TESTS "Build unit tests" ON)
if(GLSS_BUILD_TESTS)
    enable_testing()
    add_executable(glss_tests tests/test_core.cpp)
    target_link_libraries(glss_tests PRIVATE glss_core)
    target_compile_options(glss_tests PRIVATE ${GLSS_WARNINGS})
    add_test(NAME glss_core_tests COMMAND glss_tests)
endif()
