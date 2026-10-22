cmake_minimum_required(VERSION 3.28)

project(harmonia VERSION 0.7.15 LANGUAGES C CXX)

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
# VK_EXT_opacity_micromap types/functions are still behind VK_ENABLE_BETA_EXTENSIONS
# in the SDK 1.4 headers (the extension ships widely but is not yet "stable" in the
# header sense). Define it project-wide so the OMM structs (VkMicromapEXT,
# VkMicromapBuildInfoEXT, VkAccelerationStructureTrianglesOpacityMicromapEXT, …)
# and the volk function pointers (vkCreateMicromapEXT, vkCmdBuildMicromapsEXT, …)
# are visible.
add_compile_definitions(VK_ENABLE_BETA_EXTENSIONS)

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

    # Sanitizer lane (Clang/GCC only):
    #   cmake -DHARMONIA_SANITIZER=address|undefined|thread ...
    set(HARMONIA_SANITIZER "" CACHE STRING "Sanitizer: address, undefined, or thread (Clang/GCC only)")
    if(HARMONIA_SANITIZER)
        if(MSVC)
            message(FATAL_ERROR "HARMONIA_SANITIZER requires Clang/GCC (use a Ninja+clang or WSL configure)")
        endif()
        add_compile_options("-fsanitize=${HARMONIA_SANITIZER}" -fno-omit-frame-pointer)
        add_link_options("-fsanitize=${HARMONIA_SANITIZER}")
    endif()
    # Coverage lane (Clang/GCC only):
    #   cmake -DHARMONIA_COVERAGE=ON -B build/coverage ...   then: python tools/coverage.py
    set(HARMONIA_COVERAGE OFF CACHE BOOL "Instrument for source coverage (Clang/GCC only)")
    if(HARMONIA_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            message(FATAL_ERROR "HARMONIA_COVERAGE requires Clang/GCC (use a Ninja+clang or WSL configure)")
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
            if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
                # clang-cl + MSVC linker: the profile runtime only links when
                # lld-link drives the link step
                add_link_options(-fprofile-instr-generate -fuse-ld=lld)
            else()
                add_link_options(-fprofile-instr-generate)
            endif()
        else()
            add_compile_options(--coverage)
            add_link_options(--coverage)
        endif()
    endif()


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
    GIT_TAG v0.7.8
)
FetchContent_MakeAvailable(aether)

FetchContent_Declare(
    slang_math
    GIT_REPOSITORY https://github.com/McNopper/slang-math.git
    GIT_TAG v0.4.0
)
FetchContent_MakeAvailable(slang_math)

