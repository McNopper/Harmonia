include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests — pure CPU, no Vulkan, no GPU resources
# ---------------------------------------------------------------------------
add_executable(harmonia_unit_tests
    unit/test_colorspace.cpp
    unit/test_colorspace_seams.cpp
    unit/test_app_args.cpp
    unit/test_tonemap.cpp
    unit/test_math.cpp
    unit/test_rng.cpp
    unit/test_env_sampling_contract.cpp
    unit/test_procedural_geometry.cpp
    unit/test_emissive.cpp
    unit/test_device_fault.cpp
)

target_link_libraries(harmonia_unit_tests
    PRIVATE harmonia::harmonia harmonia::low_level harmonia_project_options GTest::gtest GTest::gtest_main
)

gtest_discover_tests(harmonia_unit_tests PROPERTIES RUN_SERIAL TRUE)

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
    component/test_accumulation_pass.cpp
    component/test_scene_output_copy_pass.cpp
    component/test_commandpool.cpp
    component/test_sms.cpp
    component/test_frame_completion.cpp
)

target_link_libraries(harmonia_component_tests
    PRIVATE harmonia::harmonia harmonia::low_level harmonia_project_options GTest::gtest
)

target_include_directories(harmonia_component_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(COMPILE_SLANG_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(COMPILE_SLANG_EXTRA_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/shaders")
set(COMPILE_SLANG_EXTRA_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/sms.slang"
    "${PROJECT_SOURCE_DIR}/shaders/sms_geometry.slang" "${PROJECT_SOURCE_DIR}/shaders/sms_solver.slang")
compile_slang_shaders(harmonia_test_shaders "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    sms_test.slang sms_reciprocal_test.slang sms_trial_test.slang sms_geometry_test.slang
    sms_projected_test.slang sms_root_test.slang sms_measure_test.slang)
unset(COMPILE_SLANG_EXTRA_DEPENDS)
unset(COMPILE_SLANG_SHADER_ROOT)
unset(COMPILE_SLANG_EXTRA_INCLUDE_DIRS)
add_dependencies(harmonia_component_tests harmonia_test_shaders)
target_compile_definitions(harmonia_component_tests PRIVATE
    HARMONIA_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
)

gtest_discover_tests(harmonia_component_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# Integration tests — full Context creation end-to-end
# ---------------------------------------------------------------------------
add_executable(harmonia_integration_tests
    integration/main.cpp
    integration/test_context.cpp
    integration/test_scene_loader.cpp
    integration/test_ibl_probe_encoding.cpp
    integration/test_frame_completion_capture.cpp
)

target_link_libraries(harmonia_integration_tests
    PRIVATE harmonia::harmonia harmonia::low_level harmonia_project_options GTest::gtest
)

target_include_directories(harmonia_integration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(harmonia_integration_tests
    PRIVATE AETHER_ASSETS_DIR="${aether_SOURCE_DIR}/assets"
)

gtest_discover_tests(harmonia_integration_tests PROPERTIES RUN_SERIAL TRUE)
