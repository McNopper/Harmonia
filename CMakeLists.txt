cmake_minimum_required(VERSION 3.28)

project(harmonia VERSION 0.3.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(FetchContent)
# Include the shared CMake modules by absolute path so that, when Harmonia is
# pulled in via FetchContent by a consumer (Hyperion/Theia) that has its own
# identically-named modules earlier on CMAKE_MODULE_PATH, we still load ours
# (harmonia_configure_target lives only in Harmonia's Platform.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Platform.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VulkanSDK.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(VK_NO_PROTOTYPES)

# ── Aether (file-format library) ─────────────────────────────────────────────
# Consumed via FetchContent, pinned to a published release tag.  For local
# development against a working copy, override FETCHCONTENT_SOURCE_DIR_AETHER
# (e.g. -DFETCHCONTENT_SOURCE_DIR_AETHER=../Aether).
FetchContent_Declare(
    aether
    GIT_REPOSITORY https://github.com/McNopper/Aether.git
    GIT_TAG v0.3.2
)
FetchContent_MakeAvailable(aether)

set(HARMONIA_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(HARMONIA_ENTRY_SHADERS
    tonemap_vert.slang
    tonemap.slang
)

set(HARMONIA_LIBRARY_SOURCES
    src/harmonia/core/Buffer.cpp
    src/harmonia/core/CommandPool.cpp
    src/harmonia/core/Image.cpp
    src/harmonia/core/Logger.cpp
    src/harmonia/core/Queue.cpp
    src/harmonia/core/ShaderModule.cpp
    src/harmonia/core/Timer.cpp
    src/harmonia/renderer/AccelerationStructure.cpp
    src/harmonia/renderer/Camera.cpp
    src/harmonia/renderer/Descriptors.cpp
    src/harmonia/renderer/Pipeline.cpp
    src/harmonia/scene/Geometry.cpp
    src/harmonia/scene/IblProbe.cpp
    src/harmonia/scene/Light.cpp
    src/harmonia/scene/Mesh.cpp
    src/harmonia/scene/MaterialLibrary.cpp
    src/harmonia/scene/ObjImporter.cpp
    src/harmonia/scene/ProceduralGeometry.cpp
    src/harmonia/scene/SceneLoader.cpp
    src/harmonia/scene/Texture.cpp
    src/harmonia/utils/ColorSpace.cpp
    src/harmonia/presentation/ImageCapture.cpp
    src/harmonia/pipeline/SceneOutputCopyPass.cpp
    src/harmonia/presentation/Surface.cpp
    src/harmonia/presentation/Swapchain.cpp
    src/harmonia/presentation/ToneMapper.cpp
    src/harmonia/vulkan_init/Context.cpp
    src/harmonia/vulkan_init/PhysicalDevice.cpp
    src/harmonia/vulkan_init/DebugUtils.cpp
    src/harmonia/app/App.cpp
    src/harmonia/app/GreenScreenRenderer.cpp
    src/harmonia/vma_impl.cpp
)

# Volk — compiled from the Vulkan SDK source.
if(NOT TARGET volk)
    add_library(volk STATIC "${VULKANSDK_VOLK_SOURCE}")
    target_include_directories(volk SYSTEM PUBLIC "${VULKANSDK_INCLUDE_DIR}")
    set_target_properties(volk PROPERTIES LINKER_LANGUAGE C)
endif()

add_library(harmonia_project_options INTERFACE)
harmonia_configure_target(harmonia_project_options)

add_library(harmonia_vulkan_sdk INTERFACE)
target_include_directories(harmonia_vulkan_sdk SYSTEM INTERFACE "${VULKANSDK_INCLUDE_DIR}")
target_link_libraries(harmonia_vulkan_sdk INTERFACE ${VULKANSDK_LIBRARIES})

find_package(OpenImageIO REQUIRED)

# Resolve Harmonia's shaders relative to Harmonia's own source tree so the
# tonemap shaders compile correctly when Harmonia is consumed via FetchContent
# (CMAKE_SOURCE_DIR would otherwise point at the consumer project).
# Also expose the source dir so consumers (Hyperion, Theia) can add it to
# COMPILE_SLANG_EXTRA_INCLUDE_DIRS and import shared Slang modules directly.
set(HARMONIA_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    CACHE INTERNAL "Harmonia Slang shader source directory for consumer include paths")
set(COMPILE_SLANG_SHADER_ROOT "${HARMONIA_SHADER_SOURCE_DIR}")
compile_slang_shaders(harmonia_shaders "${HARMONIA_SHADER_OUTPUT_DIR}" ${HARMONIA_ENTRY_SHADERS})
unset(COMPILE_SLANG_SHADER_ROOT)

add_library(harmonia_lib STATIC ${HARMONIA_LIBRARY_SOURCES})
add_library(harmonia::harmonia ALIAS harmonia_lib)
add_dependencies(harmonia_lib harmonia_shaders)

target_include_directories(harmonia_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(harmonia_lib
    PUBLIC harmonia_project_options harmonia_vulkan_sdk volk aether::aether OpenImageIO::OpenImageIO
)

target_compile_definitions(harmonia_lib
    PUBLIC HARMONIA_SHADER_DIR=\"${HARMONIA_SHADER_OUTPUT_DIR}\"
)

# Tests only build when Harmonia is the top-level project.
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()
    if(TARGET gtest)
        set_property(TARGET gtest PROPERTY SYSTEM ON)
    endif()
    if(TARGET gtest_main)
        set_property(TARGET gtest_main PROPERTY SYSTEM ON)
    endif()
    if(NOT TARGET GTest::gtest AND TARGET gtest)
        add_library(GTest::gtest ALIAS gtest)
    endif()
    if(NOT TARGET GTest::gtest_main AND TARGET gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
    add_subdirectory(tests)
endif()