set(HARMONIA_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(HARMONIA_ENTRY_SHADERS
    accumulation.slang
    denoiser.slang
    tonemap_vert.slang
    tonemap.slang
)

# Layered targets (AR2): the include-DAG of ARCHITECTURE.md expressed as
# target_link_libraries - an upward include is now a COMPILE error, a
# cross-layer link a LINK error. Bottom-up; harmonia::harmonia stays the
# consumer-facing alias (now the app-layer target).
set(HARMONIA_vocab_SOURCES
    src/harmonia/vma_impl.cpp
)
set(HARMONIA_utils_SOURCES
    src/harmonia/utils/ColorSpace.cpp
)
set(HARMONIA_core_SOURCES
    src/harmonia/core/Buffer.cpp
    src/harmonia/core/CommandPool.cpp
    src/harmonia/core/DescriptorBufferWriter.cpp
    src/harmonia/core/HostImageUpload.cpp
    src/harmonia/core/Image.cpp
    src/harmonia/core/Logger.cpp
    src/harmonia/core/ShaderModule.cpp
)
set(HARMONIA_low_level_SOURCES
    src/harmonia/vulkan_init/Context.cpp
    src/harmonia/vulkan_init/PhysicalDevice.cpp
    src/harmonia/vulkan_init/DebugUtils.cpp
)
set(HARMONIA_rt_SOURCES
    src/harmonia/rt/AccelerationStructure.cpp
    src/harmonia/rt/MicromapBuilder.cpp
    src/harmonia/rt/TlasBuilder.cpp
)
set(HARMONIA_scene_SOURCES
    src/harmonia/scene/Geometry.cpp
    src/harmonia/scene/IblProbe.cpp
    src/harmonia/scene/Light.cpp
    src/harmonia/scene/Mesh.cpp
    src/harmonia/scene/MaterialLibrary.cpp
    src/harmonia/scene/ProceduralGeometry.cpp
    src/harmonia/scene/SceneBase.cpp
    src/harmonia/scene/SceneLoader.cpp
    src/harmonia/scene/Texture.cpp
)
set(HARMONIA_scene_rt_SOURCES
    src/harmonia/scene_rt/TriangleMesh.hpp
    src/harmonia/scene_rt/Sphere.hpp
    src/harmonia/scene_rt/EmissiveBuilder.hpp
    src/harmonia/scene_rt/SceneRt.hpp
    src/harmonia/scene_rt/GeometryRt.cpp
    src/harmonia/scene_rt/SceneRt.cpp
    src/harmonia/scene_rt/EmissiveBuilder.cpp
)
set(HARMONIA_passes_SOURCES
    src/harmonia/renderer/Camera.cpp
    src/harmonia/renderer/Descriptors.cpp
    src/harmonia/renderer/Pipeline.cpp
    src/harmonia/pipeline/AccumulationPass.cpp
    src/harmonia/pipeline/SceneOutputCopyPass.cpp
    src/harmonia/pipeline/SceneOutputCopyPassDenoise.cpp
    src/harmonia/presentation/ImageCapture.cpp
    src/harmonia/presentation/Swapchain.cpp
    src/harmonia/presentation/ToneMapper.cpp
)
set(HARMONIA_app_SOURCES
    src/harmonia/app/App.cpp
    src/harmonia/app/AppOffscreen.cpp
    src/harmonia/app/AppPresent.cpp
    src/harmonia/app/CliParser.cpp
    src/harmonia/app/FrameSync.cpp
    src/harmonia/app/OffscreenCapture.cpp
    src/harmonia/app/GreenScreenRenderer.cpp
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

add_library(harmonia_vocab STATIC ${HARMONIA_vocab_SOURCES})
add_library(harmonia::vocab ALIAS harmonia_vocab)
add_library(harmonia_utils STATIC ${HARMONIA_utils_SOURCES})
add_library(harmonia::utils ALIAS harmonia_utils)
target_link_libraries(harmonia_utils PUBLIC harmonia::vocab slang-math)
add_library(harmonia_core STATIC ${HARMONIA_core_SOURCES})
add_library(harmonia::core ALIAS harmonia_core)
target_link_libraries(harmonia_core PUBLIC harmonia::utils volk)
add_library(harmonia_low_level STATIC ${HARMONIA_low_level_SOURCES})
add_library(harmonia::low_level ALIAS harmonia_low_level)
target_link_libraries(harmonia_low_level PUBLIC harmonia::core)
add_library(harmonia_rt STATIC ${HARMONIA_rt_SOURCES})
add_library(harmonia::rt ALIAS harmonia_rt)
target_link_libraries(harmonia_rt PUBLIC harmonia::core aether)
add_library(harmonia_scene STATIC ${HARMONIA_scene_SOURCES})
add_library(harmonia::scene ALIAS harmonia_scene)
target_link_libraries(harmonia_scene PUBLIC harmonia::rt aether
    PRIVATE OpenImageIO::OpenImageIO tomlplusplus::tomlplusplus)
add_library(harmonia_scene_rt STATIC ${HARMONIA_scene_rt_SOURCES})
add_library(harmonia::scene_rt ALIAS harmonia_scene_rt)
target_link_libraries(harmonia_scene_rt PUBLIC harmonia::scene harmonia::rt)
add_library(harmonia_passes STATIC ${HARMONIA_passes_SOURCES})
add_library(harmonia::passes ALIAS harmonia_passes)
target_link_libraries(harmonia_passes PUBLIC harmonia::scene harmonia::core OpenImageIO::OpenImageIO)
add_library(harmonia_app STATIC ${HARMONIA_app_SOURCES})
add_library(harmonia::app_layer ALIAS harmonia_app)
target_link_libraries(harmonia_app PUBLIC harmonia::passes harmonia::scene_rt)
add_dependencies(harmonia_app harmonia_shaders)
add_library(harmonia::harmonia ALIAS harmonia_app)

target_include_directories(harmonia_vocab
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(harmonia_vocab
    PUBLIC harmonia_project_options harmonia_vulkan_sdk volk
)
target_link_libraries(harmonia_utils
    PUBLIC slang-math::slang-math
)
target_link_libraries(harmonia_core
    PUBLIC
)
target_link_libraries(harmonia_scene
    PUBLIC aether::aether
)
target_link_libraries(harmonia_scene_rt
    PUBLIC tomlplusplus::tomlplusplus
)
target_link_libraries(harmonia_app
    PUBLIC harmonia_project_options harmonia_vulkan_sdk harmonia::low_level
    PRIVATE tomlplusplus::tomlplusplus
)

target_compile_definitions(harmonia_app
    PUBLIC HARMONIA_SHADER_DIR=\"${HARMONIA_SHADER_OUTPUT_DIR}\"
           VK_ENABLE_BETA_EXTENSIONS # propagate to consumers (Hyperion/Theia) — OMM types are beta-flagged
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
        # gtest v1.15.2 has an implicit char8_t->char32_t conversion in C++23;
        # suppress it - but only where the compiler actually KNOWS
        # -Wno-character-conversion (clang added that group after the runner
        # images' bundled clang; unknown -Wno-* options are silent until
        # -Werror, where they hard-fail - observed in CI). The check runs with
        # -Werror so it fails exactly like the real build would; a compiler
        # whose check fails also cannot emit the warning, so flag and warning
        # stay coupled (clang-cl 22 warns AND knows the flag).
        include(CheckCXXCompilerFlag)
        set(_harmonia_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
        check_cxx_compiler_flag("-Wno-character-conversion" HARMONIA_HAVE_WNO_CHARACTER_CONVERSION)
        set(CMAKE_REQUIRED_FLAGS "${_harmonia_saved_required_flags}")
        if(HARMONIA_HAVE_WNO_CHARACTER_CONVERSION)
            foreach(_gt_tgt IN ITEMS gtest gtest_main gmock gmock_main)
                if(TARGET ${_gt_tgt})
                    target_compile_options(${_gt_tgt} PRIVATE
                        $<$<CXX_COMPILER_ID:Clang,MSVC>:-Wno-character-conversion>
                    )
                endif()
            endforeach()
        endif()
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

# Static-analysis ctest lane: parallel clang-tidy over
# build/compile_commands.json, classified per the .clang-tidy WarningsAsErrors
# contract. Skips (77) when clang-tidy / Python3 / the database is unavailable.
if(PROJECT_IS_TOP_LEVEL)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_tidy
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_tidy.py)
        set_tests_properties(test_tidy PROPERTIES SKIP_RETURN_CODE 77 LABELS analysis RUN_SERIAL TRUE)
        # The architecture gate (LOCAL ONLY - not a CI gate): the engine and
        # the ARCHITECTURE.md contract are vendored in the repo, but the gate
        # runs from a per-user engine install (the paths below). Machines
        # without that install - e.g. CI runners - skip registration.
        set(CHECK_ARCH_ENGINE "")
        foreach(_cand "$ENV{USERPROFILE}/.config/opencode/skills/check-arch/scripts/check_arch.py"
                      "$ENV{HOME}/.config/opencode/skills/check-arch/scripts/check_arch.py")
          if(EXISTS "${_cand}")
            set(CHECK_ARCH_ENGINE "${_cand}")
            break()
          endif()
        endforeach()
        if(CHECK_ARCH_ENGINE)
            add_test(NAME test_arch
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_arch.py
                             --repo "${CMAKE_SOURCE_DIR}" --doc "${CMAKE_SOURCE_DIR}/ARCHITECTURE.md")
            set_tests_properties(test_arch PROPERTIES SKIP_RETURN_CODE 77 LABELS analysis RUN_SERIAL TRUE)
        endif()
    endif()
endif()

