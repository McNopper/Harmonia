cmake_minimum_required(VERSION 3.28)

project(harmonia VERSION 0.7.5 LANGUAGES C CXX)

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

# ============================================================
# AI-first static analysis (clang-tidy / cppcheck / clang-format)
#
# Canonical setup shared by every repo in the pipeline, ported from the
# Hephaestus cpp/ template. Analysis runs only when the generator emits a
# compile database (Ninja/Makefiles) and the compiler is Clang/GNU-
# compatible; otherwise it is skipped and the project configures/builds
# normally. The analysis *targets* are opt-in; in-build analysis is OFF by
# default so the build is never blocked by findings.
#
# Guarded on PROJECT_IS_TOP_LEVEL so the targets are only defined when this
# project is built standalone: several of these repos are consumed via
# FetchContent by downstream pipeline repos, where redefining the targets
# would clash with the consumer's own.
# ============================================================
if(PROJECT_IS_TOP_LEVEL)
    set(PROJECT_REPORTS_DIR ${CMAKE_BINARY_DIR}/reports)
    file(MAKE_DIRECTORY ${PROJECT_REPORTS_DIR})

    set(PROJECT_ANALYSIS_AVAILABLE FALSE)
    if(CMAKE_EXPORT_COMPILE_COMMANDS
       AND (CMAKE_GENERATOR MATCHES "Ninja" OR CMAKE_GENERATOR MATCHES "Makefiles")
       AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU"))
        set(PROJECT_ANALYSIS_AVAILABLE TRUE)
        message(STATUS "AI-first static analysis enabled (${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_ID})")
    else()
        message(STATUS "AI-first static analysis skipped for this toolchain "
            "(${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_ID}); "
            "use Ninja/Makefiles with a Clang- or GNU-compatible compiler to enable it")
    endif()

    file(GENERATE
        OUTPUT ${PROJECT_REPORTS_DIR}/analysis-status.txt
        CONTENT "analysis=$<BOOL:${PROJECT_ANALYSIS_AVAILABLE}>\ngenerator=${CMAKE_GENERATOR}\ncompiler=${CMAKE_CXX_COMPILER_ID}\n"
    )

    option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" ON)
    option(ENABLE_CLANG_TIDY_IN_BUILD "Run clang-tidy during compilation" OFF)
    option(ENABLE_CLANG_FORMAT "Enable clang-format targets" ON)
    option(ENABLE_CPPCHECK "Enable cppcheck static analysis target" ON)
    option(ENABLE_CPPCHECK_IN_BUILD "Run cppcheck alongside the compiler" OFF)

    # All first-party C/C++ sources (clang-format); compiled translation units
    # only (clang-tidy) — headers are analysed transitively via HeaderFilterRegex.
    file(GLOB_RECURSE PROJECT_CXX_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hxx"
    )
    file(GLOB_RECURSE PROJECT_CXX_TUS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
    )

    # Drop single-TU third-party implementation wrappers (stb, VMA) from the
    # clang-tidy set: they hold no first-party logic (just a #define + the
    # vendor include) and clang-tidy cannot always resolve the vendored path.
    list(REMOVE_ITEM PROJECT_CXX_TUS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/harmonia/stb_image_impl.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/harmonia/vma_impl.cpp"
    )

    # ----- clang-tidy -----
    if(ENABLE_CLANG_TIDY AND PROJECT_ANALYSIS_AVAILABLE)
        find_program(CLANG_TIDY_EXE NAMES clang-tidy)
        if(CLANG_TIDY_EXE)
            if(ENABLE_CLANG_TIDY_IN_BUILD)
                set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
                message(STATUS "clang-tidy enabled during build: ${CLANG_TIDY_EXE}")
            else()
                message(STATUS "clang-tidy target enabled: ${CLANG_TIDY_EXE}")
            endif()

            set(CLANG_TIDY_REPORT_DIR ${PROJECT_REPORTS_DIR}/clang-tidy)
            file(MAKE_DIRECTORY ${CLANG_TIDY_REPORT_DIR})

            add_custom_target(tidy
                COMMAND ${CLANG_TIDY_EXE}
                    -p ${CMAKE_BINARY_DIR}
                    --export-fixes=${CLANG_TIDY_REPORT_DIR}/fixes.yaml
                    ${PROJECT_CXX_TUS}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Running clang-tidy; fixes -> ${CLANG_TIDY_REPORT_DIR}/fixes.yaml"
                VERBATIM
            )
        else()
            message(STATUS "clang-tidy not found")
        endif()
    endif()

    # ----- cppcheck -----
    if(ENABLE_CPPCHECK AND PROJECT_ANALYSIS_AVAILABLE)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
        include(cppcheck)
    endif()

    # ----- clang-format -----
    if(ENABLE_CLANG_FORMAT)
        find_program(CLANG_FORMAT_EXE NAMES clang-format)
        if(CLANG_FORMAT_EXE)
            add_custom_target(format
                COMMAND ${CLANG_FORMAT_EXE}
                    -i
                    --style=file
                    ${PROJECT_CXX_SOURCES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Formatting source files with clang-format"
                VERBATIM
            )

            add_custom_target(format-check
                COMMAND ${CLANG_FORMAT_EXE}
                    --dry-run
                    --Werror
                    --style=file
                    ${PROJECT_CXX_SOURCES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Checking source formatting with clang-format"
                VERBATIM
            )

            message(STATUS "clang-format enabled: ${CLANG_FORMAT_EXE}")
        else()
            message(STATUS "clang-format not found")
        endif()
    endif()

    # ----- Aggregate verification targets -----
    # verify:      build + test (fast default).
    # verify-full: verify + format-check + tidy + cppcheck (opt-in strict).
    add_custom_target(verify
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --build-config $<CONFIG>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Fast verification: build + test"
        VERBATIM
    )

    add_custom_target(verify-full
        COMMENT "Full verification: verify + format-check + tidy + cppcheck"
    )
    add_dependencies(verify-full verify)
    if(TARGET format-check)
        add_dependencies(verify-full format-check)
    endif()
    if(TARGET cppcheck)
        add_dependencies(verify-full cppcheck)
    endif()
    if(TARGET tidy)
        add_dependencies(verify-full tidy)
    endif()
endif()

# ── Aether (file-format library) ─────────────────────────────────────────────
# Consumed via FetchContent, pinned to a published release tag.  For local
# development against a working copy, override FETCHCONTENT_SOURCE_DIR_AETHER
# (e.g. -DFETCHCONTENT_SOURCE_DIR_AETHER=../Aether).
FetchContent_Declare(
    aether
    GIT_REPOSITORY https://github.com/McNopper/Aether.git
    GIT_TAG v0.7.2
)
FetchContent_MakeAvailable(aether)

FetchContent_Declare(
    slang_math
    GIT_REPOSITORY https://github.com/McNopper/slang-math.git
    GIT_TAG v0.2.1
)
FetchContent_MakeAvailable(slang_math)

set(HARMONIA_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(HARMONIA_ENTRY_SHADERS
    accumulation.slang
    denoiser.slang
    tonemap_vert.slang
    tonemap.slang
)

set(HARMONIA_LIBRARY_SOURCES
    src/harmonia/core/Buffer.cpp
    src/harmonia/core/CommandPool.cpp
    src/harmonia/core/Image.cpp
    src/harmonia/core/Logger.cpp
    src/harmonia/core/ShaderModule.cpp
    src/harmonia/renderer/AccelerationStructure.cpp
    src/harmonia/renderer/TlasBuilder.cpp
    src/harmonia/renderer/Camera.cpp
    src/harmonia/renderer/Descriptors.cpp
    src/harmonia/renderer/Pipeline.cpp
    src/harmonia/scene/Geometry.cpp
    src/harmonia/scene/EmissiveBuilder.cpp
    src/harmonia/scene/IblProbe.cpp
    src/harmonia/scene/Light.cpp
    src/harmonia/scene/Mesh.cpp
    src/harmonia/scene/MaterialLibrary.cpp
    src/harmonia/scene/ProceduralGeometry.cpp
    src/harmonia/scene/SceneBase.cpp
    src/harmonia/scene/SceneLoader.cpp
    src/harmonia/scene/Texture.cpp
    src/harmonia/utils/ColorSpace.cpp
    src/harmonia/presentation/ImageCapture.cpp
    src/harmonia/pipeline/AccumulationPass.cpp
    src/harmonia/pipeline/SceneOutputCopyPass.cpp
    src/harmonia/presentation/Swapchain.cpp
    src/harmonia/presentation/ToneMapper.cpp
    src/harmonia/vulkan_init/Context.cpp
    src/harmonia/vulkan_init/PhysicalDevice.cpp
    src/harmonia/vulkan_init/DebugUtils.cpp
    src/harmonia/app/App.cpp
    src/harmonia/app/CliParser.cpp
    src/harmonia/app/FrameSync.cpp
    src/harmonia/app/OffscreenCapture.cpp
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
find_package(tomlplusplus CONFIG REQUIRED)

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
    PUBLIC harmonia_project_options harmonia_vulkan_sdk volk aether::aether slang-math::slang-math OpenImageIO::OpenImageIO
    PRIVATE tomlplusplus::tomlplusplus
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
        # gtest v1.15.2 has an implicit char8_t→char32_t conversion in C++23; suppress.
        foreach(_gt_tgt IN ITEMS gtest gtest_main gmock gmock_main)
            if(TARGET ${_gt_tgt})
                target_compile_options(${_gt_tgt} PRIVATE
                    $<$<CXX_COMPILER_ID:Clang,MSVC>:-Wno-character-conversion>
                )
            endif()
        endforeach()
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

