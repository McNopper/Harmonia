include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests — pure CPU, no Vulkan, no GPU resources
# ---------------------------------------------------------------------------
add_executable(harmonia_unit_tests
    unit/test_colorspace.cpp
    unit/test_tonemap.cpp
    unit/test_math.cpp
    unit/test_procedural_geometry.cpp
)

target_link_libraries(harmonia_unit_tests
    PRIVATE harmonia_lib harmonia_project_options GTest::gtest GTest::gtest_main
)

gtest_discover_tests(harmonia_unit_tests)

# ---------------------------------------------------------------------------
# Component tests — single Vulkan object lifecycle (Buffer, Image, CommandPool)
#
# One Vulkan context is created in component/main.cpp and shared across all
# tests via the VulkanTestFixture infrastructure.  Tests GTEST_SKIP() when no
# Vulkan device is available.
# ---------------------------------------------------------------------------
add_executable(harmonia_component_tests
    component/main.cpp
    component/test_buffer.cpp
    component/test_image.cpp
    component/test_commandpool.cpp
)

target_link_libraries(harmonia_component_tests
    PRIVATE harmonia_lib harmonia_project_options GTest::gtest
)

target_include_directories(harmonia_component_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

gtest_discover_tests(harmonia_component_tests)

# ---------------------------------------------------------------------------
# Integration tests — full Context creation end-to-end
# ---------------------------------------------------------------------------
add_executable(harmonia_integration_tests
    integration/main.cpp
    integration/test_context.cpp
)

target_link_libraries(harmonia_integration_tests
    PRIVATE harmonia_lib harmonia_project_options GTest::gtest
)

target_include_directories(harmonia_integration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

gtest_discover_tests(harmonia_integration_tests)
